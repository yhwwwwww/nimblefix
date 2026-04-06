#include "fastfix/profile/dictgen_input.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>


namespace fastfix::profile {

namespace {

constexpr char kDictgenCommentPrefix = '#';
constexpr char kDictgenAssignmentSeparator = '=';
constexpr char kDictgenEntrySeparator = '|';
constexpr char kDictgenFieldRuleSeparator = ',';
constexpr char kDictgenFieldRuleModeSeparator = ':';
constexpr std::string_view kProfileIdKey = "profile_id";
constexpr std::string_view kSchemaHashKey = "schema_hash";
constexpr std::string_view kFieldEntryKind = "field";
constexpr std::string_view kMessageEntryKind = "message";
constexpr std::string_view kGroupEntryKind = "group";

namespace field_columns {
constexpr std::size_t kKind = 0U;
constexpr std::size_t kTag = 1U;
constexpr std::size_t kName = 2U;
constexpr std::size_t kValueType = 3U;
constexpr std::size_t kFlags = 4U;
constexpr std::size_t kCount = 5U;
}  // namespace field_columns

namespace message_columns {
constexpr std::size_t kKind = 0U;
constexpr std::size_t kMsgType = 1U;
constexpr std::size_t kName = 2U;
constexpr std::size_t kFlags = 3U;
constexpr std::size_t kFieldRules = 4U;
constexpr std::size_t kCount = 5U;
}  // namespace message_columns

namespace group_columns {
constexpr std::size_t kKind = 0U;
constexpr std::size_t kCountTag = 1U;
constexpr std::size_t kDelimiterTag = 2U;
constexpr std::size_t kName = 3U;
constexpr std::size_t kFlags = 4U;
constexpr std::size_t kFieldRules = 5U;
constexpr std::size_t kCount = 6U;
}  // namespace group_columns

auto Trim(std::string_view input) -> std::string_view {
    std::size_t begin = 0;
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
        ++begin;
    }

    std::size_t end = input.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }

    return input.substr(begin, end - begin);
}

auto Split(std::string_view input, char delimiter) -> std::vector<std::string> {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= input.size()) {
        const auto end = input.find(delimiter, begin);
        if (end == std::string_view::npos) {
            parts.emplace_back(Trim(input.substr(begin)));
            break;
        }
        parts.emplace_back(Trim(input.substr(begin, end - begin)));
        begin = end + 1;
    }
    return parts;
}

auto ParseU32(std::string_view token) -> base::Result<std::uint32_t> {
    try {
        const auto value = std::stoull(std::string(token), nullptr, 0);
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return base::Status::InvalidArgument("value out of range for uint32");
        }
        return static_cast<std::uint32_t>(value);
    } catch (...) {
        return base::Status::InvalidArgument("invalid uint32 value: '" + std::string(token) + "'");
    }
}

auto ParseU64(std::string_view token) -> base::Result<std::uint64_t> {
    try {
        return std::stoull(std::string(token), nullptr, 0);
    } catch (...) {
        return base::Status::InvalidArgument("invalid uint64 value: '" + std::string(token) + "'");
    }
}

auto ParseValueType(std::string_view token) -> base::Result<ValueType> {
    std::string value(token);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (value == "string") {
        return ValueType::kString;
    }
    if (value == "int" || value == "length" || value == "numingroup") {
        return ValueType::kInt;
    }
    if (value == "char") {
        return ValueType::kChar;
    }
    if (value == "float" || value == "price" || value == "qty") {
        return ValueType::kFloat;
    }
    if (value == "boolean" || value == "bool") {
        return ValueType::kBoolean;
    }
    if (value == "timestamp" || value == "utctimestamp") {
        return ValueType::kTimestamp;
    }
    if (value == "unknown") {
        return ValueType::kUnknown;
    }

    return base::Status::InvalidArgument("unknown value type: '" + std::string(token) + "'");
}

auto ParseFieldRules(std::string_view token) -> base::Result<std::vector<FieldRule>> {
    std::vector<FieldRule> rules;
    const auto trimmed = Trim(token);
    if (trimmed.empty()) {
        return rules;
    }

    for (const auto& item : Split(trimmed, kDictgenFieldRuleSeparator)) {
        if (item.empty()) {
            continue;
        }
        const auto colon = item.find(kDictgenFieldRuleModeSeparator);
        if (colon == std::string::npos) {
            return base::Status::InvalidArgument("invalid field-rule token: '" + item + "'");
        }

        auto tag = ParseU32(item.substr(0, colon));
        if (!tag.ok()) {
            return tag.status();
        }

        FieldRule rule;
        rule.tag = tag.value();
        const auto mode = Trim(item.substr(colon + 1));
        if (mode == "r" || mode == "R") {
            rule.flags = static_cast<std::uint32_t>(FieldRuleFlags::kRequired);
        } else if (mode == "o" || mode == "O") {
            rule.flags = static_cast<std::uint32_t>(FieldRuleFlags::kNone);
        } else {
            return base::Status::InvalidArgument("unknown field-rule mode: '" + std::string(mode) + "'");
        }
        rules.push_back(rule);
    }

    return rules;
}

auto ParseFieldParts(const std::vector<std::string>& parts) -> base::Result<FieldDef> {
    if (parts.size() != field_columns::kCount) {
        return base::Status::InvalidArgument("field entries must have 5 parts");
    }

    auto tag = ParseU32(parts[field_columns::kTag]);
    if (!tag.ok()) {
        return tag.status();
    }
    auto value_type = ParseValueType(parts[field_columns::kValueType]);
    if (!value_type.ok()) {
        return value_type.status();
    }
    auto flags = ParseU32(parts[field_columns::kFlags]);
    if (!flags.ok()) {
        return flags.status();
    }

    FieldDef field;
    field.tag = tag.value();
    field.name = parts[field_columns::kName];
    field.value_type = value_type.value();
    field.flags = flags.value();
    return field;
}

auto ParseMessageParts(const std::vector<std::string>& parts) -> base::Result<MessageDef> {
    if (parts.size() != message_columns::kCount) {
        return base::Status::InvalidArgument("message entries must have 5 parts");
    }

    auto rules = ParseFieldRules(parts[message_columns::kFieldRules]);
    if (!rules.ok()) {
        return rules.status();
    }
    auto flags = ParseU32(parts[message_columns::kFlags]);
    if (!flags.ok()) {
        return flags.status();
    }

    MessageDef message;
    message.msg_type = parts[message_columns::kMsgType];
    message.name = parts[message_columns::kName];
    message.flags = flags.value();
    message.field_rules = std::move(rules).value();
    return message;
}

auto ParseGroupParts(const std::vector<std::string>& parts) -> base::Result<GroupDef> {
    if (parts.size() != group_columns::kCount) {
        return base::Status::InvalidArgument("group entries must have 6 parts");
    }

    auto count_tag = ParseU32(parts[group_columns::kCountTag]);
    if (!count_tag.ok()) {
        return count_tag.status();
    }
    auto delimiter_tag = ParseU32(parts[group_columns::kDelimiterTag]);
    if (!delimiter_tag.ok()) {
        return delimiter_tag.status();
    }
    auto flags = ParseU32(parts[group_columns::kFlags]);
    if (!flags.ok()) {
        return flags.status();
    }
    auto rules = ParseFieldRules(parts[group_columns::kFieldRules]);
    if (!rules.ok()) {
        return rules.status();
    }

    GroupDef group;
    group.count_tag = count_tag.value();
    group.delimiter_tag = delimiter_tag.value();
    group.name = parts[group_columns::kName];
    group.flags = flags.value();
    group.field_rules = std::move(rules).value();
    return group;
}

auto LoadFileLines(const std::filesystem::path& path) -> base::Result<std::vector<std::string>> {
    std::ifstream in(path);
    if (!in.is_open()) {
        return base::Status::IoError("unable to open dictgen input: '" + path.string() + "'");
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

auto SplitTextLines(std::string_view text) -> std::vector<std::string> {
    std::vector<std::string> lines;
    std::string current;
    current.reserve(text.size());
    for (const auto ch : text) {
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            lines.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty() || text.empty()) {
        lines.push_back(std::move(current));
    }
    return lines;
}

auto ParseDictionaryLines(const std::vector<std::string>& lines)
    -> base::Result<NormalizedDictionary> {
    NormalizedDictionary dictionary;
    for (const auto& raw_line : lines) {
        const auto trimmed = Trim(raw_line);
        if (trimmed.empty() || trimmed.starts_with(kDictgenCommentPrefix)) {
            continue;
        }

        if (trimmed.find(kDictgenEntrySeparator) == std::string_view::npos) {
            const auto eq = trimmed.find(kDictgenAssignmentSeparator);
            if (eq == std::string_view::npos) {
                return base::Status::InvalidArgument("invalid dictgen line: '" + std::string(trimmed) + "'");
            }

            const auto key = Trim(trimmed.substr(0, eq));
            const auto value = Trim(trimmed.substr(eq + 1));
            if (key == kProfileIdKey) {
                auto parsed = ParseU64(value);
                if (!parsed.ok()) {
                    return parsed.status();
                }
                dictionary.profile_id = parsed.value();
            } else if (key == kSchemaHashKey) {
                auto parsed = ParseU64(value);
                if (!parsed.ok()) {
                    return parsed.status();
                }
                dictionary.schema_hash = parsed.value();
            } else {
                return base::Status::InvalidArgument("unknown dictgen key: '" + std::string(key) + "'");
            }
            continue;
        }

        const auto parts = Split(trimmed, kDictgenEntrySeparator);
        if (parts.empty()) {
            continue;
        }

        if (parts[field_columns::kKind] == kFieldEntryKind) {
            auto field = ParseFieldParts(parts);
            if (!field.ok()) {
                return field.status();
            }
            dictionary.fields.push_back(std::move(field).value());
            continue;
        }

        if (parts[message_columns::kKind] == kMessageEntryKind) {
            auto message = ParseMessageParts(parts);
            if (!message.ok()) {
                return message.status();
            }
            dictionary.messages.push_back(std::move(message).value());
            continue;
        }

        if (parts[group_columns::kKind] == kGroupEntryKind) {
            auto group = ParseGroupParts(parts);
            if (!group.ok()) {
                return group.status();
            }
            dictionary.groups.push_back(std::move(group).value());
            continue;
        }

        return base::Status::InvalidArgument("unknown dictgen entry kind: '" + parts[field_columns::kKind] + "'");
    }

    return dictionary;
}

auto ParseDictionary(const std::filesystem::path& path)
    -> base::Result<NormalizedDictionary> {
    auto lines = LoadFileLines(path);
    if (!lines.ok()) {
        return lines.status();
    }

    return ParseDictionaryLines(lines.value());
}

}  // namespace

auto LoadNormalizedDictionaryText(std::string_view text)
    -> base::Result<NormalizedDictionary> {
    const auto lines = SplitTextLines(text);
    return ParseDictionaryLines(lines);
}

auto LoadNormalizedDictionaryFile(const std::filesystem::path& path)
    -> base::Result<NormalizedDictionary> {
    return ParseDictionary(path);
}

}  // namespace fastfix::profile
