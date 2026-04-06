#include "fastfix/codec/fix_codec.h"
#include "fastfix/codec/simd_scan.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <tuple>

namespace fastfix::codec {

namespace {

inline constexpr std::size_t kTokenInlineCapacity = 16U;

template <typename Integer>
inline constexpr std::size_t kIntegerTextBufferBytes =
    static_cast<std::size_t>(std::numeric_limits<Integer>::digits10) + 3U;

struct Token {
    std::uint32_t tag{0};
    std::string_view value;
    std::size_t start_offset{0};
    std::size_t end_offset{0};
    std::size_t value_offset{0};
    std::size_t value_length{0};
};

using TokenList = base::InlineSplitVector<Token, kTokenInlineCapacity>;

struct SeenTagBuffer {
    static constexpr std::size_t kInlineCapacity = 64U;

    auto contains(std::uint32_t tag) const -> bool {
        const auto inline_end = inline_tags.begin() + static_cast<std::ptrdiff_t>(inline_size);
        return std::find(inline_tags.begin(), inline_end, tag) != inline_end ||
               std::find(overflow_tags.begin(), overflow_tags.end(), tag) != overflow_tags.end();
    }

    auto push_back(std::uint32_t tag) -> void {
        if (inline_size < inline_tags.size()) {
            inline_tags[inline_size++] = tag;
            return;
        }
        overflow_tags.push_back(tag);
    }

    auto reserve(std::size_t count) -> void {
        if (count > inline_tags.size()) {
            overflow_tags.reserve(count - inline_tags.size());
        }
    }

    auto clear() -> void {
        inline_size = 0U;
        overflow_tags.clear();
    }

    std::array<std::uint32_t, kInlineCapacity> inline_tags{};
    std::size_t inline_size{0U};
    std::vector<std::uint32_t> overflow_tags;
};

struct ParsedContainerRef {
    bool root{false};
    std::uint32_t entry_index{message::kInvalidParsedIndex};
};

struct ScopeValidationState {
    SeenTagBuffer seen_tags;
    int last_rule_index{-1};
};

struct CompiledScopeTemplate;

struct CompiledScopeStep {
    enum class Kind : std::uint8_t {
        kScalar,
        kGroup,
    };

    Kind kind{Kind::kScalar};
    std::uint32_t tag{0};
    std::string prefix;
    std::shared_ptr<const CompiledScopeTemplate> group_scope;
};

struct CompiledScopeTemplate {
    std::vector<CompiledScopeStep> steps;
    std::vector<std::uint32_t> scalar_tags;
    std::vector<std::uint32_t> group_tags;
};

struct TemplateCacheKey {
    std::uintptr_t dictionary_identity{0};
    std::uint64_t schema_hash{0};
    std::uint64_t profile_id{0};
    std::string msg_type;
    std::string begin_string;
    std::string sender_comp_id;
    std::string target_comp_id;
    std::string default_appl_ver_id;
    char delimiter{kFixSoh};

    [[nodiscard]] auto operator<(const TemplateCacheKey& other) const -> bool {
        return std::tie(
                   dictionary_identity,
                   schema_hash,
                   profile_id,
                   msg_type,
                   begin_string,
                   sender_comp_id,
                   target_comp_id,
                   default_appl_ver_id,
                   delimiter) <
            std::tie(
                     other.dictionary_identity,
                   other.schema_hash,
                   other.profile_id,
                   other.msg_type,
                   other.begin_string,
                   other.sender_comp_id,
                   other.target_comp_id,
                   other.default_appl_ver_id,
                   other.delimiter);
    }
};

auto IsStandardSessionField(std::uint32_t tag) -> bool {
    switch (tag) {
        case 34U:
        case 35U:
        case 43U:
        case 49U:
        case 52U:
        case 56U:
        case 97U:
        case 122U:
        case 1137U:
            return true;
        default:
            return false;
    }
}

auto IsImplicitStandardField(std::uint32_t tag) -> bool {
    switch (tag) {
        case 7U:
        case 16U:
        case 34U:
        case 35U:
        case 36U:
        case 43U:
        case 45U:
        case 49U:
        case 52U:
        case 56U:
        case 58U:
        case 97U:
        case 98U:
        case 108U:
        case 112U:
        case 123U:
        case 141U:
        case 371U:
        case 372U:
        case 373U:
        case 122U:
        case 1137U:
            return true;
        default:
            return false;
    }
}

auto SetValidationIssue(
    ValidationIssue* issue,
    ValidationIssueKind kind,
    std::uint32_t tag,
    std::string text) -> void {
    if (issue == nullptr || issue->present()) {
        return;
    }
    issue->kind = kind;
    issue->tag = tag;
    issue->text = std::move(text);
}

auto HasSeenTag(const ScopeValidationState& state, std::uint32_t tag) -> bool {
    return state.seen_tags.contains(tag);
}

auto CountGroupRules(
    const profile::NormalizedDictionaryView& dictionary,
    std::span<const profile::FieldRuleRecord> rules) -> std::size_t {
    std::size_t count = 0U;
    for (const auto& rule : rules) {
        if (dictionary.find_group(rule.tag) != nullptr) {
            ++count;
        }
    }
    return count;
}

template <typename RuleSpan>
auto ValidateConsumedField(
    const profile::NormalizedDictionaryView& dictionary,
    std::uint32_t tag,
    RuleSpan rules,
    int rule_index,
    ScopeValidationState* state,
    ValidationIssue* issue,
    bool allow_standard_session_fields,
    bool schema_known,
    const char* scope_name) -> void {
    if (state == nullptr) {
        return;
    }

    if (HasSeenTag(*state, tag)) {
        SetValidationIssue(
            issue,
            ValidationIssueKind::kDuplicateField,
            tag,
            "field " + std::to_string(tag) + " appears more than once");
        return;
    }
    state->seen_tags.push_back(tag);

    if (allow_standard_session_fields && IsStandardSessionField(tag)) {
        return;
    }
    if (!schema_known) {
        return;
    }
    if (dictionary.find_field(tag) == nullptr && !(rules.empty() && IsImplicitStandardField(tag))) {
        SetValidationIssue(
            issue,
            ValidationIssueKind::kUnknownField,
            tag,
            "field " + std::to_string(tag) + " is not present in the bound dictionary");
        return;
    }

    if (!rules.empty()) {
        if (rule_index < 0) {
            SetValidationIssue(
                issue,
                ValidationIssueKind::kFieldNotAllowed,
                tag,
                "field " + std::to_string(tag) + " is not allowed by the bound " + std::string(scope_name) +
                    " definition");
            return;
        }
        if (rule_index < state->last_rule_index) {
            SetValidationIssue(
                issue,
                ValidationIssueKind::kFieldOutOfOrder,
                tag,
                "field " + std::to_string(tag) + " is out of order for the bound " + std::string(scope_name) +
                    " definition");
            return;
        }
        state->last_rule_index = rule_index;
    }
}

auto ParseUnsigned(std::string_view text, const char* label) -> base::Result<std::uint32_t> {
    std::uint32_t value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc() || ptr != end) {
        return base::Status::InvalidArgument(std::string("invalid ") + label);
    }
    return value;
}

auto ParseSigned(std::string_view text, const char* label) -> base::Result<std::int64_t> {
    std::int64_t value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc() || ptr != end) {
        return base::Status::InvalidArgument(std::string("invalid ") + label);
    }
    return value;
}

auto ParseDouble(std::string_view text, const char* label) -> base::Result<double> {
    double value = 0.0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc() || ptr != end) {
        return base::Status::InvalidArgument(std::string("invalid ") + label);
    }
    return value;
}

auto ParseBoolean(std::string_view text, const char* label) -> base::Result<bool> {
    if (text == "Y") {
        return true;
    }
    if (text == "N") {
        return false;
    }
    return base::Status::InvalidArgument(std::string("invalid ") + label);
}

auto ResolveFieldType(
    std::uint32_t tag,
    const profile::NormalizedDictionaryView& dictionary) -> message::FieldValueType {
    if (const auto* field = dictionary.find_field(tag); field != nullptr) {
        switch (static_cast<profile::ValueType>(field->value_type)) {
            case profile::ValueType::kInt:
                return message::FieldValueType::kInt;
            case profile::ValueType::kChar:
                return message::FieldValueType::kChar;
            case profile::ValueType::kFloat:
                return message::FieldValueType::kFloat;
            case profile::ValueType::kBoolean:
                return message::FieldValueType::kBoolean;
            default:
                return message::FieldValueType::kString;
        }
    }

    switch (tag) {
        case 9:
        case 34:
        case 45:
        case 371:
        case 373:
        case 98:
        case 108:
        case 7:
        case 16:
        case 36:
            return message::FieldValueType::kInt;
        case 141:
        case 43:
        case 97:
        case 123:
            return message::FieldValueType::kBoolean;
        default:
            return message::FieldValueType::kString;
    }
}

template <typename Builder>
auto ApplyScalarField(
    Builder& builder,
    std::uint32_t tag,
    std::string_view value,
    const profile::NormalizedDictionaryView& dictionary) -> base::Status {
    switch (ResolveFieldType(tag, dictionary)) {
        case message::FieldValueType::kInt: {
            auto parsed = ParseSigned(value, "integer field");
            if (!parsed.ok()) {
                return parsed.status();
            }
            builder.set_int(tag, parsed.value());
            return base::Status::Ok();
        }
        case message::FieldValueType::kChar:
            if (value.size() != 1U) {
                return base::Status::InvalidArgument("char field must have exactly one byte");
            }
            builder.set_char(tag, value.front());
            return base::Status::Ok();
        case message::FieldValueType::kFloat: {
            auto parsed = ParseDouble(value, "floating-point field");
            if (!parsed.ok()) {
                return parsed.status();
            }
            builder.set_float(tag, parsed.value());
            return base::Status::Ok();
        }
        case message::FieldValueType::kBoolean: {
            auto parsed = ParseBoolean(value, "boolean field");
            if (!parsed.ok()) {
                return parsed.status();
            }
            builder.set_boolean(tag, parsed.value());
            return base::Status::Ok();
        }
        default:
            builder.set_string(tag, value);
            return base::Status::Ok();
    }
}

auto RootContainer() -> ParsedContainerRef {
    return ParsedContainerRef{.root = true};
}

auto EntryContainer(std::uint32_t entry_index) -> ParsedContainerRef {
    return ParsedContainerRef{.root = false, .entry_index = entry_index};
}

auto ResolveContainer(const message::ParsedMessageData& parsed, ParsedContainerRef ref)
    -> const message::ParsedEntryData& {
    return ref.root ? parsed.root : parsed.entries[ref.entry_index];
}

auto ResolveContainer(message::ParsedMessageData* parsed, ParsedContainerRef ref)
    -> message::ParsedEntryData& {
    return ref.root ? parsed->root : parsed->entries[ref.entry_index];
}

auto FindParsedFieldIndex(
    const message::ParsedMessageData& parsed,
    ParsedContainerRef container,
    std::uint32_t tag) -> std::uint32_t {
    auto field_index = ResolveContainer(parsed, container).first_field;
    while (field_index != message::kInvalidParsedIndex) {
        if (parsed.field_slots[field_index].tag == tag) {
            return field_index;
        }
        field_index = parsed.field_slots[field_index].next_field;
    }
    return message::kInvalidParsedIndex;
}

auto AppendParsedFieldSlot(
    message::ParsedMessageData* parsed,
    ParsedContainerRef container,
    message::ParsedFieldSlot slot) -> void {
    auto existing = FindParsedFieldIndex(*parsed, container, slot.tag);
    if (existing != message::kInvalidParsedIndex) {
        slot.next_field = parsed->field_slots[existing].next_field;
        parsed->field_slots[existing] = std::move(slot);
        return;
    }

    const auto new_index = static_cast<std::uint32_t>(parsed->field_slots.size());
    parsed->field_slots.push_back(std::move(slot));

    auto& entry = ResolveContainer(parsed, container);
    if (entry.first_field == message::kInvalidParsedIndex) {
        entry.first_field = new_index;
    } else {
        auto tail = entry.first_field;
        while (parsed->field_slots[tail].next_field != message::kInvalidParsedIndex) {
            tail = parsed->field_slots[tail].next_field;
        }
        parsed->field_slots[tail].next_field = new_index;
    }
    ++entry.field_count;
}

auto MakeParsedFieldSlot(
    std::span<const std::byte> bytes,
    const Token& token,
    const profile::NormalizedDictionaryView& dictionary) -> base::Result<message::ParsedFieldSlot> {
    message::ParsedFieldSlot slot;
    slot.tag = token.tag;
    slot.type = ResolveFieldType(token.tag, dictionary);
    slot.value_offset = static_cast<std::uint32_t>(token.value_offset);
    slot.value_length = static_cast<std::uint16_t>(token.value_length);
    (void)bytes;
    return slot;
}

auto FindParsedGroupIndex(
    const message::ParsedMessageData& parsed,
    ParsedContainerRef container,
    std::uint32_t count_tag) -> std::uint32_t {
    auto group_index = ResolveContainer(parsed, container).first_group;
    while (group_index != message::kInvalidParsedIndex) {
        if (parsed.groups[group_index].count_tag == count_tag) {
            return group_index;
        }
        group_index = parsed.groups[group_index].next_group;
    }
    return message::kInvalidParsedIndex;
}

auto EnsureParsedGroup(
    message::ParsedMessageData* parsed,
    ParsedContainerRef container,
    std::uint32_t count_tag,
    std::uint16_t depth) -> std::uint32_t {
    auto existing = FindParsedGroupIndex(*parsed, container, count_tag);
    if (existing != message::kInvalidParsedIndex) {
        return existing;
    }

    const auto new_index = static_cast<std::uint32_t>(parsed->groups.size());
    parsed->groups.push_back(message::ParsedGroupFrame{
        .count_tag = count_tag,
        .first_entry = message::kInvalidParsedIndex,
        .entry_count = 0U,
        .depth = depth,
        .next_group = message::kInvalidParsedIndex,
    });

    auto& entry = ResolveContainer(parsed, container);
    if (entry.first_group == message::kInvalidParsedIndex) {
        entry.first_group = new_index;
    } else {
        auto tail = entry.first_group;
        while (parsed->groups[tail].next_group != message::kInvalidParsedIndex) {
            tail = parsed->groups[tail].next_group;
        }
        parsed->groups[tail].next_group = new_index;
    }
    ++entry.group_count;
    return new_index;
}

auto AppendParsedEntry(message::ParsedMessageData* parsed, std::uint32_t group_index) -> std::uint32_t {
    const auto new_index = static_cast<std::uint32_t>(parsed->entries.size());
    parsed->entries.push_back(message::ParsedEntryData{});

    auto& group = parsed->groups[group_index];
    if (group.first_entry == message::kInvalidParsedIndex) {
        group.first_entry = new_index;
    } else {
        auto tail = group.first_entry;
        while (parsed->entries[tail].next_entry != message::kInvalidParsedIndex) {
            tail = parsed->entries[tail].next_entry;
        }
        parsed->entries[tail].next_entry = new_index;
    }
    ++group.entry_count;
    return new_index;
}

auto ParseParsedGroupEntries(
    const profile::NormalizedDictionaryView& dictionary,
    std::span<const std::byte> bytes,
    const TokenList& tokens,
    std::size_t index,
    const profile::GroupDefRecord& group_def,
    message::ParsedMessageData* parsed,
    ParsedContainerRef parent,
    std::uint16_t depth,
    ValidationIssue* validation_issue) -> base::Result<std::size_t> {
    auto count = ParseUnsigned(tokens[index].value, "group count");
    if (!count.ok()) {
        return count.status();
    }
    const auto frame_index = EnsureParsedGroup(parsed, parent, group_def.count_tag, depth);
    const auto group_rules = dictionary.group_field_rules(group_def);
    parsed->entries.reserve(parsed->entries.size() + count.value());
    ++index;

    for (std::uint32_t entry_index = 0; entry_index < count.value(); ++entry_index) {
        ScopeValidationState validation_state;
        validation_state.seen_tags.reserve(group_rules.size());
        if (index >= tokens.size() || tokens[index].tag != group_def.delimiter_tag) {
            SetValidationIssue(
                validation_issue,
                ValidationIssueKind::kIncorrectNumInGroupCount,
                group_def.count_tag,
                "group " + std::to_string(group_def.count_tag) + " expected delimiter tag " +
                    std::to_string(group_def.delimiter_tag) + " for entry " +
                    std::to_string(entry_index + 1U));
            return index;
        }

        const auto parsed_entry_index = AppendParsedEntry(parsed, frame_index);
        const auto entry_ref = EntryContainer(parsed_entry_index);
        bool seen_any_field = false;
        while (index < tokens.size()) {
            const auto tag = tokens[index].tag;
            if (seen_any_field && tag == group_def.delimiter_tag) {
                break;
            }

            if (const auto ri = dictionary.group_rule_index(group_def, tag); ri >= 0) {
                ValidateConsumedField(
                    dictionary,
                    tag,
                    group_rules,
                    ri,
                    &validation_state,
                    validation_issue,
                    false,
                    true,
                    "group");

                auto slot = MakeParsedFieldSlot(bytes, tokens[index], dictionary);
                if (!slot.ok()) {
                    return slot.status();
                }
                AppendParsedFieldSlot(parsed, entry_ref, std::move(slot).value());

                if (const auto* nested_group = dictionary.find_group(tag); nested_group != nullptr) {
                    auto next_index = ParseParsedGroupEntries(
                        dictionary,
                        bytes,
                        tokens,
                        index,
                        *nested_group,
                        parsed,
                        entry_ref,
                        static_cast<std::uint16_t>(depth + 1U),
                        validation_issue);
                    if (!next_index.ok()) {
                        return next_index.status();
                    }
                    index = next_index.value();
                    seen_any_field = true;
                    continue;
                }

                ++index;
                seen_any_field = true;
                continue;
            }

            break;
        }
    }

    return index;
}

auto Tokenize(std::span<const std::byte> bytes, char delimiter) -> base::Result<TokenList> {
    TokenList tokens;
    const auto delimiter_byte = static_cast<std::byte>(static_cast<unsigned char>(delimiter));
    const auto equals_byte = static_cast<std::byte>('=');
    tokens.reserve(std::max<std::size_t>(32U, bytes.size() / 8U));
    std::size_t field_start = 0;
    while (field_start < bytes.size()) {
        const auto remaining = bytes.size() - field_start;
        const auto* soh_ptr = FindByte(bytes.data() + field_start, remaining, delimiter_byte);
        const auto index = static_cast<std::size_t>(soh_ptr - bytes.data());

        if (index >= bytes.size()) {
            break;
        }

        if (index == field_start) {
            return base::Status::FormatError("empty FIX field is not allowed");
        }

        const auto field_len = index - field_start;
        const auto* eq_ptr = FindByte(bytes.data() + field_start, field_len, equals_byte);
        const auto equals = static_cast<std::size_t>(eq_ptr - (bytes.data() + field_start));
        if (equals >= field_len || equals == 0U || equals == field_len - 1U) {
            return base::Status::FormatError("invalid FIX field syntax");
        }

        const auto* field_bytes = reinterpret_cast<const char*>(bytes.data() + field_start);
        auto tag = ParseUnsigned(std::string_view(field_bytes, equals), "field tag");
        if (!tag.ok()) {
            return tag.status();
        }

        const auto value_start = field_start + equals + 1U;
        const auto value_len = field_len - equals - 1U;
        tokens.push_back(Token{
            .tag = tag.value(),
            .value = std::string_view(reinterpret_cast<const char*>(bytes.data() + value_start), value_len),
            .start_offset = field_start,
            .end_offset = index + 1U,
            .value_offset = value_start,
            .value_length = value_len,
        });
        field_start = index + 1U;
    }

    if (field_start != bytes.size()) {
        return base::Status::FormatError("FIX frame is missing its final delimiter");
    }

    if (tokens.empty()) {
        return base::Status::FormatError("FIX frame has no fields");
    }
    return tokens;
}

auto ParseGroupEntries(
    const profile::NormalizedDictionaryView& dictionary,
    const TokenList& tokens,
    std::size_t index,
    const profile::GroupDefRecord& group_def,
    const std::function<void(std::size_t)>& reserve_group_entries,
    const std::function<message::GroupEntryBuilder()>& add_group_entry,
    ValidationIssue* validation_issue) -> base::Result<std::size_t> {
    auto count = ParseUnsigned(tokens[index].value, "group count");
    if (!count.ok()) {
        return count.status();
    }
    const auto group_rules = dictionary.group_field_rules(group_def);
    if (reserve_group_entries) {
        reserve_group_entries(count.value());
    }
    ++index;

    for (std::uint32_t entry_index = 0; entry_index < count.value(); ++entry_index) {
        ScopeValidationState validation_state;
        validation_state.seen_tags.reserve(group_rules.size());
        if (index >= tokens.size() || tokens[index].tag != group_def.delimiter_tag) {
            SetValidationIssue(
                validation_issue,
                ValidationIssueKind::kIncorrectNumInGroupCount,
                group_def.count_tag,
                "group " + std::to_string(group_def.count_tag) + " expected delimiter tag " +
                    std::to_string(group_def.delimiter_tag) + " for entry " +
                    std::to_string(entry_index + 1U));
            return index;
        }

        auto entry = add_group_entry();
        entry.reserve_fields(group_rules.size());
        entry.reserve_groups(CountGroupRules(dictionary, group_rules));
        bool seen_any_field = false;
        while (index < tokens.size()) {
            const auto tag = tokens[index].tag;
            if (seen_any_field && tag == group_def.delimiter_tag) {
                break;
            }

            if (const auto ri = dictionary.group_rule_index(group_def, tag); ri >= 0) {
                ValidateConsumedField(
                    dictionary,
                    tag,
                    group_rules,
                    ri,
                    &validation_state,
                    validation_issue,
                    false,
                    true,
                    "group");
                if (const auto* nested_group = dictionary.find_group(tag); nested_group != nullptr) {
                    auto status = ApplyScalarField(entry, tag, tokens[index].value, dictionary);
                    if (!status.ok()) {
                        return status;
                    }
                    auto next_index = ParseGroupEntries(
                        dictionary,
                        tokens,
                        index,
                        *nested_group,
                        [&entry, tag](std::size_t count) { entry.reserve_group_entries(tag, count); },
                        [&entry, tag]() { return entry.add_group_entry(tag); },
                        validation_issue);
                    if (!next_index.ok()) {
                        return next_index.status();
                    }
                    index = next_index.value();
                    seen_any_field = true;
                    continue;
                }

                auto status = ApplyScalarField(entry, tag, tokens[index].value, dictionary);
                if (!status.ok()) {
                    return status;
                }
                ++index;
                seen_any_field = true;
                continue;
            }

            break;
        }
    }

    return index;
}

auto ShouldSkipField(std::uint32_t tag) -> bool {
    return tag == 8U || tag == 9U || tag == 10U;
}

auto IsTemplateManagedHeaderField(std::uint32_t tag) -> bool {
    switch (tag) {
        case 34U:
        case 35U:
        case 43U:
        case 49U:
        case 52U:
        case 56U:
            return true;
        default:
            return false;
    }
}

auto ContainsTag(const std::vector<std::uint32_t>& tags, std::uint32_t tag) -> bool {
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

auto MakeTagPrefix(std::uint32_t tag) -> std::string {
    std::string prefix = std::to_string(tag);
    prefix.push_back('=');
    return prefix;
}

auto MakeFixedFieldFragment(std::uint32_t tag, std::string_view value, char delimiter) -> std::string {
    std::string fragment = MakeTagPrefix(tag);
    fragment.append(value);
    fragment.push_back(delimiter);
    return fragment;
}

auto AccumulateChecksum(std::string_view text, std::uint32_t* checksum) -> void {
    if (checksum == nullptr) {
        return;
    }
    for (const auto ch : text) {
        *checksum += static_cast<unsigned char>(ch);
    }
}

auto AccumulateAppendedRange(const std::string& out, std::size_t start_offset, std::uint32_t* checksum) -> void {
    if (checksum == nullptr || start_offset >= out.size()) {
        return;
    }
    AccumulateChecksum(std::string_view(out.data() + start_offset, out.size() - start_offset), checksum);
}

auto AppendTracked(std::string& out, std::uint32_t* checksum, std::string_view text) -> void {
    out.append(text);
    AccumulateChecksum(text, checksum);
}

auto AppendTracked(std::string& out, std::uint32_t* checksum, char value) -> void {
    out.push_back(value);
    if (checksum != nullptr) {
        *checksum += static_cast<unsigned char>(value);
    }
}

template <typename Integer>
auto AppendIntegerDigits(std::string& out, std::uint32_t* checksum, Integer value) -> void {
    std::array<char, kIntegerTextBufferBytes<Integer>> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc()) {
        return;
    }
    AppendTracked(out, checksum, std::string_view(buffer.data(), static_cast<std::size_t>(ptr - buffer.data())));
}

auto AppendPrefixedStringField(
    std::string& out,
    std::uint32_t* checksum,
    std::string_view prefix,
    std::string_view value,
    char delimiter) -> void {
    AppendTracked(out, checksum, prefix);
    AppendTracked(out, checksum, value);
    AppendTracked(out, checksum, delimiter);
}

auto AppendPrefixedCountField(
    std::string& out,
    std::uint32_t* checksum,
    std::string_view prefix,
    std::uint32_t value,
    char delimiter) -> void {
    AppendTracked(out, checksum, prefix);
    AppendIntegerDigits(out, checksum, value);
    AppendTracked(out, checksum, delimiter);
}

auto AppendField(std::string& out, std::uint32_t tag, std::string_view value, char delimiter) -> void {
    std::array<char, kIntegerTextBufferBytes<std::uint32_t>> tag_buf{};
    const auto [tag_ptr, tag_ec] = std::to_chars(tag_buf.data(), tag_buf.data() + tag_buf.size(), tag);
    if (tag_ec != std::errc()) {
        return;
    }
    out.append(tag_buf.data(), static_cast<std::size_t>(tag_ptr - tag_buf.data()));
    out.push_back('=');
    out.append(value);
    out.push_back(delimiter);
}

auto AppendField(std::string& out, std::uint32_t tag, std::int64_t value, char delimiter) -> void {
    std::array<char, kIntegerTextBufferBytes<std::int64_t>> val_buf{};
    const auto [val_ptr, val_ec] = std::to_chars(val_buf.data(), val_buf.data() + val_buf.size(), value);
    if (val_ec != std::errc()) {
        return;
    }
    AppendField(out, tag, std::string_view(val_buf.data(), static_cast<std::size_t>(val_ptr - val_buf.data())), delimiter);
}

auto AppendField(std::string& out, std::uint32_t tag, double value, char delimiter) -> void {
    std::array<char, 32> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general, 12);
    if (ec != std::errc()) {
        return;
    }
    AppendField(out, tag, std::string_view(buffer.data(), static_cast<std::size_t>(ptr - buffer.data())), delimiter);
}

auto AppendField(std::string& out, std::uint32_t tag, bool value, char delimiter) -> void {
    AppendField(out, tag, std::string_view(value ? "Y" : "N"), delimiter);
}

auto AppendPrefixedFieldValue(
    std::string& out,
    std::uint32_t* checksum,
    std::string_view prefix,
    const message::FieldView& field,
    char delimiter) -> void {
    AppendTracked(out, checksum, prefix);
    switch (field.type) {
        case message::FieldValueType::kInt:
            AppendIntegerDigits(out, checksum, field.int_value);
            break;
        case message::FieldValueType::kChar:
            AppendTracked(out, checksum, field.char_value);
            break;
        case message::FieldValueType::kFloat: {
            std::array<char, 32> float_buf{};
            const auto [fptr, fec] = std::to_chars(float_buf.data(), float_buf.data() + float_buf.size(), field.float_value, std::chars_format::general, 12);
            if (fec == std::errc()) {
                AppendTracked(out, checksum, std::string_view(float_buf.data(), static_cast<std::size_t>(fptr - float_buf.data())));
            }
            break;
        }
        case message::FieldValueType::kBoolean:
            AppendTracked(out, checksum, std::string_view(field.bool_value ? "Y" : "N"));
            break;
        default:
            AppendTracked(out, checksum, field.string_value);
            break;
    }
    AppendTracked(out, checksum, delimiter);
}

auto EncodeFieldValue(std::string& out, const message::FieldValue& field, char delimiter) -> void {
    switch (field.type) {
        case message::FieldValueType::kInt:
            return AppendField(out, field.tag, field.int_value, delimiter);
        case message::FieldValueType::kChar:
            return AppendField(out, field.tag, std::string_view(&field.char_value, 1U), delimiter);
        case message::FieldValueType::kFloat:
            return AppendField(out, field.tag, field.float_value, delimiter);
        case message::FieldValueType::kBoolean:
            return AppendField(out, field.tag, field.bool_value, delimiter);
        default:
            return AppendField(out, field.tag, field.string_value, delimiter);
    }
}

auto EncodeFieldValue(std::string& out, const message::FieldView& field, char delimiter) -> void {
    switch (field.type) {
        case message::FieldValueType::kInt:
            return AppendField(out, field.tag, field.int_value, delimiter);
        case message::FieldValueType::kChar:
            return AppendField(out, field.tag, std::string_view(&field.char_value, 1U), delimiter);
        case message::FieldValueType::kFloat:
            return AppendField(out, field.tag, field.float_value, delimiter);
        case message::FieldValueType::kBoolean:
            return AppendField(out, field.tag, field.bool_value, delimiter);
        default:
            return AppendField(out, field.tag, field.string_value, delimiter);
    }
}

auto EncodeGroups(std::string& out, const std::vector<message::GroupData>& groups, char delimiter) -> void;
auto EncodeMessageBody(
    std::string& out,
    const message::MessageData& data,
    char delimiter,
    bool skip_standard_header) -> void;

auto EncodeGroupData(std::string& out, const message::GroupData& group, char delimiter) -> void {
    AppendField(out, group.count_tag, static_cast<std::int64_t>(group.entries.size()), delimiter);
    for (const auto& entry : group.entries) {
        EncodeMessageBody(out, entry, delimiter, false);
    }
}

auto AppendTrackedGenericField(
    std::string& out,
    std::uint32_t* checksum,
    const message::FieldView& field,
    char delimiter) -> void {
    const auto start_offset = out.size();
    EncodeFieldValue(out, field, delimiter);
    AccumulateAppendedRange(out, start_offset, checksum);
}

auto EncodeGroupData(std::string& out, message::GroupView group, char delimiter) -> void;
auto EncodeMessageBody(
    std::string& out,
    message::MessageView view,
    char delimiter,
    bool skip_standard_header) -> void;

auto AppendTrackedGenericGroup(
    std::string& out,
    std::uint32_t* checksum,
    message::GroupView group,
    char delimiter) -> void {
    const auto start_offset = out.size();
    EncodeGroupData(out, group, delimiter);
    AccumulateAppendedRange(out, start_offset, checksum);
}

auto HasGroupCountTag(const std::vector<message::GroupData>& groups, std::uint32_t tag) -> bool {
    return std::any_of(groups.begin(), groups.end(), [&](const auto& group) { return group.count_tag == tag; });
}

auto HasGroupCountTag(message::MessageView view, std::uint32_t tag) -> bool {
    for (std::size_t index = 0; index < view.group_count(); ++index) {
        const auto group = view.group_at(index);
        if (group.has_value() && group->count_tag() == tag) {
            return true;
        }
    }
    return false;
}

auto EncodeMessageBody(
    std::string& out,
    const message::MessageData& data,
    char delimiter,
    bool skip_standard_header) -> void {
    for (const auto& field : data.fields) {
        if (field.tag == 8U || field.tag == 9U || field.tag == 10U) {
            continue;
        }
        if (HasGroupCountTag(data.groups, field.tag)) {
            continue;
        }
        if (skip_standard_header && (field.tag == 35U || field.tag == 34U || field.tag == 49U || field.tag == 56U ||
                                     field.tag == 52U || field.tag == 43U)) {
            continue;
        }
        EncodeFieldValue(out, field, delimiter);
    }
    EncodeGroups(out, data.groups, delimiter);
}

auto EncodeGroups(std::string& out, const std::vector<message::GroupData>& groups, char delimiter) -> void {
    for (const auto& group : groups) {
        EncodeGroupData(out, group, delimiter);
    }
}

auto EncodeMessageBody(
    std::string& out,
    message::MessageView view,
    char delimiter,
    bool skip_standard_header) -> void {
    for (std::size_t index = 0; index < view.field_count(); ++index) {
        const auto field = view.field_at(index);
        if (!field.has_value()) {
            continue;
        }
        if (field->tag == 8U || field->tag == 9U || field->tag == 10U) {
            continue;
        }
        if (HasGroupCountTag(view, field->tag)) {
            continue;
        }
        if (skip_standard_header && (field->tag == 35U || field->tag == 34U || field->tag == 49U || field->tag == 56U ||
                                     field->tag == 52U || field->tag == 43U)) {
            continue;
        }
        EncodeFieldValue(out, *field, delimiter);
    }
    for (std::size_t index = 0; index < view.group_count(); ++index) {
        const auto group = view.group_at(index);
        if (group.has_value()) {
            EncodeGroupData(out, *group, delimiter);
        }
    }
}

auto EncodeGroupData(std::string& out, message::GroupView group, char delimiter) -> void {
    AppendField(out, group.count_tag(), static_cast<std::int64_t>(group.size()), delimiter);
    for (const auto entry : group) {
        EncodeMessageBody(out, entry, delimiter, false);
    }
}

auto NormalizeDelimiter(char delimiter) -> char {
    return delimiter == '\0' ? kFixSoh : delimiter;
}

auto CompileScopeTemplate(
    const profile::NormalizedDictionaryView& dictionary,
    std::span<const profile::FieldRuleRecord> rules,
    bool skip_managed_header_fields) -> std::shared_ptr<const CompiledScopeTemplate> {
    auto scope = std::make_shared<CompiledScopeTemplate>();
    for (const auto& rule : rules) {
        if (skip_managed_header_fields && IsTemplateManagedHeaderField(rule.tag)) {
            continue;
        }

        CompiledScopeStep step;
        step.tag = rule.tag;
        step.prefix = MakeTagPrefix(rule.tag);
        if (const auto* group_def = dictionary.find_group(rule.tag); group_def != nullptr) {
            step.kind = CompiledScopeStep::Kind::kGroup;
            step.group_scope = CompileScopeTemplate(dictionary, dictionary.group_field_rules(*group_def), false);
            scope->group_tags.push_back(rule.tag);
        } else {
            step.kind = CompiledScopeStep::Kind::kScalar;
            scope->scalar_tags.push_back(rule.tag);
        }
        scope->steps.push_back(std::move(step));
    }
    return scope;
}

auto EncodeScopeTemplate(
    std::string& out,
    std::uint32_t* checksum,
    message::MessageView view,
    const CompiledScopeTemplate& scope,
    char delimiter,
    bool skip_standard_header) -> void {
    for (const auto& step : scope.steps) {
        if (step.kind == CompiledScopeStep::Kind::kScalar) {
            const auto field = view.find_field_view(step.tag);
            if (!field.has_value()) {
                continue;
            }
            if (HasGroupCountTag(view, field->tag)) {
                continue;
            }
            AppendPrefixedFieldValue(out, checksum, step.prefix, *field, delimiter);
            continue;
        }

        const auto group = view.group(step.tag);
        if (!group.has_value()) {
            continue;
        }

        AppendPrefixedCountField(
            out,
            checksum,
            step.prefix,
            static_cast<std::uint32_t>(group->size()),
            delimiter);
        for (const auto entry : *group) {
            EncodeScopeTemplate(out, checksum, entry, *step.group_scope, delimiter, false);
        }
    }

    for (std::size_t index = 0; index < view.field_count(); ++index) {
        const auto field = view.field_at(index);
        if (!field.has_value()) {
            continue;
        }
        if (ShouldSkipField(field->tag)) {
            continue;
        }
        if (HasGroupCountTag(view, field->tag)) {
            continue;
        }
        if (ContainsTag(scope.scalar_tags, field->tag) || ContainsTag(scope.group_tags, field->tag)) {
            continue;
        }
        if (skip_standard_header && IsTemplateManagedHeaderField(field->tag)) {
            continue;
        }
        AppendTrackedGenericField(out, checksum, *field, delimiter);
    }

    for (std::size_t index = 0; index < view.group_count(); ++index) {
        const auto group = view.group_at(index);
        if (!group.has_value()) {
            continue;
        }
        if (ContainsTag(scope.group_tags, group->count_tag())) {
            continue;
        }
        AppendTrackedGenericGroup(out, checksum, *group, delimiter);
    }
}

auto ReplaceUnsignedPlaceholder(
    std::string& out,
    std::size_t offset,
    std::size_t placeholder_width,
    std::uint32_t value,
    std::uint32_t* checksum) -> void {
    std::array<char, kIntegerTextBufferBytes<std::uint32_t>> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc()) {
        return;
    }

    const auto original = std::string_view(out.data() + offset, placeholder_width);
    if (checksum != nullptr) {
        std::uint32_t original_sum = 0;
        for (const auto ch : original) {
            original_sum += static_cast<unsigned char>(ch);
        }
        *checksum -= original_sum;
        AccumulateChecksum(std::string_view(buffer.data(), static_cast<std::size_t>(ptr - buffer.data())), checksum);
    }

    out.replace(offset, placeholder_width, buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

auto CopyTextToBytes(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> bytes(text.size());
    if (!text.empty()) {
        std::memcpy(bytes.data(), text.data(), text.size());
    }
    return bytes;
}

auto WriteTwoDigits(char* out, int value) -> void {
    out[0] = static_cast<char>('0' + ((value / 10) % 10));
    out[1] = static_cast<char>('0' + (value % 10));
}

auto WriteThreeDigits(char* out, int value) -> void {
    out[0] = static_cast<char>('0' + ((value / 100) % 10));
    out[1] = static_cast<char>('0' + ((value / 10) % 10));
    out[2] = static_cast<char>('0' + (value % 10));
}

auto WriteFourDigits(char* out, int value) -> void {
    out[0] = static_cast<char>('0' + ((value / 1000) % 10));
    out[1] = static_cast<char>('0' + ((value / 100) % 10));
    out[2] = static_cast<char>('0' + ((value / 10) % 10));
    out[3] = static_cast<char>('0' + (value % 10));
}

auto EncodeFixMessageGenericToBuffer(
    message::MessageView message,
    const EncodeOptions& options,
    EncodeBuffer* buffer) -> base::Status {
    if (buffer == nullptr) {
        return base::Status::InvalidArgument("encode buffer is null");
    }

    const char delimiter = NormalizeDelimiter(options.delimiter);
    buffer->storage.clear();
    auto& full = buffer->storage;

    // BeginString + BodyLength placeholder
    const auto begin_string = options.begin_string.empty() ? std::string_view("FIX.4.4") : std::string_view(options.begin_string);
    full.append("8=");
    full.append(begin_string);
    full.push_back(delimiter);
    full.append("9=");

    constexpr std::size_t kBodyLengthPlaceholderWidth = 10U;
    const auto body_length_offset = full.size();
    full.append(kBodyLengthPlaceholderWidth, '0');
    full.push_back(delimiter);
    const auto body_start = full.size();

    // Body content directly into buffer (untracked — checksum computed at end)
    const auto msg_type = message.msg_type();
    AppendField(full, 35U, msg_type.empty() ? std::string_view("UNKNOWN") : msg_type, delimiter);

    const auto seq_num = options.msg_seq_num == 0U ? 1U : options.msg_seq_num;
    AppendField(full, 34U, static_cast<std::int64_t>(seq_num), delimiter);
    if (!options.sender_comp_id.empty()) {
        AppendField(full, 49U, options.sender_comp_id, delimiter);
    }
    if (!options.target_comp_id.empty()) {
        AppendField(full, 56U, options.target_comp_id, delimiter);
    }
    UtcTimestampBuffer timestamp_buffer;
    const auto sending_time = options.sending_time.empty() ? CurrentUtcTimestamp(&timestamp_buffer) : options.sending_time;
    AppendField(full, 52U, sending_time, delimiter);
    if (msg_type == "A" && !options.default_appl_ver_id.empty()) {
        AppendField(full, 1137U, options.default_appl_ver_id, delimiter);
    }
    if (options.poss_dup) {
        AppendField(full, 43U, std::string_view("Y"), delimiter);
    }
    if (!options.orig_sending_time.empty()) {
        AppendField(full, 122U, options.orig_sending_time, delimiter);
    }

    EncodeMessageBody(full, message, delimiter, true);

    // Backfill body length
    const auto body_length = static_cast<std::uint32_t>(full.size() - body_start);
    {
        std::array<char, kIntegerTextBufferBytes<std::uint32_t>> bl_buf{};
        const auto [ptr, ec] = std::to_chars(bl_buf.data(), bl_buf.data() + bl_buf.size(), body_length);
        if (ec == std::errc()) {
            full.replace(body_length_offset, kBodyLengthPlaceholderWidth,
                         bl_buf.data(), static_cast<std::size_t>(ptr - bl_buf.data()));
        }
    }

    // Compute checksum over entire buffer so far (same approach as post-hoc scan)
    std::uint32_t checksum = 0;
    for (const auto ch : full) {
        checksum += static_cast<unsigned char>(ch);
    }
    checksum %= 256U;

    // Append 10=XXX<SOH> without ostringstream
    std::array<char, 3> cksum_digits{};
    cksum_digits[0] = static_cast<char>('0' + ((checksum / 100U) % 10U));
    cksum_digits[1] = static_cast<char>('0' + ((checksum / 10U) % 10U));
    cksum_digits[2] = static_cast<char>('0' + (checksum % 10U));
    AppendField(full, 10U, std::string_view(cksum_digits.data(), 3U), delimiter);
    return base::Status::Ok();
}

auto MakeTemplateConfig(const EncodeOptions& options) -> EncodeTemplateConfig {
    return EncodeTemplateConfig{
        .begin_string = options.begin_string.empty() ? std::string("FIX.4.4") : options.begin_string,
        .sender_comp_id = options.sender_comp_id,
        .target_comp_id = options.target_comp_id,
        .default_appl_ver_id = options.default_appl_ver_id,
        .delimiter = NormalizeDelimiter(options.delimiter),
    };
}

auto BuildTemplateCacheKey(
    const profile::NormalizedDictionaryView& dictionary,
    std::string_view msg_type,
    const EncodeOptions& options) -> TemplateCacheKey {
    const auto config = MakeTemplateConfig(options);
    return TemplateCacheKey{
        .dictionary_identity = reinterpret_cast<std::uintptr_t>(&dictionary.profile().header()),
        .schema_hash = dictionary.profile().header().schema_hash,
        .profile_id = dictionary.profile().header().profile_id,
        .msg_type = std::string(msg_type),
        .begin_string = config.begin_string,
        .sender_comp_id = config.sender_comp_id,
        .target_comp_id = config.target_comp_id,
        .default_appl_ver_id = config.default_appl_ver_id,
        .delimiter = config.delimiter,
    };
}

auto LookupCachedTemplate(
    const profile::NormalizedDictionaryView& dictionary,
    std::string_view msg_type,
    const EncodeOptions& options) -> const FrameEncodeTemplate* {
    static std::mutex cache_mutex;
    static std::map<TemplateCacheKey, FrameEncodeTemplate> cache;

    auto key = BuildTemplateCacheKey(dictionary, msg_type, options);
    {
        std::scoped_lock lock(cache_mutex);
        const auto found = cache.find(key);
        if (found != cache.end()) {
            return &found->second;
        }
    }

    auto compiled = CompileFrameEncodeTemplate(dictionary, msg_type, MakeTemplateConfig(options));
    if (!compiled.ok()) {
        return nullptr;
    }

    std::scoped_lock lock(cache_mutex);
    const auto [it, inserted] = cache.emplace(std::move(key), compiled.value());
    return &it->second;
}

}  // namespace

struct FrameEncodeTemplate::State {
    std::string msg_type;
    std::string begin_string;
    std::string sender_comp_id;
    std::string target_comp_id;
    std::string default_appl_ver_id;
    char delimiter{kFixSoh};
    std::string begin_prefix;
    std::string msg_type_fragment;
    std::string msg_seq_prefix;
    std::string sender_fragment;
    std::string target_fragment;
    std::string sending_time_prefix;
    std::string default_appl_ver_fragment;
    std::string poss_dup_fragment;
    std::string orig_sending_time_prefix;
    std::shared_ptr<const CompiledScopeTemplate> body_scope;
};

auto CurrentUtcTimestamp(UtcTimestampBuffer* buffer) -> std::string_view {
    if (buffer == nullptr) {
        return {};
    }

    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto millis = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count());

    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);

    auto* out = buffer->storage.data();
    WriteFourDigits(out, tm.tm_year + 1900);
    WriteTwoDigits(out + 4, tm.tm_mon + 1);
    WriteTwoDigits(out + 6, tm.tm_mday);
    out[8] = '-';
    WriteTwoDigits(out + 9, tm.tm_hour);
    out[11] = ':';
    WriteTwoDigits(out + 12, tm.tm_min);
    out[14] = ':';
    WriteTwoDigits(out + 15, tm.tm_sec);
    out[17] = '.';
    WriteThreeDigits(out + 18, millis);
    return buffer->view();
}

auto CurrentUtcTimestamp() -> std::string {
    UtcTimestampBuffer buffer;
    return std::string(CurrentUtcTimestamp(&buffer));
}

auto FrameEncodeTemplate::msg_type() const -> std::string_view {
    return state_ == nullptr ? std::string_view{} : std::string_view(state_->msg_type);
}

auto CompileFrameEncodeTemplate(
    const profile::NormalizedDictionaryView& dictionary,
    std::string_view msg_type,
    const EncodeTemplateConfig& config) -> base::Result<FrameEncodeTemplate> {
    if (msg_type.empty()) {
        return base::Status::InvalidArgument("cannot compile a frame template without MsgType");
    }

    const auto* message_def = dictionary.find_message(msg_type);
    if (message_def == nullptr) {
        return base::Status::NotFound("message type is not present in the bound dictionary");
    }

    const char delimiter = NormalizeDelimiter(config.delimiter);
    auto state = std::make_shared<FrameEncodeTemplate::State>();
    state->msg_type = std::string(msg_type);
    state->begin_string = config.begin_string.empty() ? std::string("FIX.4.4") : config.begin_string;
    state->sender_comp_id = config.sender_comp_id;
    state->target_comp_id = config.target_comp_id;
    state->default_appl_ver_id = config.default_appl_ver_id;
    state->delimiter = delimiter;
    state->begin_prefix = MakeFixedFieldFragment(8U, state->begin_string, delimiter);
    state->begin_prefix.append("9=");
    state->msg_type_fragment = MakeFixedFieldFragment(35U, state->msg_type, delimiter);
    state->msg_seq_prefix = MakeTagPrefix(34U);
    state->sending_time_prefix = MakeTagPrefix(52U);
    state->orig_sending_time_prefix = MakeTagPrefix(122U);
    state->poss_dup_fragment = MakeFixedFieldFragment(43U, "Y", delimiter);
    if (!state->sender_comp_id.empty()) {
        state->sender_fragment = MakeFixedFieldFragment(49U, state->sender_comp_id, delimiter);
    }
    if (!state->target_comp_id.empty()) {
        state->target_fragment = MakeFixedFieldFragment(56U, state->target_comp_id, delimiter);
    }
    if (state->msg_type == "A" && !state->default_appl_ver_id.empty()) {
        state->default_appl_ver_fragment = MakeFixedFieldFragment(1137U, state->default_appl_ver_id, delimiter);
    }
    state->body_scope = CompileScopeTemplate(dictionary, dictionary.message_field_rules(*message_def), true);
    return FrameEncodeTemplate(std::move(state));
}

auto FrameEncodeTemplate::Encode(
    const message::Message& message,
    const EncodeOptions& options) const -> base::Result<std::vector<std::byte>> {
    return Encode(message.view(), options);
}

auto FrameEncodeTemplate::Encode(
    message::MessageView message,
    const EncodeOptions& options) const -> base::Result<std::vector<std::byte>> {
    EncodeBuffer buffer;
    auto status = EncodeToBuffer(message, options, &buffer);
    if (!status.ok()) {
        return status;
    }
    return CopyTextToBytes(buffer.text());
}

auto FrameEncodeTemplate::EncodeToBuffer(
    const message::Message& message,
    const EncodeOptions& options,
    EncodeBuffer* buffer) const -> base::Status {
    return EncodeToBuffer(message.view(), options, buffer);
}

auto FrameEncodeTemplate::EncodeToBuffer(
    message::MessageView message,
    const EncodeOptions& options,
    EncodeBuffer* buffer) const -> base::Status {
    if (state_ == nullptr) {
        return base::Status::InvalidArgument("frame template is not initialized");
    }
    if (buffer == nullptr) {
        return base::Status::InvalidArgument("encode buffer is null");
    }
    if (message.valid() && !message.msg_type().empty() && message.msg_type() != state_->msg_type) {
        return base::Status::InvalidArgument("message type does not match compiled frame template");
    }

    const char delimiter = NormalizeDelimiter(options.delimiter);
    if (delimiter != state_->delimiter) {
        return base::Status::InvalidArgument("delimiter does not match compiled frame template");
    }
    if (!options.begin_string.empty() && options.begin_string != state_->begin_string) {
        return base::Status::InvalidArgument("BeginString does not match compiled frame template");
    }
    if (!options.sender_comp_id.empty() && options.sender_comp_id != state_->sender_comp_id) {
        return base::Status::InvalidArgument("SenderCompID does not match compiled frame template");
    }
    if (!options.target_comp_id.empty() && options.target_comp_id != state_->target_comp_id) {
        return base::Status::InvalidArgument("TargetCompID does not match compiled frame template");
    }
    if (!options.default_appl_ver_id.empty() && options.default_appl_ver_id != state_->default_appl_ver_id) {
        return base::Status::InvalidArgument("DefaultApplVerID does not match compiled frame template");
    }

    buffer->storage.clear();
    auto& full = buffer->storage;
    std::uint32_t checksum = 0;
    AppendTracked(full, &checksum, state_->begin_prefix);

    constexpr std::size_t kBodyLengthPlaceholderWidth = 10U;
    const auto body_length_offset = full.size();
    AppendTracked(full, &checksum, std::string_view("0000000000", kBodyLengthPlaceholderWidth));
    AppendTracked(full, &checksum, delimiter);
    const auto body_start = full.size();

    AppendTracked(full, &checksum, state_->msg_type_fragment);

    const auto seq_num = options.msg_seq_num == 0U ? 1U : options.msg_seq_num;
    AppendPrefixedCountField(full, &checksum, state_->msg_seq_prefix, seq_num, delimiter);
    if (!state_->sender_fragment.empty()) {
        AppendTracked(full, &checksum, state_->sender_fragment);
    }
    if (!state_->target_fragment.empty()) {
        AppendTracked(full, &checksum, state_->target_fragment);
    }

    UtcTimestampBuffer timestamp_buffer;
    const auto sending_time = options.sending_time.empty() ? CurrentUtcTimestamp(&timestamp_buffer) : options.sending_time;
    AppendPrefixedStringField(full, &checksum, state_->sending_time_prefix, sending_time, delimiter);
    if (!state_->default_appl_ver_fragment.empty()) {
        AppendTracked(full, &checksum, state_->default_appl_ver_fragment);
    }
    if (options.poss_dup) {
        AppendTracked(full, &checksum, state_->poss_dup_fragment);
    }
    if (!options.orig_sending_time.empty()) {
        AppendPrefixedStringField(
            full,
            &checksum,
            state_->orig_sending_time_prefix,
            options.orig_sending_time,
            delimiter);
    }

    EncodeScopeTemplate(full, &checksum, message, *state_->body_scope, delimiter, true);

    ReplaceUnsignedPlaceholder(
        full,
        body_length_offset,
        kBodyLengthPlaceholderWidth,
        static_cast<std::uint32_t>(full.size() - body_start),
        &checksum);

    checksum %= 256U;
    AppendTracked(full, nullptr, std::string_view("10="));
    std::array<char, 4> checksum_digits{};
    checksum_digits[0] = static_cast<char>('0' + ((checksum / 100U) % 10U));
    checksum_digits[1] = static_cast<char>('0' + ((checksum / 10U) % 10U));
    checksum_digits[2] = static_cast<char>('0' + (checksum % 10U));
    AppendTracked(full, nullptr, std::string_view(checksum_digits.data(), 3U));
    AppendTracked(full, nullptr, delimiter);
    return base::Status::Ok();
}

auto EncodeFixMessageToBuffer(
    const message::Message& message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options,
    EncodeBuffer* buffer) -> base::Status {
    return EncodeFixMessageToBuffer(message.view(), dictionary, options, buffer);
}

auto EncodeFixMessageToBuffer(
    message::MessageView message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options,
    EncodeBuffer* buffer) -> base::Status {
    const auto msg_type = message.msg_type();
    if (!msg_type.empty()) {
        const auto* compiled = LookupCachedTemplate(dictionary, msg_type, options);
        if (compiled != nullptr) {
            auto status = compiled->EncodeToBuffer(message, options, buffer);
            if (status.ok()) {
                return status;
            }
        }
    }
    return EncodeFixMessageGenericToBuffer(message, options, buffer);
}

auto EncodeFixMessageToBuffer(
    const message::Message& message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options,
    EncodeBuffer* buffer,
    const PrecompiledTemplateTable* precompiled) -> base::Status {
    return EncodeFixMessageToBuffer(message.view(), dictionary, options, buffer, precompiled);
}

auto EncodeFixMessageToBuffer(
    message::MessageView message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options,
    EncodeBuffer* buffer,
    const PrecompiledTemplateTable* precompiled) -> base::Status {
    const auto msg_type = message.msg_type();
    if (!msg_type.empty()) {
        if (precompiled != nullptr) {
            if (const auto* tmpl = precompiled->find(msg_type); tmpl != nullptr) {
                auto status = tmpl->EncodeToBuffer(message, options, buffer);
                if (status.ok()) {
                    return status;
                }
            }
        } else {
            const auto* compiled = LookupCachedTemplate(dictionary, msg_type, options);
            if (compiled != nullptr) {
                auto status = compiled->EncodeToBuffer(message, options, buffer);
                if (status.ok()) {
                    return status;
                }
            }
        }
    }
    return EncodeFixMessageGenericToBuffer(message, options, buffer);
}

auto PrecompiledTemplateTable::Build(
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeTemplateConfig& config) -> base::Result<PrecompiledTemplateTable> {
    PrecompiledTemplateTable table;
    for (const auto& message_def : dictionary.messages()) {
        auto msg_type = dictionary.message_type(message_def);
        if (!msg_type.has_value() || msg_type->empty()) {
            continue;
        }

        auto compiled = CompileFrameEncodeTemplate(dictionary, *msg_type, config);
        if (!compiled.ok()) {
            continue;
        }

        table.entries_.push_back(Entry{
            .msg_type = std::string(*msg_type),
            .tmpl = std::move(compiled).value(),
        });
    }

    std::sort(table.entries_.begin(), table.entries_.end(),
        [](const auto& a, const auto& b) { return a.msg_type < b.msg_type; });
    return table;
}

auto PrecompiledTemplateTable::find(std::string_view msg_type) const -> const FrameEncodeTemplate* {
    if (last_hit_tmpl_ != nullptr && last_hit_msg_type_ == msg_type) {
        return last_hit_tmpl_;
    }
    auto it = std::lower_bound(entries_.begin(), entries_.end(), msg_type,
        [](const Entry& entry, std::string_view type) { return entry.msg_type < type; });
    if (it != entries_.end() && it->msg_type == msg_type) {
        last_hit_msg_type_ = it->msg_type;
        last_hit_tmpl_ = &it->tmpl;
        return last_hit_tmpl_;
    }
    return nullptr;
}

auto EncodeFixMessage(
    const message::Message& message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options) -> base::Result<std::vector<std::byte>> {
    return EncodeFixMessage(message.view(), dictionary, options);
}

auto EncodeFixMessage(
    message::MessageView message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options) -> base::Result<std::vector<std::byte>> {
    EncodeBuffer buffer;
    auto status = EncodeFixMessageToBuffer(message, dictionary, options, &buffer);
    if (!status.ok()) {
        return status;
    }
    return CopyTextToBytes(buffer.text());
}

auto DecodeFixMessageView(
    std::span<const std::byte> bytes,
    const profile::NormalizedDictionaryView& dictionary,
    char delimiter) -> base::Result<DecodedMessageView> {
    const char normalized_delimiter = NormalizeDelimiter(delimiter);
    auto tokens = Tokenize(bytes, normalized_delimiter);
    if (!tokens.ok()) {
        return tokens.status();
    }
    if (tokens.value().size() < 4U) {
        return base::Status::FormatError("FIX frame is too short");
    }

    const auto& fields = tokens.value();
    if (fields[0].tag != 8U || fields[1].tag != 9U || fields.back().tag != 10U) {
        return base::Status::FormatError("FIX frame must begin with 8 and 9 and end with 10");
    }

    auto declared_body_length = ParseUnsigned(fields[1].value, "BodyLength");
    if (!declared_body_length.ok()) {
        return declared_body_length.status();
    }
    const auto actual_body_length = fields.back().start_offset - fields[1].end_offset;
    if (declared_body_length.value() != actual_body_length) {
        return base::Status::FormatError("BodyLength mismatch");
    }

    auto expected_checksum = ParseUnsigned(fields.back().value, "CheckSum");
    if (!expected_checksum.ok()) {
        return expected_checksum.status();
    }
    std::uint32_t actual_checksum = 0;
    for (std::size_t index = 0; index < fields.back().start_offset; ++index) {
        actual_checksum += std::to_integer<unsigned char>(bytes[index]);
    }
    actual_checksum %= 256U;
    if (actual_checksum != expected_checksum.value()) {
        return base::Status::FormatError("CheckSum mismatch");
    }

    message::ParsedMessageData parsed_message;
    parsed_message.raw = bytes;
    parsed_message.field_slots.reserve(fields.size());
    parsed_message.groups.reserve(std::max<std::size_t>(1U, fields.size() / 4U));
    SessionHeaderView header;
    ValidationIssue validation_issue;
    header.begin_string = fields[0].value;
    header.body_length = declared_body_length.value();
    header.checksum = expected_checksum.value();
    ScopeValidationState message_validation;
    message_validation.seen_tags.reserve(fields.size());

    std::size_t index = 2U;
    while (index + 1U < fields.size()) {
        const auto& field = fields[index];
        if (ShouldSkipField(field.tag)) {
            ++index;
            continue;
        }

        if (field.tag == 35U) {
            parsed_message.msg_type = field.value;
            header.msg_type = field.value;
            const auto* message_def = dictionary.find_message(header.msg_type);
            const auto rules = message_def != nullptr
                                   ? dictionary.message_field_rules(*message_def)
                                   : std::span<const profile::FieldRuleRecord>{};
            ValidateConsumedField(
                dictionary,
                field.tag,
                rules,
                message_def != nullptr ? dictionary.message_rule_index(*message_def, field.tag) : -1,
                &message_validation,
                &validation_issue,
                true,
                message_def != nullptr,
                "message");
            auto slot = MakeParsedFieldSlot(bytes, field, dictionary);
            if (!slot.ok()) {
                return slot.status();
            }
            AppendParsedFieldSlot(&parsed_message, RootContainer(), std::move(slot).value());
            ++index;
            continue;
        }
        if (field.tag == 34U) {
            auto parsed = ParseUnsigned(field.value, "MsgSeqNum");
            if (!parsed.ok()) {
                return parsed.status();
            }
            header.msg_seq_num = parsed.value();
        } else if (field.tag == 49U) {
            header.sender_comp_id = field.value;
        } else if (field.tag == 56U) {
            header.target_comp_id = field.value;
        } else if (field.tag == 52U) {
            header.sending_time = field.value;
        } else if (field.tag == 122U) {
            header.orig_sending_time = field.value;
        } else if (field.tag == 1137U) {
            header.default_appl_ver_id = field.value;
        } else if (field.tag == 43U) {
            auto parsed = ParseBoolean(field.value, "PossDupFlag");
            if (!parsed.ok()) {
                return parsed.status();
            }
            header.poss_dup = parsed.value();
        }

        const profile::MessageDefRecord* message_def = nullptr;
        if (!header.msg_type.empty()) {
            message_def = dictionary.find_message(header.msg_type);
        }
        const auto rules = message_def != nullptr
                               ? dictionary.message_field_rules(*message_def)
                               : std::span<const profile::FieldRuleRecord>{};

        ValidateConsumedField(
            dictionary,
            field.tag,
            rules,
            message_def != nullptr ? dictionary.message_rule_index(*message_def, field.tag) : -1,
            &message_validation,
            &validation_issue,
            true,
            message_def != nullptr,
            "message");

        if (message_def != nullptr && dictionary.message_rule_allows_tag(*message_def, field.tag) &&
            dictionary.find_group(field.tag) != nullptr) {
            auto slot = MakeParsedFieldSlot(bytes, field, dictionary);
            if (!slot.ok()) {
                return slot.status();
            }
            AppendParsedFieldSlot(&parsed_message, RootContainer(), std::move(slot).value());
            auto next_index = ParseParsedGroupEntries(
                dictionary,
                bytes,
                fields,
                index,
                *dictionary.find_group(field.tag),
                &parsed_message,
                RootContainer(),
                1U,
                &validation_issue);
            if (!next_index.ok()) {
                return next_index.status();
            }
            index = next_index.value();
            continue;
        }

        auto slot = MakeParsedFieldSlot(bytes, field, dictionary);
        if (!slot.ok()) {
            return slot.status();
        }
        AppendParsedFieldSlot(&parsed_message, RootContainer(), std::move(slot).value());
        ++index;
    }

    DecodedMessageView decoded;
    decoded.message = message::ParsedMessage(std::move(parsed_message));
    decoded.header = std::move(header);
    decoded.raw = bytes;
    decoded.validation_issue = std::move(validation_issue);
    return decoded;
}

auto DecodeFixMessage(
    std::span<const std::byte> bytes,
    const profile::NormalizedDictionaryView& dictionary,
    char delimiter) -> base::Result<DecodedMessage> {
    auto decoded = DecodeFixMessageView(bytes, dictionary, delimiter);
    if (!decoded.ok()) {
        return decoded.status();
    }
    return std::move(decoded).value().ToOwned();
}

auto PeekSessionHeaderView(
    std::span<const std::byte> bytes,
    char delimiter) -> base::Result<SessionHeaderView> {
    const char normalized_delimiter = NormalizeDelimiter(delimiter);
    const auto delimiter_byte = static_cast<std::byte>(static_cast<unsigned char>(normalized_delimiter));
    const auto equals_byte = static_cast<std::byte>('=');
    SessionHeaderView header;
    std::size_t field_start = 0U;
    std::size_t field_count = 0U;
    std::size_t body_start_offset = 0U;
    std::size_t checksum_field_start = 0U;
    bool saw_checksum = false;

    while (field_start < bytes.size()) {
        const auto remaining = bytes.size() - field_start;
        const auto* soh_ptr = FindByte(bytes.data() + field_start, remaining, delimiter_byte);
        const auto index = static_cast<std::size_t>(soh_ptr - bytes.data());

        if (index >= bytes.size()) {
            break;
        }

        if (index == field_start) {
            return base::Status::FormatError("empty FIX field is not allowed");
        }

        const auto field_len = index - field_start;
        const auto* eq_ptr = FindByte(bytes.data() + field_start, field_len, equals_byte);
        const auto equals = static_cast<std::size_t>(eq_ptr - (bytes.data() + field_start));
        if (equals >= field_len || equals == 0U || equals == field_len - 1U) {
            return base::Status::FormatError("invalid FIX field syntax");
        }

        const auto* field_bytes = reinterpret_cast<const char*>(bytes.data() + field_start);
        auto tag = ParseUnsigned(std::string_view(field_bytes, equals), "field tag");
        if (!tag.ok()) {
            return tag.status();
        }
        const auto value = std::string_view(
            reinterpret_cast<const char*>(bytes.data() + field_start + equals + 1U),
            field_len - equals - 1U);
        ++field_count;

        if (field_count == 1U) {
            if (tag.value() != 8U) {
                return base::Status::FormatError("FIX frame must begin with 8 and 9 and end with 10");
            }
            header.begin_string = value;
            field_start = index + 1U;
            continue;
        }

        if (field_count == 2U) {
            if (tag.value() != 9U) {
                return base::Status::FormatError("FIX frame must begin with 8 and 9 and end with 10");
            }
            auto declared_body_length = ParseUnsigned(value, "BodyLength");
            if (!declared_body_length.ok()) {
                return declared_body_length.status();
            }
            header.body_length = declared_body_length.value();
            body_start_offset = index + 1U;
            field_start = index + 1U;
            continue;
        }

        if (tag.value() == 10U) {
            if (index + 1U != bytes.size()) {
                return base::Status::FormatError("FIX frame must begin with 8 and 9 and end with 10");
            }

            auto expected_checksum = ParseUnsigned(value, "CheckSum");
            if (!expected_checksum.ok()) {
                return expected_checksum.status();
            }
            header.checksum = expected_checksum.value();
            checksum_field_start = field_start;
            saw_checksum = true;
            field_start = index + 1U;
            break;
        }

        switch (tag.value()) {
            case 35U:
                header.msg_type = value;
                break;
            case 34U: {
                auto parsed = ParseUnsigned(value, "MsgSeqNum");
                if (!parsed.ok()) {
                    return parsed.status();
                }
                header.msg_seq_num = parsed.value();
                break;
            }
            case 43U: {
                auto parsed = ParseBoolean(value, "PossDupFlag");
                if (!parsed.ok()) {
                    return parsed.status();
                }
                header.poss_dup = parsed.value();
                break;
            }
            case 49U:
                header.sender_comp_id = value;
                break;
            case 52U:
                header.sending_time = value;
                break;
            case 122U:
                header.orig_sending_time = value;
                break;
            case 56U:
                header.target_comp_id = value;
                break;
            case 1137U:
                header.default_appl_ver_id = value;
                break;
            default:
                break;
        }

        field_start = index + 1U;
    }

    if (field_start != bytes.size()) {
        return base::Status::FormatError("FIX frame is missing its final delimiter");
    }
    if (field_count == 0U) {
        return base::Status::FormatError("FIX frame has no fields");
    }
    if (field_count < 4U) {
        return base::Status::FormatError("FIX frame is too short");
    }
    if (!saw_checksum) {
        return base::Status::FormatError("FIX frame must begin with 8 and 9 and end with 10");
    }

    const auto actual_body_length = checksum_field_start - body_start_offset;
    if (header.body_length != actual_body_length) {
        return base::Status::FormatError("BodyLength mismatch");
    }

    std::uint32_t actual_checksum = 0;
    for (std::size_t index = 0; index < checksum_field_start; ++index) {
        actual_checksum += std::to_integer<unsigned char>(bytes[index]);
    }
    actual_checksum %= 256U;
    if (actual_checksum != header.checksum) {
        return base::Status::FormatError("CheckSum mismatch");
    }

    return header;
}

auto PeekSessionHeader(
    std::span<const std::byte> bytes,
    char delimiter) -> base::Result<SessionHeader> {
    auto header = PeekSessionHeaderView(bytes, delimiter);
    if (!header.ok()) {
        return header.status();
    }
    return header.value().ToOwned();
}

}  // namespace fastfix::codec