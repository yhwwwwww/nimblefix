#include "nimblefix/profile/builder_codegen.h"

#include "nimblefix/codec/fix_tags.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nimble::profile {

namespace {

auto
DefaultNamespaceName(const NormalizedDictionary& dictionary) -> std::string
{
  return "nimble::generated::profile_" + std::to_string(dictionary.profile_id);
}

auto
CamelToSnake(std::string_view name) -> std::string
{
  const auto LowercaseWord = [](std::string_view word) {
    std::string result;
    result.reserve(word.size());
    for (const auto ch : word) {
      result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
  };

  const auto TokenizeIdentifierWords = [](std::string_view text) {
    auto CountLowercaseRun = [](std::string_view value, std::size_t offset) {
      std::size_t count = 0U;
      while (offset + count < value.size() && std::islower(static_cast<unsigned char>(value[offset + count])) != 0) {
        ++count;
      }
      return count;
    };

    std::vector<std::string> words;
    std::string current;
    current.reserve(text.size());

    const auto FlushCurrent = [&]() {
      if (!current.empty()) {
        words.push_back(current);
        current.clear();
      }
    };

    for (std::size_t index = 0U; index < text.size(); ++index) {
      const auto ch = text[index];
      if (!std::isalnum(static_cast<unsigned char>(ch))) {
        FlushCurrent();
        continue;
      }

      if (!current.empty()) {
        const auto prev = current.back();
        bool boundary = false;
        if (std::islower(static_cast<unsigned char>(prev)) != 0 && std::isupper(static_cast<unsigned char>(ch)) != 0) {
          boundary = true;
        } else if (std::isdigit(static_cast<unsigned char>(prev)) != 0 &&
                   std::isalpha(static_cast<unsigned char>(ch)) != 0) {
          boundary = true;
        } else if (std::isupper(static_cast<unsigned char>(prev)) != 0 &&
                   std::isupper(static_cast<unsigned char>(ch)) != 0 && index + 1U < text.size() &&
                   std::islower(static_cast<unsigned char>(text[index + 1U])) != 0) {
          const auto lower_run = CountLowercaseRun(text, index + 1U);
          const bool pluralized_acronym =
            lower_run == 1U && std::tolower(static_cast<unsigned char>(text[index + 1U])) == 's';
          boundary = !pluralized_acronym;
        }
        if (boundary) {
          FlushCurrent();
        }
      }

      current.push_back(ch);
    }

    FlushCurrent();
    return words;
  };

  const auto words = TokenizeIdentifierWords(name);
  std::string result;
  result.reserve(name.size() + 8U);
  for (std::size_t index = 0U; index < words.size(); ++index) {
    if (index != 0U) {
      result.push_back('_');
    }
    result.append(LowercaseWord(words[index]));
  }

  if (result.empty()) {
    return "field";
  }
  if (std::isdigit(static_cast<unsigned char>(result.front()))) {
    result.insert(result.begin(), '_');
  }
  return result;
}

auto
MakeIdentifier(std::string_view name) -> std::string
{
  std::string result;
  result.reserve(name.size() + 4U);
  for (const auto ch : name) {
    if (std::isalnum(static_cast<unsigned char>(ch))) {
      result.push_back(ch);
    } else {
      result.push_back('_');
    }
  }
  if (result.empty()) {
    return "Value";
  }
  if (std::isdigit(static_cast<unsigned char>(result.front()))) {
    result.insert(result.begin(), '_');
  }
  return result;
}

auto
LowercaseWord(std::string_view word) -> std::string
{
  std::string result;
  result.reserve(word.size());
  for (const auto ch : word) {
    result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return result;
}

auto
PascalCaseWord(std::string_view word) -> std::string
{
  if (word.empty()) {
    return {};
  }

  auto lower = LowercaseWord(word);
  lower.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(lower.front())));
  return lower;
}

auto
TokenizeIdentifierWords(std::string_view text) -> std::vector<std::string>
{
  auto CountLowercaseRun = [](std::string_view value, std::size_t offset) {
    std::size_t count = 0U;
    while (offset + count < value.size() && std::islower(static_cast<unsigned char>(value[offset + count])) != 0) {
      ++count;
    }
    return count;
  };

  std::vector<std::string> words;
  std::string current;
  current.reserve(text.size());

  const auto FlushCurrent = [&]() {
    if (!current.empty()) {
      words.push_back(current);
      current.clear();
    }
  };

  for (std::size_t index = 0U; index < text.size(); ++index) {
    const auto ch = text[index];
    if (!std::isalnum(static_cast<unsigned char>(ch))) {
      FlushCurrent();
      continue;
    }

    if (!current.empty()) {
      const auto prev = current.back();
      bool boundary = false;
      if (std::islower(static_cast<unsigned char>(prev)) != 0 && std::isupper(static_cast<unsigned char>(ch)) != 0) {
        boundary = true;
      } else if (std::isdigit(static_cast<unsigned char>(prev)) != 0 &&
                 std::isalpha(static_cast<unsigned char>(ch)) != 0) {
        boundary = true;
      } else if (std::isupper(static_cast<unsigned char>(prev)) != 0 &&
                 std::isupper(static_cast<unsigned char>(ch)) != 0 && index + 1U < text.size() &&
                 std::islower(static_cast<unsigned char>(text[index + 1U])) != 0) {
        const auto lower_run = CountLowercaseRun(text, index + 1U);
        const bool pluralized_acronym =
          lower_run == 1U && std::tolower(static_cast<unsigned char>(text[index + 1U])) == 's';
        boundary = !pluralized_acronym;
      }
      if (boundary) {
        FlushCurrent();
      }
    }

    current.push_back(ch);
  }

  FlushCurrent();
  return words;
}

auto
SnakeCaseIdentifierFromWords(const std::vector<std::string>& words, std::string_view fallback) -> std::string
{
  std::string result;
  for (std::size_t index = 0U; index < words.size(); ++index) {
    if (words[index].empty()) {
      continue;
    }
    if (!result.empty()) {
      result.push_back('_');
    }
    result.append(LowercaseWord(words[index]));
  }

  if (result.empty()) {
    result = std::string(fallback);
  }
  if (!result.empty() && std::isdigit(static_cast<unsigned char>(result.front())) != 0) {
    result.insert(result.begin(), '_');
  }
  return result;
}

auto
PascalCaseIdentifierFromWords(const std::vector<std::string>& words, std::string_view fallback) -> std::string
{
  std::string result;
  for (const auto& word : words) {
    if (word.empty()) {
      continue;
    }
    result.append(PascalCaseWord(word));
  }

  if (result.empty()) {
    result = std::string(fallback);
  }
  if (!result.empty() && std::isdigit(static_cast<unsigned char>(result.front())) != 0) {
    result.insert(result.begin(), '_');
  }
  return result;
}

auto
CaseInsensitiveEqual(std::string_view lhs, std::string_view rhs) -> bool
{
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < lhs.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(lhs[index])) != std::tolower(static_cast<unsigned char>(rhs[index]))) {
      return false;
    }
  }
  return true;
}

auto
SingularizeWord(std::string_view word) -> std::string
{
  auto lower = LowercaseWord(word);
  if (lower == "ids") {
    return "id";
  }
  if (lower.size() > 3U && lower.ends_with("ies")) {
    return lower.substr(0U, lower.size() - 3U) + "y";
  }
  if (lower.size() > 1U && lower.back() == 's' && !lower.ends_with("ss")) {
    lower.pop_back();
  }
  return lower;
}

auto
PluralizeWord(std::string_view word) -> std::string
{
  auto lower = LowercaseWord(word);
  if (lower == "id") {
    return "ids";
  }
  if (lower.size() > 1U && lower.back() == 'y') {
    const auto prev = lower[lower.size() - 2U];
    const bool vowel = prev == 'a' || prev == 'e' || prev == 'i' || prev == 'o' || prev == 'u';
    if (!vowel) {
      return lower.substr(0U, lower.size() - 1U) + "ies";
    }
  }
  return lower + 's';
}

auto
FieldApiWords(const FieldDef& field) -> std::vector<std::string>
{
  auto words = TokenizeIdentifierWords(field.name);
  if (field.value_type == ValueType::kBoolean && !words.empty() && CaseInsensitiveEqual(words.back(), "Flag")) {
    words.pop_back();
  }
  return words;
}

auto
FieldMethodName(const FieldDef& field) -> std::string
{
  return SnakeCaseIdentifierFromWords(FieldApiWords(field), "field");
}

auto
CommonPrefixWords(const std::vector<std::vector<std::string>>& word_sets) -> std::vector<std::string>
{
  if (word_sets.empty()) {
    return {};
  }

  auto prefix = word_sets.front();
  for (std::size_t set_index = 1U; set_index < word_sets.size() && !prefix.empty(); ++set_index) {
    std::size_t shared = 0U;
    while (shared < prefix.size() && shared < word_sets[set_index].size() &&
           CaseInsensitiveEqual(prefix[shared], word_sets[set_index][shared])) {
      ++shared;
    }
    prefix.resize(shared);
  }
  return prefix;
}

struct GroupApiNames
{
  std::string singular_pascal;
  std::string singular_snake;
  std::string plural_snake;
  std::string entry_type;
  std::string view_type;
  std::string entry_view_type;
};

struct GroupContextApiName
{
  const GroupDef* group{ nullptr };
  GroupApiNames names;
};

auto
GroupStemWords(const GroupDef& group,
               const std::unordered_map<std::uint32_t, const FieldDef*>& field_by_tag,
               const std::unordered_map<std::uint32_t, const GroupDef*>& group_by_tag) -> std::vector<std::string>
{
  std::vector<std::vector<std::string>> field_words;
  field_words.reserve(group.field_rules.size());
  for (const auto& rule : group.field_rules) {
    if (group_by_tag.count(rule.tag) != 0U) {
      continue;
    }
    const auto field_it = field_by_tag.find(rule.tag);
    if (field_it == field_by_tag.end()) {
      continue;
    }
    field_words.push_back(TokenizeIdentifierWords(field_it->second->name));
  }

  auto stem_words = CommonPrefixWords(field_words);
  if (!stem_words.empty()) {
    return stem_words;
  }

  stem_words = TokenizeIdentifierWords(group.name);
  if (!stem_words.empty() && CaseInsensitiveEqual(stem_words.front(), "No")) {
    stem_words.erase(stem_words.begin());
  }
  if (!stem_words.empty()) {
    stem_words.back() = SingularizeWord(stem_words.back());
  }
  return stem_words;
}

auto
BuildGroupApiNames(const GroupDef& group,
                   const std::unordered_map<std::uint32_t, const FieldDef*>& field_by_tag,
                   const std::unordered_map<std::uint32_t, const GroupDef*>& group_by_tag) -> GroupApiNames
{
  auto singular_words = GroupStemWords(group, field_by_tag, group_by_tag);
  if (singular_words.empty()) {
    singular_words.push_back("group");
  }

  auto plural_words = singular_words;
  plural_words.back() = PluralizeWord(plural_words.back());

  const auto singular_pascal = PascalCaseIdentifierFromWords(singular_words, "Group");
  return GroupApiNames{
    .singular_pascal = singular_pascal,
    .singular_snake = SnakeCaseIdentifierFromWords(singular_words, "group"),
    .plural_snake = SnakeCaseIdentifierFromWords(plural_words, "groups"),
    .entry_type = singular_pascal + "Entry",
    .view_type = singular_pascal + "View",
    .entry_view_type = singular_pascal + "EntryView",
  };
}

auto
BuildResolvedGroupApiNames(const std::vector<const GroupDef*>& groups,
                           const std::unordered_map<std::uint32_t, const FieldDef*>& field_by_tag,
                           const std::unordered_map<std::uint32_t, const GroupDef*>& group_by_tag)
  -> std::unordered_map<std::uint32_t, GroupApiNames>
{
  std::vector<GroupContextApiName> resolved_groups;
  resolved_groups.reserve(groups.size());
  std::unordered_map<std::string, std::size_t> base_type_counts;
  std::unordered_map<std::string, std::size_t> candidate_type_counts;

  for (const auto* group : groups) {
    auto names = BuildGroupApiNames(*group, field_by_tag, group_by_tag);
    ++base_type_counts[names.singular_pascal];
    resolved_groups.push_back(GroupContextApiName{ .group = group, .names = std::move(names) });
  }

  for (const auto& group : resolved_groups) {
    auto candidate = group.names.singular_pascal;
    if (base_type_counts.at(group.names.singular_pascal) > 1U) {
      candidate += MakeIdentifier(group.group->name);
    }
    ++candidate_type_counts[candidate];
  }

  std::unordered_map<std::uint32_t, GroupApiNames> resolved;
  resolved.reserve(resolved_groups.size());
  for (auto& group : resolved_groups) {
    auto names = std::move(group.names);
    auto singular_pascal = names.singular_pascal;
    if (base_type_counts.at(names.singular_pascal) > 1U) {
      singular_pascal += MakeIdentifier(group.group->name);
    }
    if (candidate_type_counts.at(singular_pascal) > 1U) {
      singular_pascal += std::to_string(group.group->count_tag);
    }

    names.singular_pascal = singular_pascal;
    names.entry_type = singular_pascal + "Entry";
    names.view_type = singular_pascal + "View";
    names.entry_view_type = singular_pascal + "EntryView";
    resolved.emplace(group.group->count_tag, std::move(names));
  }
  return resolved;
}

template<typename RuleRange>
auto
BuildContextGroupApiNames(const RuleRange& rules,
                          const std::unordered_map<std::uint32_t, GroupApiNames>& group_api_names,
                          const std::unordered_map<std::uint32_t, const GroupDef*>& group_by_tag)
  -> std::unordered_map<std::uint32_t, GroupApiNames>
{
  std::vector<GroupContextApiName> groups;
  groups.reserve(rules.size());
  std::unordered_map<std::string, std::size_t> plural_counts;

  for (const auto& rule : rules) {
    const auto group_it = group_by_tag.find(rule.tag);
    if (group_it == group_by_tag.end()) {
      continue;
    }

    auto names = group_api_names.at(group_it->second->count_tag);
    const auto duplicate_count = ++plural_counts[names.plural_snake];
    if (duplicate_count > 1U) {
      const auto suffix = CamelToSnake(group_it->second->name);
      names.plural_snake += "_" + suffix;
      names.singular_snake += "_" + suffix;
    }
    groups.push_back(GroupContextApiName{ .group = group_it->second, .names = std::move(names) });
  }

  std::unordered_map<std::uint32_t, GroupApiNames> resolved;
  resolved.reserve(groups.size());
  for (auto& group : groups) {
    resolved.emplace(group.group->count_tag, std::move(group.names));
  }
  return resolved;
}

auto
QualifiedFieldStorageType(ValueType type) -> std::string_view
{
  switch (type) {
    case ValueType::kString:
    case ValueType::kTimestamp:
      return "std::string_view";
    case ValueType::kInt:
      return "std::int64_t";
    case ValueType::kChar:
      return "char";
    case ValueType::kFloat:
      return "double";
    case ValueType::kBoolean:
      return "bool";
    case ValueType::kUnknown:
      break;
  }
  return "std::string_view";
}

auto
ViewRawReturnType(ValueType type) -> std::string_view
{
  switch (type) {
    case ValueType::kString:
    case ValueType::kTimestamp:
      return "std::optional<std::string_view>";
    case ValueType::kInt:
      return "std::optional<std::int64_t>";
    case ValueType::kChar:
      return "std::optional<char>";
    case ValueType::kFloat:
      return "std::optional<double>";
    case ValueType::kBoolean:
      return "std::optional<bool>";
    case ValueType::kUnknown:
      break;
  }
  return "std::optional<std::string_view>";
}

auto
MessageViewGetter(ValueType type) -> std::string_view
{
  switch (type) {
    case ValueType::kString:
      return "get_string";
    case ValueType::kTimestamp:
      return "get_string";
    case ValueType::kInt:
      return "get_int";
    case ValueType::kChar:
      return "get_char";
    case ValueType::kFloat:
      return "get_float";
    case ValueType::kBoolean:
      return "get_boolean";
    case ValueType::kUnknown:
      break;
  }
  return "get_string";
}

auto
IsStringLike(ValueType type) -> bool
{
  return type == ValueType::kString || type == ValueType::kTimestamp;
}

auto
BodyEncodeAppendMethod(ValueType type) -> std::string_view
{
  switch (type) {
    case ValueType::kString:
    case ValueType::kTimestamp:
      return "append_string_field";
    case ValueType::kInt:
      return "append_int_field";
    case ValueType::kChar:
      return "append_char_field";
    case ValueType::kFloat:
      return "append_float_field";
    case ValueType::kBoolean:
      return "append_bool_field";
    case ValueType::kUnknown:
      break;
  }
  return "append_string_field";
}

auto
ShapeScalarKind(ValueType type) -> std::string_view
{
  switch (type) {
    case ValueType::kString:
    case ValueType::kTimestamp:
      return "detail::ScalarKind::kString";
    case ValueType::kInt:
      return "detail::ScalarKind::kInt";
    case ValueType::kChar:
      return "detail::ScalarKind::kChar";
    case ValueType::kFloat:
      return "detail::ScalarKind::kFloat";
    case ValueType::kBoolean:
      return "detail::ScalarKind::kBool";
    case ValueType::kUnknown:
      break;
  }
  return "detail::ScalarKind::kString";
}

auto
ShapePresence(const FieldRule& rule) -> std::string_view
{
  return rule.required() ? "detail::Presence::kAlways" : "detail::Presence::kOptional";
}

auto
ShouldSkipShapeRule(std::uint32_t tag) -> bool
{
  return codec::tags::IsAggregateSessionEnvelopeTag(tag);
}

auto
HasEnums(const FieldDef& field) -> bool
{
  return !field.enum_values.empty();
}

auto
EnumTypeName(const FieldDef& field) -> std::string
{
  return PascalCaseIdentifierFromWords(FieldApiWords(field), MakeIdentifier(field.name));
}

auto
EnumValueName(std::string_view name) -> std::string
{
  return PascalCaseIdentifierFromWords(TokenizeIdentifierWords(name), MakeIdentifier(name));
}

auto
EnumUnderlyingType(ValueType type) -> std::string_view
{
  switch (type) {
    case ValueType::kChar:
      return "char";
    case ValueType::kInt:
      return "std::int64_t";
    case ValueType::kString:
    case ValueType::kTimestamp:
      return "std::uint16_t";
    case ValueType::kFloat:
      return "double";
    case ValueType::kBoolean:
      return "bool";
    case ValueType::kUnknown:
      break;
  }
  return "std::uint16_t";
}

auto
EmitStringLiteral(std::string_view text) -> std::string
{
  std::string out;
  out.reserve(text.size() + 8U);
  for (const auto ch : text) {
    switch (ch) {
      case '\\':
        out.append("\\\\");
        break;
      case '"':
        out.append("\\\"");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

auto
EmitCharLiteral(char value) -> std::string
{
  switch (value) {
    case '\\':
      return "'\\\\'";
    case '\'':
      return "'\\\''";
    case '\n':
      return "'\\n'";
    case '\r':
      return "'\\r'";
    case '\t':
      return "'\\t'";
    default:
      return std::string("'") + value + "'";
  }
}

auto
TryParseIntegerLiteral(std::string_view text) -> std::optional<std::int64_t>
{
  if (text.empty()) {
    return std::nullopt;
  }
  std::size_t index = 0U;
  try {
    const auto value = std::stoll(std::string(text), &index, 10);
    if (index != text.size()) {
      return std::nullopt;
    }
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

auto
EmitEnumValueLiteral(const FieldDef& field, std::string_view value, std::uint16_t ordinal) -> std::string
{
  switch (field.value_type) {
    case ValueType::kChar:
      if (value.size() == 1U) {
        return EmitCharLiteral(value.front());
      }
      break;
    case ValueType::kInt: {
      const auto parsed = TryParseIntegerLiteral(value);
      if (parsed.has_value()) {
        return std::to_string(*parsed);
      }
      break;
    }
    case ValueType::kString:
    case ValueType::kTimestamp:
    case ValueType::kFloat:
    case ValueType::kBoolean:
    case ValueType::kUnknown:
      break;
  }
  return std::to_string(ordinal);
}

auto
EmitEnumToWireReturnType(const FieldDef& field) -> std::string_view
{
  switch (field.value_type) {
    case ValueType::kChar:
      return "char";
    case ValueType::kInt:
      return "std::int64_t";
    case ValueType::kString:
    case ValueType::kTimestamp:
      return "std::string_view";
    case ValueType::kFloat:
      return "double";
    case ValueType::kBoolean:
      return "bool";
    case ValueType::kUnknown:
      break;
  }
  return "std::string_view";
}

auto
EmitTypedParameterType(const FieldDef& field) -> std::string
{
  if (HasEnums(field)) {
    return EnumTypeName(field);
  }

  switch (field.value_type) {
    case ValueType::kString:
    case ValueType::kTimestamp:
      return "std::string_view";
    case ValueType::kInt:
      return "std::int64_t";
    case ValueType::kChar:
      return "char";
    case ValueType::kFloat:
      return "double";
    case ValueType::kBoolean:
      return "bool";
    case ValueType::kUnknown:
      break;
  }
  return "std::string_view";
}

auto
EmitSetterStorageStatement(const FieldDef& field, std::string_view field_member) -> std::string
{
  std::ostringstream out;
  if (HasEnums(field)) {
    switch (field.value_type) {
      case ValueType::kString:
      case ValueType::kTimestamp:
        out << field_member << "_ = ToWire(value);";
        return out.str();
      case ValueType::kChar:
      case ValueType::kInt:
      case ValueType::kFloat:
      case ValueType::kBoolean:
        out << field_member << "_ = ToWire(value);";
        return out.str();
      case ValueType::kUnknown:
        break;
    }
  }

  if (IsStringLike(field.value_type)) {
    out << field_member << "_ = value;";
  } else {
    out << field_member << "_ = value;";
  }
  return out.str();
}

auto
FieldByTag(const NormalizedDictionary& dictionary) -> std::unordered_map<std::uint32_t, const FieldDef*>
{
  std::unordered_map<std::uint32_t, const FieldDef*> by_tag;
  by_tag.reserve(dictionary.fields.size());
  for (const auto& field : dictionary.fields) {
    by_tag.emplace(field.tag, &field);
  }
  return by_tag;
}

auto
GroupByTag(const NormalizedDictionary& dictionary) -> std::unordered_map<std::uint32_t, const GroupDef*>
{
  std::unordered_map<std::uint32_t, const GroupDef*> by_tag;
  by_tag.reserve(dictionary.groups.size());
  for (const auto& group : dictionary.groups) {
    by_tag.emplace(group.count_tag, &group);
  }
  return by_tag;
}

auto
OrderedGroupsByNesting(const NormalizedDictionary& dictionary,
                       const std::unordered_map<std::uint32_t, const GroupDef*>& group_by_tag)
  -> std::vector<const GroupDef*>
{
  enum class VisitState : std::uint8_t
  {
    kVisiting,
    kVisited,
  };

  std::unordered_map<std::uint32_t, VisitState> visited;
  visited.reserve(dictionary.groups.size());

  std::vector<const GroupDef*> ordered;
  ordered.reserve(dictionary.groups.size());

  std::function<void(const GroupDef&)> visit_group = [&](const GroupDef& group) {
    const auto visited_it = visited.find(group.count_tag);
    if (visited_it != visited.end()) {
      return;
    }

    visited.emplace(group.count_tag, VisitState::kVisiting);
    for (const auto& rule : group.field_rules) {
      const auto nested_it = group_by_tag.find(rule.tag);
      if (nested_it == group_by_tag.end()) {
        continue;
      }
      const auto nested_state = visited.find(nested_it->second->count_tag);
      if (nested_state != visited.end() && nested_state->second == VisitState::kVisiting) {
        continue;
      }
      visit_group(*nested_it->second);
    }
    visited[group.count_tag] = VisitState::kVisited;
    ordered.push_back(&group);
  };

  for (const auto& group : dictionary.groups) {
    visit_group(group);
  }
  return ordered;
}

auto
RequiredWordCount(std::size_t field_count) -> std::size_t
{
  return (field_count + 63U) / 64U;
}

template<typename RuleRange>
auto
RequiredFieldMaskWords(const RuleRange& rules) -> std::vector<std::uint64_t>
{
  std::vector<std::uint64_t> words(RequiredWordCount(rules.size()), 0U);
  for (std::size_t index = 0U; index < rules.size(); ++index) {
    if (!rules[index].required()) {
      continue;
    }
    words[index / 64U] |= (std::uint64_t{ 1 } << (index % 64U));
  }
  return words;
}

auto
RuleDisplayName(std::uint32_t tag,
                const std::unordered_map<std::uint32_t, const FieldDef*>& field_by_tag,
                const std::unordered_map<std::uint32_t, const GroupDef*>& group_by_tag) -> std::string_view
{
  const auto group_it = group_by_tag.find(tag);
  if (group_it != group_by_tag.end()) {
    return group_it->second->name;
  }
  const auto field_it = field_by_tag.find(tag);
  if (field_it != field_by_tag.end()) {
    return field_it->second->name;
  }
  return "field";
}

template<typename RuleRange>
auto
EmitRequiredFieldMetadata(std::ostringstream& out,
                          const RuleRange& rules,
                          const std::unordered_map<std::uint32_t, const FieldDef*>& field_by_tag,
                          const std::unordered_map<std::uint32_t, const GroupDef*>& group_by_tag) -> void
{
  out << "  static constexpr std::size_t kFieldCount = " << rules.size() << "U;\n";

  out << "  static constexpr detail::RequiredFieldMask<kFieldCount> kRequiredFields";
  const auto required_words = RequiredFieldMaskWords(rules);
  if (required_words.empty()) {
    out << "{};\n";
  } else {
    out << "{ ";
    for (std::size_t index = 0U; index < required_words.size(); ++index) {
      if (index != 0U) {
        out << ", ";
      }
      out << required_words[index] << "ULL";
    }
    out << " };\n";
  }

  out << "  static constexpr std::array<std::string_view, kFieldCount> kFieldNames";
  if (rules.empty()) {
    out << "{};\n";
  } else {
    out << "{ ";
    for (std::size_t index = 0U; index < rules.size(); ++index) {
      if (index != 0U) {
        out << ", ";
      }
      out << '"' << EmitStringLiteral(RuleDisplayName(rules[index].tag, field_by_tag, group_by_tag)) << '"';
    }
    out << " };\n";
  }
}

auto
ShapeMessageVariableName(const MessageDef& message) -> std::string
{
  return "k" + MakeIdentifier(message.name);
}

auto
ShapeGroupPathName(const GroupDef& group) -> std::string
{
  if (!group.name.empty()) {
    return CamelToSnake(group.name);
  }
  return "group_" + std::to_string(group.count_tag);
}

auto
ReserveShapeArrayName(std::unordered_map<std::string, std::size_t>& used_names,
                      std::string candidate,
                      std::uint32_t tag) -> std::string
{
  if (used_names.emplace(candidate, 1U).second) {
    return candidate;
  }

  auto disambiguated = candidate + "_" + std::to_string(tag);
  if (used_names.emplace(disambiguated, 1U).second) {
    return disambiguated;
  }

  auto& collision_count = used_names[candidate];
  for (;;) {
    ++collision_count;
    disambiguated = candidate + "_" + std::to_string(tag) + "_" + std::to_string(collision_count);
    if (used_names.emplace(disambiguated, 1U).second) {
      return disambiguated;
    }
  }
}

auto
ShapeGroupArrayName(std::string_view message_shape_name, const std::vector<std::string>& group_path) -> std::string
{
  std::string name = std::string(message_shape_name);
  for (const auto& group_name : group_path) {
    name.push_back('_');
    name.append(group_name);
  }
  name.append("_entry");
  return name;
}

auto
CountShapeNodes(const std::vector<FieldRule>& rules,
                const std::unordered_map<std::uint32_t, const FieldDef*>& field_by_tag,
                const std::unordered_map<std::uint32_t, const GroupDef*>& group_by_tag) -> std::size_t
{
  std::size_t count = 0U;
  for (const auto& rule : rules) {
    if (ShouldSkipShapeRule(rule.tag)) {
      continue;
    }
    if (group_by_tag.count(rule.tag) != 0U || field_by_tag.count(rule.tag) != 0U) {
      ++count;
    }
  }
  return count;
}

auto
EmitShapeNodeArray(std::ostringstream& out,
                   std::string_view array_name,
                   const std::vector<FieldRule>& rules,
                   const std::unordered_map<std::uint32_t, const FieldDef*>& field_by_tag,
                   const std::unordered_map<std::uint32_t, const GroupDef*>& group_by_tag,
                   std::unordered_map<std::string, std::size_t>& used_array_names,
                   std::string_view message_shape_name,
                   const std::vector<std::string>& group_path) -> void
{
  struct NestedGroupShape
  {
    std::uint32_t count_tag{ 0U };
    std::string entry_array_name;
    std::size_t entry_count{ 0U };
  };

  std::vector<NestedGroupShape> nested_groups;
  nested_groups.reserve(rules.size());

  for (const auto& rule : rules) {
    if (ShouldSkipShapeRule(rule.tag)) {
      continue;
    }

    const auto group_it = group_by_tag.find(rule.tag);
    if (group_it == group_by_tag.end()) {
      continue;
    }

    const auto& group = *group_it->second;
    const auto entry_count = CountShapeNodes(group.field_rules, field_by_tag, group_by_tag);
    std::string entry_array_name;
    if (entry_count != 0U) {
      auto nested_path = group_path;
      nested_path.push_back(ShapeGroupPathName(group));
      entry_array_name =
        ReserveShapeArrayName(used_array_names, ShapeGroupArrayName(message_shape_name, nested_path), group.count_tag);
      EmitShapeNodeArray(out,
                         entry_array_name,
                         group.field_rules,
                         field_by_tag,
                         group_by_tag,
                         used_array_names,
                         message_shape_name,
                         nested_path);
    }
    nested_groups.push_back(NestedGroupShape{
      .count_tag = group.count_tag,
      .entry_array_name = std::move(entry_array_name),
      .entry_count = entry_count,
    });
  }

  out << "inline constexpr detail::BodyNode " << array_name << "[] = {\n";
  std::size_t nested_group_index = 0U;
  for (const auto& rule : rules) {
    if (ShouldSkipShapeRule(rule.tag)) {
      continue;
    }

    const auto group_it = group_by_tag.find(rule.tag);
    if (group_it != group_by_tag.end()) {
      const auto& group = *group_it->second;
      const auto& nested = nested_groups[nested_group_index++];
      out << "  detail::BodyNode{ detail::BodyNode::Kind::kGroup, " << group.count_tag << "U, {}, "
          << ShapePresence(rule) << ", " << group.delimiter_tag << "U, ";
      if (nested.entry_count == 0U) {
        out << "nullptr, 0U";
      } else {
        out << nested.entry_array_name << ", " << nested.entry_count << "U";
      }
      out << " },\n";
      continue;
    }

    const auto field_it = field_by_tag.find(rule.tag);
    if (field_it == field_by_tag.end()) {
      continue;
    }

    const auto& field = *field_it->second;
    out << "  detail::BodyNode{ detail::BodyNode::Kind::kScalar, " << field.tag << "U, "
        << ShapeScalarKind(field.value_type) << ", " << ShapePresence(rule) << ", 0U, nullptr, 0U },\n";
  }
  out << "};\n\n";
}

auto
EmitMessageShapeData(std::ostringstream& out,
                     const NormalizedDictionary& dictionary,
                     const std::unordered_map<std::uint32_t, const FieldDef*>& field_by_tag,
                     const std::unordered_map<std::uint32_t, const GroupDef*>& group_by_tag) -> void
{
  out << "namespace shape_data {\n\n";
  std::unordered_map<std::string, std::size_t> used_array_names;

  for (const auto& message : dictionary.messages) {
    const auto shape_name = ShapeMessageVariableName(message);
    const auto body_count = CountShapeNodes(message.field_rules, field_by_tag, group_by_tag);
    const auto body_array_name = shape_name + "_body";

    if (body_count != 0U) {
      used_array_names.emplace(body_array_name, 1U);
      EmitShapeNodeArray(
        out, body_array_name, message.field_rules, field_by_tag, group_by_tag, used_array_names, shape_name, {});
    }

    out << "inline constexpr detail::MessageShape " << shape_name << " = {\n"
        << "  " << dictionary.schema_hash << "ULL,\n"
        << "  \"" << EmitStringLiteral(message.msg_type) << "\",\n";
    if (body_count == 0U) {
      out << "  nullptr,\n"
          << "  0U,\n";
    } else {
      out << "  " << body_array_name << ",\n"
          << "  static_cast<std::uint32_t>(std::size(" << body_array_name << ")),\n";
    }
    out << "  nullptr,\n"
        << "  0U,\n"
        << "};\n\n";
  }

  out << "} // namespace shape_data\n\n";
}

auto
EmitProfileSupport(std::ostringstream& out, const NormalizedDictionary& dictionary) -> void
{
  (void)dictionary;
}

auto
EmitTagConstants(std::ostringstream& out, const NormalizedDictionary& dictionary) -> void
{
  out << "namespace Tag {\n";
  for (const auto& field : dictionary.fields) {
    if (field.name.empty()) {
      continue;
    }
    out << "inline constexpr std::uint32_t " << MakeIdentifier(field.name) << " = " << field.tag << "U;\n";
  }
  out << "} // namespace Tag\n\n";
}

auto
EmitEnums(std::ostringstream& out, const NormalizedDictionary& dictionary) -> void
{
  for (const auto& field : dictionary.fields) {
    if (!HasEnums(field)) {
      continue;
    }

    const auto enum_name = EnumTypeName(field);
    const auto wire_type = EmitEnumToWireReturnType(field);
    out << "enum class " << enum_name << " : " << EnumUnderlyingType(field.value_type) << " {\n";
    for (std::uint16_t index = 0U; index < field.enum_values.size(); ++index) {
      const auto& enum_entry = field.enum_values[index];
      out << "  " << EnumValueName(enum_entry.name) << " = " << EmitEnumValueLiteral(field, enum_entry.value, index)
          << ",\n";
    }
    out << "};\n\n";

    out << "inline constexpr std::array<detail::EnumWireEntry<" << enum_name << ", " << wire_type << ">, "
        << field.enum_values.size() << "U> k" << enum_name << "Entries{ ";
    for (std::size_t index = 0U; index < field.enum_values.size(); ++index) {
      const auto& enum_entry = field.enum_values[index];
      if (index != 0U) {
        out << ", ";
      }
      out << "detail::EnumWireEntry<" << enum_name << ", " << wire_type << ">{ " << enum_name
          << "::" << EnumValueName(enum_entry.name) << ", ";
      if (field.value_type == ValueType::kString || field.value_type == ValueType::kTimestamp) {
        out << "\"" << EmitStringLiteral(enum_entry.value) << "\"";
      } else {
        out << EmitEnumValueLiteral(field, enum_entry.value, index);
      }
      out << " }";
    }
    out << " };\n\n";

    out << "[[nodiscard]] inline auto ToWire(" << enum_name << " value) -> " << wire_type << " {\n"
        << "  return detail::EnumToWire(value, k" << enum_name << "Entries);\n"
        << "}\n\n";

    out << "[[nodiscard]] inline auto TryParse" << enum_name << "(" << wire_type << " wire) -> std::optional<"
        << enum_name << "> {\n"
        << "  return detail::TryParseEnum(wire, k" << enum_name << "Entries);\n"
        << "}\n\n";
  }
}

auto
EmitProfileStruct(std::ostringstream& out, const NormalizedDictionary& dictionary, std::string_view namespace_name)
  -> void
{
  out << "class Handler;\n"
      << "class Dispatcher;\n\n"
      << "struct Profile {\n"
      << "  using Application = ::" << namespace_name << "::Handler;\n"
      << "  using Dispatcher = ::" << namespace_name << "::Dispatcher;\n"
      << "  static constexpr std::uint64_t kProfileId = " << dictionary.profile_id << "ULL;\n"
      << "  static constexpr std::uint64_t kSchemaHash = " << dictionary.schema_hash << "ULL;\n"
      << "  static constexpr std::string_view kName = \"profile_" << dictionary.profile_id << "\";\n"
      << "};\n\n";
}

auto
EmitGroupEntryType(std::ostringstream& out,
                   const GroupDef& group,
                   const std::unordered_map<std::uint32_t, const FieldDef*>& field_by_tag,
                   const std::unordered_map<std::uint32_t, const GroupDef*>& group_by_tag,
                   const std::unordered_map<std::uint32_t, GroupApiNames>& group_api_names) -> void
{
  const auto context_group_names = BuildContextGroupApiNames(group.field_rules, group_api_names, group_by_tag);
  const auto api_names = group_api_names.at(group.count_tag);
  const auto class_name = api_names.entry_type;
  out << "class " << class_name << " {\n"
      << "public:\n"
      << "  ";

  for (const auto& rule : group.field_rules) {
    const auto nested_it = group_by_tag.find(rule.tag);
    if (nested_it != group_by_tag.end()) {
      const auto& nested_group = *nested_it->second;
      const auto& nested_names = context_group_names.at(nested_group.count_tag);
      const auto& nested_add_name = nested_names.singular_snake;
      const auto& nested_name = nested_names.plural_snake;
      const auto& entry_type = nested_names.entry_type;
      out << "auto add_" << nested_add_name << "() -> " << entry_type << "& {\n"
          << "    " << nested_name << "_.emplace_back();\n"
          << "    return " << nested_name << "_.back();\n"
          << "  }\n\n"
          << "  [[nodiscard]] auto " << nested_name << "() const -> const base::InlineSplitVector<" << entry_type
          << ", 4>& {\n"
          << "    return " << nested_name << "_;\n"
          << "  }\n\n";
      continue;
    }

    const auto it = field_by_tag.find(rule.tag);
    if (it == field_by_tag.end()) {
      continue;
    }
    const auto& field = *it->second;
    const auto method_name = FieldMethodName(field);
    const auto parameter_type = EmitTypedParameterType(field);
    out << "auto " << method_name << "(" << parameter_type << " value) -> " << class_name << "& {\n"
        << "    " << EmitSetterStorageStatement(field, method_name) << "\n"
        << "    has_" << method_name << "_ = true;\n"
        << "    return *this;\n"
        << "  }\n\n";
  }

  out << "  [[nodiscard]] auto validate() const -> base::Status {\n";
  for (const auto& rule : group.field_rules) {
    const auto nested_it = group_by_tag.find(rule.tag);
    if (nested_it != group_by_tag.end()) {
      const auto nested_name = context_group_names.at(nested_it->second->count_tag).plural_snake;
      if (rule.required()) {
        out << "    if (" << nested_name << "_.empty()) {\n"
            << "      return detail::MissingRequiredField(\"" << EmitStringLiteral(group.name) << "\", \""
            << EmitStringLiteral(nested_it->second->name) << "\");\n"
            << "    }\n";
      }
      out << "    for (const auto& entry : " << nested_name << "_) {\n"
          << "      auto status = entry.validate();\n"
          << "      if (!status.ok()) {\n"
          << "        return status;\n"
          << "      }\n"
          << "    }\n";
      continue;
    }

    if (!rule.required()) {
      continue;
    }
    const auto it = field_by_tag.find(rule.tag);
    if (it == field_by_tag.end()) {
      continue;
    }
    const auto field_name = FieldMethodName(*it->second);
    out << "    if (!has_" << field_name << "_) {\n"
        << "      return detail::MissingRequiredField(\"" << EmitStringLiteral(group.name) << "\", \""
        << EmitStringLiteral(it->second->name) << "\");\n"
        << "    }\n";
  }
  out << "    return base::Status::Ok();\n"
      << "  }\n\n";

  out << "  template<class Builder>\n"
      << "  auto AppendTo(Builder& entry) const -> base::Status {\n"
      << "    auto validation = validate();\n"
      << "    if (!validation.ok()) {\n"
      << "      return validation;\n"
      << "    }\n";
  for (const auto& rule : group.field_rules) {
    const auto nested_it = group_by_tag.find(rule.tag);
    if (nested_it != group_by_tag.end()) {
      const auto& nested_group = *nested_it->second;
      const auto nested_name = context_group_names.at(nested_group.count_tag).plural_snake;
      out << "    if (!" << nested_name << "_.empty()) {\n"
          << "      auto status = detail::AppendGroupEntries(\n"
          << "        entry,\n"
          << "        " << nested_group.count_tag << "U,\n"
          << "        " << nested_name << "_,\n"
          << "        [](auto& group_entry, const auto& nested_entry) -> base::Status {\n"
          << "          return nested_entry.AppendTo(group_entry);\n"
          << "        });\n"
          << "      if (!status.ok()) {\n"
          << "        return status;\n"
          << "      }\n"
          << "    }\n";
      continue;
    }

    const auto it = field_by_tag.find(rule.tag);
    if (it == field_by_tag.end()) {
      continue;
    }
    const auto& field = *it->second;
    const auto method_name = FieldMethodName(field);
    out << "    if (has_" << method_name << "_) {\n"
        << "      detail::AddField(entry, " << field.tag << "U, " << method_name << "_);\n"
        << "    }\n";
  }
  out << "    return base::Status::Ok();\n"
      << "  }\n\n";

  out << "  auto EncodeBody(detail::BodyEncodeBuffer& buffer) const -> base::Status {\n"
      << "    auto validation = validate();\n"
      << "    if (!validation.ok()) {\n"
      << "      return validation;\n"
      << "    }\n";
  for (const auto& rule : group.field_rules) {
    const auto nested_it = group_by_tag.find(rule.tag);
    if (nested_it != group_by_tag.end()) {
      const auto& nested_group = *nested_it->second;
      const auto nested_name = context_group_names.at(nested_group.count_tag).plural_snake;
      out << "    if (!" << nested_name << "_.empty()) {\n"
          << "      buffer.append_count_field(\"" << nested_group.count_tag << "=\", " << nested_name << "_.size());\n"
          << "      for (const auto& entry : " << nested_name << "_) {\n"
          << "        auto status = entry.EncodeBody(buffer);\n"
          << "        if (!status.ok()) {\n"
          << "          return status;\n"
          << "        }\n"
          << "      }\n"
          << "    }\n";
      continue;
    }

    const auto it = field_by_tag.find(rule.tag);
    if (it == field_by_tag.end()) {
      continue;
    }
    const auto& field = *it->second;
    const auto method_name = FieldMethodName(field);
    const auto append_method = BodyEncodeAppendMethod(field.value_type);
    out << "    if (has_" << method_name << "_) {\n"
        << "      buffer." << append_method << "(\"" << field.tag << "=\", " << method_name << "_);\n"
        << "    }\n";
  }
  out << "    return base::Status::Ok();\n"
      << "  }\n\n";

  out << "private:\n";
  for (const auto& rule : group.field_rules) {
    const auto nested_it = group_by_tag.find(rule.tag);
    if (nested_it != group_by_tag.end()) {
      const auto& nested_names = context_group_names.at(nested_it->second->count_tag);
      out << "  base::InlineSplitVector<" << nested_names.entry_type << ", 4> " << nested_names.plural_snake << "_;\n";
      continue;
    }

    const auto it = field_by_tag.find(rule.tag);
    if (it == field_by_tag.end()) {
      continue;
    }
    const auto& field = *it->second;
    const auto method_name = FieldMethodName(field);
    out << "  " << QualifiedFieldStorageType(field.value_type) << " " << method_name << "_{};\n"
        << "  bool has_" << method_name << "_{ false };\n";
  }
  out << "};\n\n";
}

auto
EmitOutboundBuilderType(std::ostringstream& out,
                        const MessageDef& message,
                        const std::unordered_map<std::uint32_t, const FieldDef*>& field_by_tag,
                        const std::unordered_map<std::uint32_t, const GroupDef*>& group_by_tag,
                        const std::unordered_map<std::uint32_t, GroupApiNames>& group_api_names) -> void
{
  const auto context_group_names = BuildContextGroupApiNames(message.field_rules, group_api_names, group_by_tag);
  const auto class_name = message.name + "Builder";
  out << "class " << class_name << " {\n"
      << "public:\n"
      << "  static constexpr std::string_view kMsgType = \"" << EmitStringLiteral(message.msg_type) << "\";\n";
  EmitRequiredFieldMetadata(out, message.field_rules, field_by_tag, group_by_tag);
  out << "\n"
      << "  auto clear() -> void {\n"
      << "    presence_.clear();\n"
      << "    raw_extras_.clear();\n";
  for (const auto& rule : message.field_rules) {
    if (group_by_tag.count(rule.tag) != 0U) {
      const auto* group = group_by_tag.at(rule.tag);
      out << "    " << context_group_names.at(group->count_tag).plural_snake << "_.clear();\n";
      continue;
    }
    const auto it = field_by_tag.find(rule.tag);
    if (it == field_by_tag.end()) {
      continue;
    }
    const auto field_name = FieldMethodName(*it->second);
    out << "    has_" << field_name << "_ = false;\n";
    out << "    " << field_name << "_ = {};\n";
  }
  out << "  }\n\n";

  for (const auto& rule : message.field_rules) {
    const auto rule_index = static_cast<std::size_t>(&rule - message.field_rules.data());
    const auto group_it = group_by_tag.find(rule.tag);
    if (group_it != group_by_tag.end()) {
      const auto& group = *group_it->second;
      const auto& group_names = context_group_names.at(group.count_tag);
      const auto& group_add_name = group_names.singular_snake;
      const auto& group_name = group_names.plural_snake;
      const auto& entry_type = group_names.entry_type;
      out << "  auto add_" << group_add_name << "() -> " << entry_type << "& {\n"
          << "    " << group_name << "_.emplace_back();\n"
          << "    presence_.set(" << rule_index << "U);\n"
          << "    return " << group_name << "_.back();\n"
          << "  }\n\n"
          << "  [[nodiscard]] auto " << group_name << "() const -> const base::InlineSplitVector<" << entry_type
          << ", 4>& {\n"
          << "    return " << group_name << "_;\n"
          << "  }\n\n";
      continue;
    }

    const auto field_it = field_by_tag.find(rule.tag);
    if (field_it == field_by_tag.end()) {
      continue;
    }
    const auto& field = *field_it->second;
    const auto field_name = FieldMethodName(field);
    out << "  auto " << field_name << "(" << EmitTypedParameterType(field) << " value) -> " << class_name << "& {\n"
        << "    " << EmitSetterStorageStatement(field, field_name) << "\n"
        << "    has_" << field_name << "_ = true;\n"
        << "    presence_.set(" << rule_index << "U);\n"
        << "    return *this;\n"
        << "  }\n\n";
  }

  out << "  template<std::uint32_t Tag>\n"
      << "  auto set_tag(std::string_view value) -> " << class_name << "& {\n"
      << "    raw_extras_.emplace_back(Tag, std::string(value));\n"
      << "    return *this;\n"
      << "  }\n\n"
      << "  template<std::uint32_t Tag>\n"
      << "  auto set_tag(auto value) -> " << class_name
      << "& requires(std::integral<decltype(value)> && !std::same_as<decltype(value), char> && "
         "!std::same_as<decltype(value), bool>) {\n"
      << "    raw_extras_.emplace_back(Tag, std::to_string(value));\n"
      << "    return *this;\n"
      << "  }\n\n"
      << "  template<std::uint32_t Tag>\n"
      << "  auto set_tag(double value) -> " << class_name << "& {\n"
      << "    std::array<char, 32> buf{};\n"
      << "    const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value, "
         "std::chars_format::general, 12);\n"
      << "    raw_extras_.emplace_back(Tag, ec == std::errc() ? std::string(buf.data(), ptr) : std::string());\n"
      << "    return *this;\n"
      << "  }\n\n"
      << "  template<std::uint32_t Tag>\n"
      << "  auto set_tag(char value) -> " << class_name << "& {\n"
      << "    raw_extras_.emplace_back(Tag, std::string(1, value));\n"
      << "    return *this;\n"
      << "  }\n\n"
      << "  template<std::uint32_t Tag>\n"
      << "  auto set_tag(bool value) -> " << class_name << "& {\n"
      << "    raw_extras_.emplace_back(Tag, std::string(1, value ? 'Y' : 'N'));\n"
      << "    return *this;\n"
      << "  }\n\n";

  out << "  [[nodiscard]] auto validate() const -> base::Status {\n"
      << "    auto status = detail::ValidateRequiredFields(\"" << EmitStringLiteral(message.name)
      << "\", presence_, kRequiredFields, kFieldNames);\n"
      << "    if (!status.ok()) {\n"
      << "      return status;\n"
      << "    }\n";
  for (const auto& rule : message.field_rules) {
    const auto group_it = group_by_tag.find(rule.tag);
    if (group_it == group_by_tag.end()) {
      continue;
    }
    const auto group_name = context_group_names.at(group_it->second->count_tag).plural_snake;
    out << "    for (const auto& entry : " << group_name << "_) {\n"
        << "      auto status = entry.validate();\n"
        << "      if (!status.ok()) {\n"
        << "        return status;\n"
        << "      }\n"
        << "    }\n";
  }
  out << "    return base::Status::Ok();\n"
      << "  }\n\n";

  out << "  auto ToMessage() const -> base::Result<message::Message> {\n"
      << "    auto validation = validate();\n"
      << "    if (!validation.ok()) {\n"
      << "      return validation;\n"
      << "    }\n"
      << "    return detail::BuildOwnedMessage(kMsgType, ";

  std::size_t scalar_count = 0U;
  std::size_t group_count = 0U;
  for (const auto& rule : message.field_rules) {
    if (group_by_tag.count(rule.tag) != 0U) {
      ++group_count;
    } else if (field_by_tag.count(rule.tag) != 0U) {
      ++scalar_count;
    }
  }
  out << scalar_count << "U, " << group_count << "U, [this](auto& builder) -> base::Status {\n";

  for (const auto& rule : message.field_rules) {
    const auto group_it = group_by_tag.find(rule.tag);
    if (group_it != group_by_tag.end()) {
      const auto& group = *group_it->second;
      const auto group_name = context_group_names.at(group.count_tag).plural_snake;
      out << "    if (!" << group_name << "_.empty()) {\n"
          << "      auto status = detail::AppendGroupEntries(\n"
          << "        builder,\n"
          << "        " << group.count_tag << "U,\n"
          << "        " << group_name << "_,\n"
          << "        [](auto& group_entry, const auto& entry) -> base::Status { return entry.AppendTo(group_entry); "
             "});\n"
          << "        if (!status.ok()) {\n"
          << "          return status;\n"
          << "        }\n"
          << "    }\n";
      continue;
    }

    const auto field_it = field_by_tag.find(rule.tag);
    if (field_it == field_by_tag.end()) {
      continue;
    }
    const auto field_name = FieldMethodName(*field_it->second);
    out << "    if (has_" << field_name << "_) {\n"
        << "      detail::AddField(builder, " << field_it->second->tag << "U, " << field_name << "_);\n"
        << "    }\n";
  }
  out << "    for (const auto& [tag, value] : raw_extras_) {\n"
      << "      builder.add_string(tag, value);\n"
      << "    }\n"
      << "    return base::Status::Ok();\n"
      << "    });\n"
      << "  }\n\n";

  out << "  auto EncodeBody(detail::BodyEncodeBuffer& buffer) const -> base::Status {\n"
      << "    auto validation = validate();\n"
      << "    if (!validation.ok()) {\n"
      << "      return validation;\n"
      << "    }\n";
  for (const auto& rule : message.field_rules) {
    const auto group_it = group_by_tag.find(rule.tag);
    if (group_it != group_by_tag.end()) {
      const auto& group = *group_it->second;
      const auto group_name = context_group_names.at(group.count_tag).plural_snake;
      out << "    if (!" << group_name << "_.empty()) {\n"
          << "      buffer.append_count_field(\"" << group.count_tag << "=\", " << group_name << "_.size());\n"
          << "      for (const auto& entry : " << group_name << "_) {\n"
          << "        auto status = entry.EncodeBody(buffer);\n"
          << "        if (!status.ok()) {\n"
          << "          return status;\n"
          << "        }\n"
          << "      }\n"
          << "    }\n";
      continue;
    }

    const auto field_it = field_by_tag.find(rule.tag);
    if (field_it == field_by_tag.end()) {
      continue;
    }
    const auto& field = *field_it->second;
    const auto field_name = FieldMethodName(field);
    const auto append_method = BodyEncodeAppendMethod(field.value_type);
    out << "    if (has_" << field_name << "_) {\n"
        << "      buffer." << append_method << "(\"" << field.tag << "=\", " << field_name << "_);\n"
        << "    }\n";
  }
  out << "    for (const auto& [tag, value] : raw_extras_) {\n"
      << "      std::array<char, 16> tag_buf{};\n"
      << "      const auto [ptr, ec] = std::to_chars(tag_buf.data(), tag_buf.data() + tag_buf.size(), tag);\n"
      << "      if (ec == std::errc()) {\n"
      << "        std::string prefix(tag_buf.data(), ptr);\n"
      << "        prefix.push_back('=');\n"
      << "        buffer.append_string_field(prefix, value);\n"
      << "      }\n"
      << "    }\n"
      << "    return base::Status::Ok();\n"
      << "  }\n\n";

  out << "private:\n";
  for (const auto& rule : message.field_rules) {
    const auto group_it = group_by_tag.find(rule.tag);
    if (group_it != group_by_tag.end()) {
      const auto& group_names = context_group_names.at(group_it->second->count_tag);
      out << "  base::InlineSplitVector<" << group_names.entry_type << ", 4> " << group_names.plural_snake << "_;\n";
      continue;
    }
    const auto field_it = field_by_tag.find(rule.tag);
    if (field_it == field_by_tag.end()) {
      continue;
    }
    const auto field_name = FieldMethodName(*field_it->second);
    out << "  " << QualifiedFieldStorageType(field_it->second->value_type) << " " << field_name << "_{};\n"
        << "  bool has_" << field_name << "_{ false };\n";
  }
  out << "  detail::FieldPresence<kFieldCount> presence_{};\n";
  out << "  std::vector<std::pair<std::uint32_t, std::string>> raw_extras_;\n";
  out << "};\n\n";
}

auto
EmitOutboundMarkerType(std::ostringstream& out, const MessageDef& message) -> void
{
  const auto class_name = message.name + "Builder";
  out << "struct " << message.name << " {\n"
      << "  static constexpr std::string_view kMsgType = \"" << EmitStringLiteral(message.msg_type) << "\";\n"
      << "  using Builder = " << class_name << ";\n"
      << "  static constexpr const detail::MessageShape& kShape = shape_data::" << ShapeMessageVariableName(message)
      << ";\n"
      << "};\n\n";
}

auto
EmitGroupViewType(std::ostringstream& out,
                  const GroupDef& group,
                  const std::unordered_map<std::uint32_t, const FieldDef*>& field_by_tag,
                  const std::unordered_map<std::uint32_t, const GroupDef*>& group_by_tag,
                  const std::unordered_map<std::uint32_t, GroupApiNames>& group_api_names) -> void
{
  const auto context_group_names = BuildContextGroupApiNames(group.field_rules, group_api_names, group_by_tag);
  const auto api_names = group_api_names.at(group.count_tag);
  const auto& entry_view_name = api_names.entry_view_type;
  const auto& group_view_name = api_names.view_type;

  out << "class " << entry_view_name << " {\n"
      << "public:\n"
      << "  explicit " << entry_view_name << "(message::MessageView view)\n"
      << "    : view_(view) {}\n\n";

  for (const auto& rule : group.field_rules) {
    const auto nested_it = group_by_tag.find(rule.tag);
    if (nested_it != group_by_tag.end()) {
      const auto& nested_group = *nested_it->second;
      const auto& nested_names = context_group_names.at(nested_group.count_tag);
      out << "  [[nodiscard]] auto " << nested_names.plural_snake << "() const -> std::optional<"
          << nested_names.view_type << "> {\n"
          << "    auto group = view_.group(" << nested_group.count_tag << "U);\n"
          << "    if (!group.has_value()) {\n"
          << "      return std::nullopt;\n"
          << "    }\n"
          << "    return " << nested_names.view_type << "(*group);\n"
          << "  }\n\n";
      continue;
    }

    const auto it = field_by_tag.find(rule.tag);
    if (it == field_by_tag.end()) {
      continue;
    }
    const auto& field = *it->second;
    const auto method_name = FieldMethodName(field);
    const auto getter = MessageViewGetter(field.value_type);
    if (HasEnums(field)) {
      const auto enum_name = EnumTypeName(field);
      out << "  [[nodiscard]] auto " << method_name << "() const -> base::Result<" << enum_name << "> {\n"
          << "    return detail::ParseRequiredEnumField<" << enum_name << ">(\n"
          << "      view_." << getter << "(" << field.tag << "U),\n"
          << "      " << field.tag << "U,\n"
          << "      \"" << EmitStringLiteral(field.name) << "\",\n"
          << "      [](auto wire) { return TryParse" << enum_name << "(wire); });\n"
          << "  }\n\n";
      out << "  [[nodiscard]] auto " << method_name << "_raw() const -> " << ViewRawReturnType(field.value_type)
          << " {\n"
          << "    return view_." << getter << "(" << field.tag << "U);\n"
          << "  }\n\n";
    } else {
      out << "  [[nodiscard]] auto " << method_name << "() const -> " << ViewRawReturnType(field.value_type) << " {\n"
          << "    return view_." << getter << "(" << field.tag << "U);\n"
          << "  }\n\n";
    }
  }

  out << "private:\n"
      << "  message::MessageView view_;\n"
      << "};\n\n";

  out << "class " << group_view_name << " {\n"
      << "public:\n"
      << "  using Iterator = detail::GroupIterator<" << entry_view_name << ">;\n\n"
      << "  explicit " << group_view_name << "(message::GroupView group) : group_(group) {}\n\n"
      << "  [[nodiscard]] auto size() const -> std::size_t { return detail::GroupSize(group_); }\n"
      << "  [[nodiscard]] auto operator[](std::size_t index) const -> " << entry_view_name << " { return "
      << entry_view_name << "(group_[index]); }\n"
      << "  [[nodiscard]] auto begin() const -> Iterator { return Iterator(group_.begin()); }\n"
      << "  [[nodiscard]] auto end() const -> Iterator { return Iterator(group_.end()); }\n\n"
      << "private:\n"
      << "  message::GroupView group_{};\n"
      << "};\n\n";
}

auto
EmitInboundViewType(std::ostringstream& out,
                    const MessageDef& message,
                    const std::unordered_map<std::uint32_t, const FieldDef*>& field_by_tag,
                    const std::unordered_map<std::uint32_t, const GroupDef*>& group_by_tag,
                    const std::unordered_map<std::uint32_t, GroupApiNames>& group_api_names) -> void
{
  const auto context_group_names = BuildContextGroupApiNames(message.field_rules, group_api_names, group_by_tag);
  const auto class_name = message.name + "View";
  out << "class " << class_name << " {\n"
      << "public:\n"
      << "  static constexpr std::string_view kMsgType = \"" << EmitStringLiteral(message.msg_type) << "\";\n\n"
      << "  [[nodiscard]] static auto Bind(message::MessageView view) -> base::Result<" << class_name << "> {\n"
      << "    return detail::ValidateMsgType<" << class_name << ">(view, kMsgType);\n"
      << "  }\n\n"
      << "  [[nodiscard]] auto raw() const -> message::MessageView { return view_; }\n\n";

  for (const auto& rule : message.field_rules) {
    const auto group_it = group_by_tag.find(rule.tag);
    if (group_it != group_by_tag.end()) {
      const auto& group = *group_it->second;
      const auto& group_names = context_group_names.at(group.count_tag);
      out << "  [[nodiscard]] auto " << group_names.plural_snake << "() const -> std::optional<"
          << group_names.view_type << "> {\n"
          << "    auto group = view_.group(" << group.count_tag << "U);\n"
          << "    if (!group.has_value()) {\n"
          << "      return std::nullopt;\n"
          << "    }\n"
          << "    return " << group_names.view_type << "(*group);\n"
          << "  }\n\n";
      continue;
    }

    const auto field_it = field_by_tag.find(rule.tag);
    if (field_it == field_by_tag.end()) {
      continue;
    }
    const auto& field = *field_it->second;
    const auto method_name = FieldMethodName(field);
    const auto getter = MessageViewGetter(field.value_type);
    if (HasEnums(field)) {
      const auto enum_name = EnumTypeName(field);
      out << "  [[nodiscard]] auto " << method_name << "() const -> base::Result<" << enum_name << "> {\n"
          << "    return detail::ParseRequiredEnumField<" << enum_name << ">(\n"
          << "      view_." << getter << "(" << field.tag << "U),\n"
          << "      " << field.tag << "U,\n"
          << "      \"" << EmitStringLiteral(field.name) << "\",\n"
          << "      [](auto wire) { return TryParse" << enum_name << "(wire); });\n"
          << "  }\n\n";
      out << "  [[nodiscard]] auto " << method_name << "_raw() const -> " << ViewRawReturnType(field.value_type)
          << " {\n"
          << "    return view_." << getter << "(" << field.tag << "U);\n"
          << "  }\n\n";
    } else {
      out << "  [[nodiscard]] auto " << method_name << "() const -> " << ViewRawReturnType(field.value_type) << " {\n"
          << "    return view_." << getter << "(" << field.tag << "U);\n"
          << "  }\n\n";
    }
  }

  out << "private:\n"
      << "  friend auto detail::ValidateMsgType<" << class_name
      << ">(message::MessageView view, std::string_view expected) -> base::Result<" << class_name << ">;\n"
      << "  explicit " << class_name << "(message::MessageView view) : view_(view) {}\n"
      << "  message::MessageView view_{};\n"
      << "};\n\n";
}

auto
EmitHandlerAndDispatcher(std::ostringstream& out, const NormalizedDictionary& dictionary) -> void
{
  out << "class Dispatcher {\n"
      << "public:\n"
      << "  constexpr Dispatcher(const detail::MessageShape* const* usage_shapes = nullptr,\n"
      << "                       std::uint32_t usage_shape_count = 0U)\n"
      << "    : usage_shapes_(usage_shapes)\n"
      << "    , usage_shape_count_(usage_shape_count) {}\n"
      << "  auto Dispatch(runtime::InlineSession<Profile>& session,\n"
      << "                message::MessageView message,\n"
      << "                Handler& handler) const -> base::Status;\n"
      << "private:\n"
      << "  const detail::MessageShape* const* usage_shapes_{ nullptr };\n"
      << "  std::uint32_t usage_shape_count_{ 0U };\n"
      << "};\n\n";

  out << "class Handler : public runtime::Application<Profile> {\n"
      << "public:\n"
      << "  ~Handler() override = default;\n"
      << "  virtual auto OnUnknownMessage(runtime::InlineSession<Profile>& session,\n"
      << "                                message::MessageView message) -> base::Status {\n"
      << "    (void)session;\n"
      << "    (void)message;\n"
      << "    return base::Status::Ok();\n"
      << "  }\n";

  for (const auto& message : dictionary.messages) {
    out << "  virtual auto On" << message.name << "(runtime::InlineSession<Profile>& session, " << message.name
        << "View view) -> base::Status {\n"
        << "    (void)session;\n"
        << "    (void)view;\n"
        << "    return base::Status::Ok();\n"
        << "  }\n";
  }

  out << "};\n\n";

  out << "inline auto Dispatcher::Dispatch(runtime::InlineSession<Profile>& session,\n"
      << "                               message::MessageView message,\n"
      << "                               Handler& handler) const -> base::Status {\n"
      << "  if (!message.valid()) {\n"
      << "    return base::Status::InvalidArgument(\"message view is invalid\");\n"
      << "  }\n";
  for (const auto& message : dictionary.messages) {
    out << "  if (message.msg_type() == \"" << EmitStringLiteral(message.msg_type) << "\") {\n"
        << "    struct NoEnumValidators {\n"
        << "      static auto Validate(const detail::BodyNode& node, message::MessageView view) -> base::Status {\n"
        << "        (void)node;\n"
        << "        (void)view;\n"
        << "        return base::Status::Ok();\n"
        << "      }\n"
        << "    };\n"
        << "    return detail::DispatchToHandlerUsingUsageShapes<" << message.name << "View>(\n"
        << "      message,\n"
        << "      session,\n"
        << "      handler,\n"
        << "      &Handler::On" << message.name << ",\n"
        << "      usage_shapes_,\n"
        << "      usage_shape_count_,\n"
        << "      \"" << EmitStringLiteral(message.msg_type) << "\",\n"
        << "      NoEnumValidators{});\n"
        << "  }\n";
  }
  out << "  return handler.OnUnknownMessage(session, message);\n"
      << "}\n\n";
}

} // namespace

auto
GenerateCppApiHeader(const NormalizedDictionary& dictionary, std::string_view namespace_name)
  -> base::Result<std::string>
{
  const auto ns = namespace_name.empty() ? DefaultNamespaceName(dictionary) : std::string(namespace_name);
  const auto field_by_tag = FieldByTag(dictionary);
  const auto group_by_tag = GroupByTag(dictionary);
  const auto ordered_groups = OrderedGroupsByNesting(dictionary, group_by_tag);
  const auto group_api_names = BuildResolvedGroupApiNames(ordered_groups, field_by_tag, group_by_tag);

  std::ostringstream out;
  out << "#pragma once\n\n"
      << "#include <charconv>\n"
      << "#include <cstdint>\n"
      << "#include <optional>\n"
      << "#include <string>\n"
      << "#include <string_view>\n"
      << "#include <array>\n"
      << "#include <utility>\n"
      << "#include <vector>\n\n"
      << "#include \"nimblefix/base/inline_split_vector.h\"\n"
      << "#include \"nimblefix/base/result.h\"\n"
      << "#include \"nimblefix/base/status.h\"\n"
      << "#include \"nimblefix/generated/detail/api_support.h\"\n"
      << "#include \"nimblefix/generated/detail/message_shape.h\"\n"
      << "#include \"nimblefix/message/message_view.h\"\n"
      << "#include \"nimblefix/runtime/application.h\"\n"
      << "#include \"nimblefix/runtime/session.h\"\n\n"
      << "// Generated by nimblefix-dictgen --cpp-api. Do not edit.\n\n"
      << "namespace " << ns << " {\n\n"
      << "namespace detail = ::nimble::generated::detail;\n"
      << "namespace base = ::nimble::base;\n\n";

  EmitProfileStruct(out, dictionary, ns);
  EmitTagConstants(out, dictionary);
  EmitProfileSupport(out, dictionary);
  EmitEnums(out, dictionary);

  for (const auto* group : ordered_groups) {
    EmitGroupEntryType(out, *group, field_by_tag, group_by_tag, group_api_names);
  }
  for (const auto* group : ordered_groups) {
    EmitGroupViewType(out, *group, field_by_tag, group_by_tag, group_api_names);
  }
  for (const auto& message : dictionary.messages) {
    EmitOutboundBuilderType(out, message, field_by_tag, group_by_tag, group_api_names);
  }
  EmitMessageShapeData(out, dictionary, field_by_tag, group_by_tag);
  for (const auto& message : dictionary.messages) {
    EmitOutboundMarkerType(out, message);
  }
  for (const auto& message : dictionary.messages) {
    EmitInboundViewType(out, message, field_by_tag, group_by_tag, group_api_names);
  }
  EmitHandlerAndDispatcher(out, dictionary);

  out << "} // namespace " << ns << "\n";
  return out.str();
}

auto
WriteCppApiHeader(const std::filesystem::path& path,
                  const NormalizedDictionary& dictionary,
                  std::string_view namespace_name) -> base::Status
{
  auto header = GenerateCppApiHeader(dictionary, namespace_name);
  if (!header.ok()) {
    return header.status();
  }

  if (const auto parent = path.parent_path(); !parent.empty()) {
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
      return base::Status::IoError("unable to create api output directory: '" + parent.string() + "'");
    }
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return base::Status::IoError("unable to open generated api header for writing: '" + path.string() + "'");
  }

  out.write(header.value().data(), static_cast<std::streamsize>(header.value().size()));
  if (!out.good()) {
    return base::Status::IoError("unable to write generated api header: '" + path.string() + "'");
  }

  return base::Status::Ok();
}

} // namespace nimble::profile
