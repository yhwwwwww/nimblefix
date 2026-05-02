#include "nimblefix/tools/schema_optimizer.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nimblefix/profile/dictgen_input.h"

namespace nimble::tools {

namespace {

constexpr char kReadableDelimiter = '|';
constexpr std::uint32_t kMsgTypeTag = 35U;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct FixFieldView
{
  std::uint32_t tag{ 0 };
  std::string_view value;
};

[[nodiscard]] auto
ParseTag(std::string_view token) -> std::optional<std::uint32_t>
{
  if (token.empty()) {
    return std::nullopt;
  }

  std::uint64_t value = 0U;
  const auto* begin = token.data();
  const auto* end = token.data() + token.size();
  const auto [ptr, error] = std::from_chars(begin, end, value);
  if (error != std::errc() || ptr != end || value > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(value);
}

template<typename Fn>
auto
VisitFixFields(std::string_view raw_fix, char delimiter, Fn&& fn) -> void
{
  std::size_t begin = 0U;
  while (begin < raw_fix.size()) {
    std::size_t end = begin;
    while (end < raw_fix.size() && raw_fix[end] != delimiter && raw_fix[end] != kReadableDelimiter) {
      ++end;
    }

    const auto field = raw_fix.substr(begin, end - begin);
    const auto equals = field.find('=');
    if (equals != std::string_view::npos && equals > 0U) {
      if (auto tag = ParseTag(field.substr(0U, equals)); tag.has_value()) {
        fn(FixFieldView{ .tag = *tag, .value = field.substr(equals + 1U) });
      }
    }

    if (end == raw_fix.size()) {
      break;
    }
    begin = end + 1U;
  }
}

[[nodiscard]] auto
ExtractMsgType(std::string_view raw_fix, char delimiter) -> std::optional<std::string>
{
  std::optional<std::string> msg_type;
  VisitFixFields(raw_fix, delimiter, [&](const FixFieldView field) {
    if (!msg_type.has_value() && field.tag == kMsgTypeTag) {
      msg_type = std::string(field.value);
    }
  });
  return msg_type;
}

[[nodiscard]] auto
ValueTypeToNfdType(profile::ValueType type) -> std::string_view
{
  switch (type) {
    case profile::ValueType::kString:
      return "string";
    case profile::ValueType::kInt:
      return "int";
    case profile::ValueType::kChar:
      return "char";
    case profile::ValueType::kFloat:
      return "float";
    case profile::ValueType::kBoolean:
      return "boolean";
    case profile::ValueType::kTimestamp:
      return "timestamp";
    case profile::ValueType::kUnknown:
      break;
  }
  return "unknown";
}

[[nodiscard]] auto
RuleMode(const profile::FieldRule& rule) -> char
{
  return rule.required() ? 'r' : 'o';
}

auto
AppendFieldRules(std::ostringstream& out, const std::vector<profile::FieldRule>& rules) -> void
{
  for (std::size_t index = 0U; index < rules.size(); ++index) {
    if (index != 0U) {
      out << ',';
    }
    out << rules[index].tag << ':' << RuleMode(rules[index]);
  }
}

[[nodiscard]] auto
Fnv1a64(std::string_view data) -> std::uint64_t
{
  auto hash = kFnvOffset;
  for (const auto byte : data) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
    hash *= kFnvPrime;
  }
  return hash;
}

[[nodiscard]] auto
FindUsage(const SchemaUsageReport& report, std::string_view msg_type) -> const MessageTagUsage*
{
  const auto iterator = report.by_msg_type.find(std::string(msg_type));
  if (iterator == report.by_msg_type.end()) {
    return nullptr;
  }
  return &iterator->second;
}

[[nodiscard]] auto
FindMessage(const profile::NormalizedDictionary& dictionary, std::string_view msg_type) -> const profile::MessageDef*
{
  const auto iterator = std::find_if(dictionary.messages.begin(),
                                     dictionary.messages.end(),
                                     [&](const profile::MessageDef& message) { return message.msg_type == msg_type; });
  if (iterator == dictionary.messages.end()) {
    return nullptr;
  }
  return &*iterator;
}

[[nodiscard]] auto
ReferencedTags(const profile::NormalizedDictionary& dictionary) -> std::set<std::uint32_t>
{
  std::set<std::uint32_t> tags;
  for (const auto& rule : dictionary.header_fields) {
    tags.insert(rule.tag);
  }
  for (const auto& rule : dictionary.trailer_fields) {
    tags.insert(rule.tag);
  }
  for (const auto& message : dictionary.messages) {
    for (const auto& rule : message.field_rules) {
      tags.insert(rule.tag);
    }
  }
  for (const auto& group : dictionary.groups) {
    tags.insert(group.count_tag);
    tags.insert(group.delimiter_tag);
    for (const auto& rule : group.field_rules) {
      tags.insert(rule.tag);
    }
  }
  return tags;
}

[[nodiscard]] auto
MaxRuleTag(const std::vector<profile::FieldRule>& rules) -> std::uint32_t
{
  std::uint32_t max_tag = 0U;
  for (const auto& rule : rules) {
    max_tag = std::max(max_tag, rule.tag);
  }
  return max_tag;
}

auto
AnnotateUsageNames(SchemaUsageReport* report, const profile::NormalizedDictionary& dictionary) -> void
{
  if (report == nullptr) {
    return;
  }
  for (auto& [msg_type, usage] : report->by_msg_type) {
    if (const auto* message = FindMessage(dictionary, msg_type); message != nullptr) {
      usage.msg_name = message->name;
    }
  }
}

[[nodiscard]] auto
ShouldKeepMessageRule(const profile::FieldRule& rule, const MessageTagUsage* usage, const SchemaOptimizerConfig& config)
  -> bool
{
  if (config.always_include_tags.contains(rule.tag)) {
    return true;
  }
  if (usage == nullptr || usage->message_count == 0U) {
    return false;
  }
  const auto count_iterator = usage->tag_counts.find(rule.tag);
  if (count_iterator == usage->tag_counts.end()) {
    return false;
  }
  const auto frequency = static_cast<double>(count_iterator->second) / static_cast<double>(usage->message_count);
  return frequency >= config.min_frequency;
}

[[nodiscard]] auto
FilterMessageRules(const profile::MessageDef& message,
                   const SchemaUsageReport& report,
                   const SchemaOptimizerConfig& config) -> std::vector<profile::FieldRule>
{
  std::vector<profile::FieldRule> rules;
  const auto* usage = FindUsage(report, message.msg_type);
  for (const auto& rule : message.field_rules) {
    if (ShouldKeepMessageRule(rule, usage, config)) {
      rules.push_back(rule);
    }
  }
  return rules;
}

[[nodiscard]] auto
FilterGlobalRules(const std::vector<profile::FieldRule>& rules, const std::set<std::uint32_t>& referenced_tags)
  -> std::vector<profile::FieldRule>
{
  std::vector<profile::FieldRule> filtered;
  for (const auto& rule : rules) {
    if (referenced_tags.contains(rule.tag)) {
      filtered.push_back(rule);
    }
  }
  return filtered;
}

[[nodiscard]] auto
FilterGroups(const profile::NormalizedDictionary& base_dictionary,
             const std::set<std::uint32_t>& message_referenced_tags,
             const std::set<std::uint32_t>& observed_tags,
             const SchemaOptimizerConfig& config) -> std::vector<profile::GroupDef>
{
  std::vector<profile::GroupDef> groups;
  for (const auto& group : base_dictionary.groups) {
    if (!message_referenced_tags.contains(group.count_tag) && !config.always_include_tags.contains(group.count_tag)) {
      continue;
    }

    profile::GroupDef trimmed = group;
    trimmed.field_rules.clear();
    for (const auto& rule : group.field_rules) {
      if (observed_tags.contains(rule.tag) || message_referenced_tags.contains(rule.tag) ||
          config.always_include_tags.contains(rule.tag) || rule.tag == group.delimiter_tag) {
        trimmed.field_rules.push_back(rule);
      }
    }
    groups.push_back(std::move(trimmed));
  }
  return groups;
}

[[nodiscard]] auto
BuildNfdText(const profile::NormalizedDictionary& trimmed_dictionary) -> std::string
{
  std::ostringstream body;
  for (const auto& field : trimmed_dictionary.fields) {
    body << "field|" << field.tag << '|' << field.name << '|' << ValueTypeToNfdType(field.value_type) << '|'
         << field.flags << '\n';
    for (const auto& entry : field.enum_values) {
      body << "enum|" << field.tag << '|' << entry.value << '|' << entry.name << '\n';
    }
  }
  body << '\n';

  if (!trimmed_dictionary.header_fields.empty()) {
    body << "header|";
    AppendFieldRules(body, trimmed_dictionary.header_fields);
    body << '\n';
  }
  if (!trimmed_dictionary.trailer_fields.empty()) {
    body << "trailer|";
    AppendFieldRules(body, trimmed_dictionary.trailer_fields);
    body << '\n';
  }
  if (!trimmed_dictionary.header_fields.empty() || !trimmed_dictionary.trailer_fields.empty()) {
    body << '\n';
  }

  for (const auto& message : trimmed_dictionary.messages) {
    body << "message|" << message.msg_type << '|' << message.name << '|' << message.flags << '|';
    AppendFieldRules(body, message.field_rules);
    body << '\n';
  }
  body << '\n';

  for (const auto& group : trimmed_dictionary.groups) {
    body << "group|" << group.count_tag << '|' << group.delimiter_tag << '|' << group.name << '|' << group.flags << '|';
    AppendFieldRules(body, group.field_rules);
    body << '\n';
  }

  const auto body_text = body.str();
  const auto schema_hash = Fnv1a64(body_text);

  std::ostringstream out;
  out << "profile_id=" << trimmed_dictionary.profile_id << '\n';
  out << "schema_hash=0x" << std::hex << std::setfill('0') << std::setw(16) << schema_hash << std::dec << '\n';
  out << '\n' << body_text;
  return out.str();
}

[[nodiscard]] auto
BuildTrimmedDictionary(const SchemaUsageReport& report,
                       const profile::NormalizedDictionary& base_dictionary,
                       const SchemaOptimizerConfig& config) -> profile::NormalizedDictionary
{
  profile::NormalizedDictionary trimmed;
  trimmed.profile_id = config.profile_id != 0U ? config.profile_id : base_dictionary.profile_id;

  std::set<std::uint32_t> message_referenced_tags;
  for (const auto& message : base_dictionary.messages) {
    auto trimmed_message = message;
    trimmed_message.field_rules = FilterMessageRules(message, report, config);
    for (const auto& rule : trimmed_message.field_rules) {
      message_referenced_tags.insert(rule.tag);
    }
    trimmed.messages.push_back(std::move(trimmed_message));
  }

  trimmed.groups = FilterGroups(base_dictionary, message_referenced_tags, report.all_observed_tags, config);

  auto referenced_tags = ReferencedTags(trimmed);
  for (const auto tag : config.always_include_tags) {
    referenced_tags.insert(tag);
  }

  trimmed.header_fields = FilterGlobalRules(base_dictionary.header_fields, referenced_tags);
  trimmed.trailer_fields = FilterGlobalRules(base_dictionary.trailer_fields, referenced_tags);

  referenced_tags = ReferencedTags(trimmed);
  for (const auto& field : base_dictionary.fields) {
    if (referenced_tags.contains(field.tag)) {
      trimmed.fields.push_back(field);
    }
  }

  return trimmed;
}

[[nodiscard]] auto
TryParseGeneratedDictionary(std::string_view nfd_text) -> std::optional<profile::NormalizedDictionary>
{
  auto parsed = profile::LoadNormalizedDictionaryText(nfd_text);
  if (!parsed.ok()) {
    return std::nullopt;
  }
  return std::move(parsed).value();
}

} // namespace

auto
AnalyzeMessages(const std::vector<std::string_view>& messages, char delimiter) -> SchemaUsageReport
{
  SchemaUsageReport report;
  for (const auto raw_message : messages) {
    const auto msg_type = ExtractMsgType(raw_message, delimiter);
    if (!msg_type.has_value() || msg_type->empty()) {
      continue;
    }

    std::set<std::uint32_t> tags_in_message;
    VisitFixFields(raw_message, delimiter, [&](const FixFieldView field) { tags_in_message.insert(field.tag); });

    auto& usage = report.by_msg_type[*msg_type];
    if (usage.msg_type.empty()) {
      usage.msg_type = *msg_type;
    }
    ++usage.message_count;
    ++report.total_messages;

    for (const auto tag : tags_in_message) {
      ++usage.tag_counts[tag];
      usage.max_tag = std::max(usage.max_tag, tag);
      report.all_observed_tags.insert(tag);
      report.global_max_tag = std::max(report.global_max_tag, tag);
    }
  }
  return report;
}

auto
AnalyzeMessageBytes(const std::vector<std::span<const std::byte>>& messages, char delimiter) -> SchemaUsageReport
{
  std::vector<std::string_view> views;
  views.reserve(messages.size());
  for (const auto bytes : messages) {
    views.emplace_back(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  return AnalyzeMessages(views, delimiter);
}

auto
GenerateTrimmedNfd(const SchemaUsageReport& report,
                   const profile::NormalizedDictionary& base_dictionary,
                   const SchemaOptimizerConfig& config) -> SchemaOptimizationResult
{
  auto annotated_report = report;
  AnnotateUsageNames(&annotated_report, base_dictionary);

  const auto bounded_min_frequency = std::clamp(config.min_frequency, 0.0, 1.0);
  SchemaOptimizerConfig bounded_config = config;
  bounded_config.min_frequency = bounded_min_frequency;
  if (bounded_config.profile_id == 0U) {
    bounded_config.profile_id = base_dictionary.profile_id;
  }

  auto trimmed_dictionary = BuildTrimmedDictionary(annotated_report, base_dictionary, bounded_config);
  auto nfd_text = BuildNfdText(trimmed_dictionary);
  if (auto parsed = TryParseGeneratedDictionary(nfd_text); parsed.has_value()) {
    trimmed_dictionary = std::move(*parsed);
  }

  auto estimates = EstimateLayoutSizes(base_dictionary, trimmed_dictionary);
  std::uint64_t total_bytes_saved = 0U;
  for (const auto& estimate : estimates) {
    total_bytes_saved += estimate.tag_to_slot_bytes_saved;
  }

  return SchemaOptimizationResult{
    .trimmed_nfd_text = std::move(nfd_text),
    .layout_estimates = std::move(estimates),
    .total_bytes_saved = total_bytes_saved,
  };
}

auto
EstimateLayoutSizes(const profile::NormalizedDictionary& original, const profile::NormalizedDictionary& trimmed)
  -> std::vector<LayoutSizeEstimate>
{
  std::vector<LayoutSizeEstimate> estimates;
  estimates.reserve(original.messages.size());
  for (const auto& original_message : original.messages) {
    const auto* trimmed_message = FindMessage(trimmed, original_message.msg_type);
    const std::vector<profile::FieldRule> empty_rules;
    const auto& trimmed_rules = trimmed_message != nullptr ? trimmed_message->field_rules : empty_rules;

    const auto original_max_tag = MaxRuleTag(original_message.field_rules);
    const auto trimmed_max_tag = MaxRuleTag(trimmed_rules);
    const auto saved_slots = original_max_tag > trimmed_max_tag ? original_max_tag - trimmed_max_tag : 0U;

    estimates.push_back(LayoutSizeEstimate{
      .msg_type = original_message.msg_type,
      .original_max_tag = original_max_tag,
      .trimmed_max_tag = trimmed_max_tag,
      .original_field_slots = original_message.field_rules.size(),
      .trimmed_field_slots = trimmed_rules.size(),
      .tag_to_slot_bytes_saved = static_cast<std::size_t>(saved_slots) * sizeof(int),
    });
  }
  return estimates;
}

auto
FormatUsageReport(const SchemaUsageReport& report) -> std::string
{
  std::ostringstream out;
  out << "Schema Usage Report\n";
  out << "total_messages=" << report.total_messages << " global_max_tag=" << report.global_max_tag
      << " observed_tags=" << report.all_observed_tags.size() << "\n\n";
  out << "MsgType  Name  Messages  UniqueTags  MaxTag\n";
  for (const auto& [msg_type, usage] : report.by_msg_type) {
    out << msg_type << "  " << (usage.msg_name.empty() ? "-" : usage.msg_name) << "  " << usage.message_count << "  "
        << usage.tag_counts.size() << "  " << usage.max_tag << '\n';
  }
  return out.str();
}

auto
FormatOptimizationResult(const SchemaOptimizationResult& result) -> std::string
{
  std::ostringstream out;
  out << "Schema Optimization Result\n";
  out << "estimated_tag_to_slot_bytes_saved=" << result.total_bytes_saved << "\n\n";
  out << "MsgType  OriginalMaxTag  TrimmedMaxTag  OriginalSlots  TrimmedSlots  BytesSaved\n";
  for (const auto& estimate : result.layout_estimates) {
    out << estimate.msg_type << "  " << estimate.original_max_tag << "  " << estimate.trimmed_max_tag << "  "
        << estimate.original_field_slots << "  " << estimate.trimmed_field_slots << "  "
        << estimate.tag_to_slot_bytes_saved << '\n';
  }
  return out.str();
}

} // namespace nimble::tools
