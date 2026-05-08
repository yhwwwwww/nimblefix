#include "nimblefix/profile/normalized_dictionary.h"

#include <algorithm>
#include <limits>

namespace nimble::profile {

namespace {

auto
ValidateStringOffset(const StringTableView& string_table, std::uint32_t offset, const char* label) -> base::Status
{
  if (!string_table.string_at(offset).has_value()) {
    return base::Status::FormatError(std::string("invalid string offset for ") + label);
  }
  return base::Status::Ok();
}

auto
ValidateFieldRuleRange(std::uint32_t first, std::uint32_t count, std::size_t total, const char* label) -> base::Status
{
  if (first > total) {
    return base::Status::FormatError(std::string("invalid field-rule start for ") + label);
  }
  if (count > (total - first)) {
    return base::Status::FormatError(std::string("invalid field-rule range for ") + label);
  }
  return base::Status::Ok();
}

} // namespace

auto
NormalizedDictionaryView::FromProfile(LoadedProfile profile) -> base::Result<NormalizedDictionaryView>
{
  auto string_table = profile.string_table();
  if (!string_table.has_value()) {
    return base::Status::FormatError("artifact is missing the string-table section");
  }

  auto field_defs = profile.fixed_section<FieldDefRecord>(SectionKind::kFieldDefs);
  if (!field_defs.has_value()) {
    return base::Status::FormatError("artifact is missing required section: field-defs");
  }

  FixedSectionView<MessageDefRecord> message_defs;
  if (const auto section = profile.fixed_section<MessageDefRecord>(SectionKind::kMessageDefs); section.has_value()) {
    message_defs = *section;
  }

  FixedSectionView<GroupDefRecord> group_defs;
  if (const auto section = profile.fixed_section<GroupDefRecord>(SectionKind::kGroupDefs); section.has_value()) {
    group_defs = *section;
  }

  FixedSectionView<FieldRuleRecord> message_field_rules;
  if (const auto section = profile.fixed_section<FieldRuleRecord>(SectionKind::kMessageFieldRules);
      section.has_value()) {
    message_field_rules = *section;
  }

  FixedSectionView<FieldRuleRecord> group_field_rules;
  if (const auto section = profile.fixed_section<FieldRuleRecord>(SectionKind::kGroupFieldRules); section.has_value()) {
    group_field_rules = *section;
  }

  if (!message_defs.empty() && !message_field_rules.valid()) {
    return base::Status::FormatError("artifact has message definitions without message field rules");
  }

  if (!group_defs.empty() && !group_field_rules.valid()) {
    return base::Status::FormatError("artifact has group definitions without group field rules");
  }

  for (const auto& field : field_defs->entries()) {
    auto status = ValidateStringOffset(*string_table, field.name_offset, "field definition");
    if (!status.ok()) {
      return status;
    }
  }

  for (const auto& message : message_defs.entries()) {
    auto status = ValidateStringOffset(*string_table, message.name_offset, "message definition");
    if (!status.ok()) {
      return status;
    }

    status = ValidateStringOffset(*string_table, message.msg_type_offset, "message type");
    if (!status.ok()) {
      return status;
    }

    status = ValidateFieldRuleRange(
      message.first_field_rule, message.field_rule_count, message_field_rules.size(), "message definition");
    if (!status.ok()) {
      return status;
    }
  }

  for (const auto& group : group_defs.entries()) {
    auto status = ValidateStringOffset(*string_table, group.name_offset, "group definition");
    if (!status.ok()) {
      return status;
    }

    status = ValidateFieldRuleRange(
      group.first_field_rule, group.field_rule_count, group_field_rules.size(), "group definition");
    if (!status.ok()) {
      return status;
    }
  }

  NormalizedDictionaryView view;
  view.profile_ = std::move(profile);
  view.string_table_ = *string_table;
  view.field_defs_ = *field_defs;
  view.message_defs_ = message_defs;
  view.group_defs_ = group_defs;
  view.message_field_rules_ = message_field_rules;
  view.group_field_rules_ = group_field_rules;

  // Load header field rules (optional — many artifacts omit them).
  if (const auto section = view.profile_.fixed_section<FieldRuleRecord>(SectionKind::kHeaderFieldRules);
      section.has_value()) {
    view.header_field_rules_ = *section;
  }

  // Load trailer field rules (optional).
  if (const auto section = view.profile_.fixed_section<FieldRuleRecord>(SectionKind::kTrailerFieldRules);
      section.has_value()) {
    view.trailer_field_rules_ = *section;
  }

  // Load enum values (optional).
  if (const auto section = view.profile_.fixed_section<EnumValueRecord>(SectionKind::kEnumValues);
      section.has_value()) {
    view.enum_values_ = *section;
  }

  // Build field index sorted by tag for binary search.
  {
    const auto& entries = view.field_defs_.entries();
    view.field_index_.resize(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
      view.field_index_[i] = { entries[i].tag, static_cast<std::uint32_t>(i) };
    }
    std::sort(view.field_index_.begin(), view.field_index_.end(), [](const TagIndexEntry& a, const TagIndexEntry& b) {
      return a.key < b.key;
    });
  }

  // Build group index sorted by count_tag for binary search.
  {
    const auto& entries = view.group_defs_.entries();
    view.group_index_.resize(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
      view.group_index_[i] = { entries[i].count_tag, static_cast<std::uint32_t>(i) };
    }
    std::sort(view.group_index_.begin(), view.group_index_.end(), [](const TagIndexEntry& a, const TagIndexEntry& b) {
      return a.key < b.key;
    });
  }

  // Build group count-tag bitmap for O(1) is_group_count_tag() queries.
  {
    view.group_count_tag_bitmap_.fill(0U);
    for (const auto& group : group_defs.entries()) {
      const auto tag = group.count_tag;
      if (tag < kGroupBitmapBits) {
        view.group_count_tag_bitmap_[tag / 64U] |= (std::uint64_t{ 1 } << (tag % 64U));
      }
    }
  }

  // Build message type index sorted by msg_type for binary search.
  {
    const auto& entries = view.message_defs_.entries();
    view.message_index_.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
      auto type = view.string_table_.string_at(entries[i].msg_type_offset);
      if (type.has_value()) {
        view.message_index_.push_back({ *type, static_cast<std::uint32_t>(i) });
      }
    }
    std::sort(view.message_index_.begin(),
              view.message_index_.end(),
              [](const MsgTypeIndexEntry& a, const MsgTypeIndexEntry& b) { return a.msg_type < b.msg_type; });
  }

  // Build sorted message rule tags (parallel to message_field_rules_).
  {
    const auto& rules = view.message_field_rules_.entries();
    view.sorted_message_rules_.resize(rules.size());
    for (const auto& message : view.message_defs_.entries()) {
      auto* base = view.sorted_message_rules_.data() + message.first_field_rule;
      for (std::uint32_t i = 0; i < message.field_rule_count; ++i) {
        base[i] = { rules[message.first_field_rule + i].tag, i };
      }
      std::sort(base, base + message.field_rule_count, [](const TagRuleEntry& a, const TagRuleEntry& b) {
        return a.tag < b.tag;
      });
    }
  }

  // Build sorted group rule tags (parallel to group_field_rules_).
  {
    const auto& rules = view.group_field_rules_.entries();
    view.sorted_group_rules_.resize(rules.size());
    for (const auto& group : view.group_defs_.entries()) {
      auto* base = view.sorted_group_rules_.data() + group.first_field_rule;
      for (std::uint32_t i = 0; i < group.field_rule_count; ++i) {
        base[i] = { rules[group.first_field_rule + i].tag, i };
      }
      std::sort(base, base + group.field_rule_count, [](const TagRuleEntry& a, const TagRuleEntry& b) {
        return a.tag < b.tag;
      });
    }
  }

  // Build direct-address lookup table for fast O(1) field lookups by tag.
  {
    view.field_direct_lookup_.fill(kNoEntry);
    const auto& entries = view.field_defs_.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const auto tag = entries[i].tag;
      if (tag < kDirectLookupSize && i <= std::numeric_limits<std::uint16_t>::max()) {
        view.field_direct_lookup_[tag] = static_cast<std::uint16_t>(i);
      }
    }
  }

  // Build per-field enum char bitmaps for O(1) single-char enum validation.
  {
    const auto& field_entries = view.field_defs_.entries();
    const auto enum_vals = view.enum_values_.entries();
    view.enum_bitmaps_.resize(field_entries.size());
    for (std::size_t i = 0; i < field_entries.size(); ++i) {
      const auto& field = field_entries[i];
      if (field.enum_count == 0U) {
        continue;
      }
      const auto begin = static_cast<std::size_t>(field.enum_offset);
      const auto count = static_cast<std::size_t>(field.enum_count);
      if (begin >= enum_vals.size()) {
        continue;
      }
      const auto end = std::min(begin + count, enum_vals.size());
      bool all_single = true;
      for (std::size_t idx = begin; idx < end; ++idx) {
        const auto val = view.string_table_.string_at(enum_vals[idx].value_offset);
        if (!val.has_value() || val->size() != 1U) {
          all_single = false;
          break;
        }
      }
      if (all_single) {
        auto& bitmap = view.enum_bitmaps_[i];
        bitmap.all_single_char = true;
        for (std::size_t idx = begin; idx < end; ++idx) {
          const auto val = view.string_table_.string_at(enum_vals[idx].value_offset);
          const auto ch = static_cast<std::uint8_t>((*val)[0]);
          bitmap.bits[ch >> 3U] |= static_cast<std::uint8_t>(1U << (ch & 7U));
        }
      }
    }
  }

  return view;
}

auto
NormalizedDictionaryView::find_field(std::uint32_t tag) const -> const FieldDefRecord*
{
  // O(1) direct-address lookup for the common case (tags < 10000).
  if (tag < kDirectLookupSize) {
    const auto slot = field_direct_lookup_[tag];
    if (slot != kNoEntry) {
      return &field_defs_.entries()[slot];
    }
    return nullptr;
  }
  // Fallback to O(log N) binary search for rare large tags.
  auto it = std::lower_bound(
    field_index_.begin(), field_index_.end(), tag, [](const TagIndexEntry& e, std::uint32_t t) { return e.key < t; });
  if (it != field_index_.end() && it->key == tag) {
    return &field_defs_.entries()[it->index];
  }
  return nullptr;
}

auto
NormalizedDictionaryView::find_message(std::string_view msg_type) const -> const MessageDefRecord*
{
  auto it = std::lower_bound(message_index_.begin(),
                             message_index_.end(),
                             msg_type,
                             [](const MsgTypeIndexEntry& e, std::string_view t) { return e.msg_type < t; });
  if (it != message_index_.end() && it->msg_type == msg_type) {
    return &message_defs_.entries()[it->index];
  }
  return nullptr;
}

auto
NormalizedDictionaryView::find_group(std::uint32_t count_tag) const -> const GroupDefRecord*
{
  auto it =
    std::lower_bound(group_index_.begin(), group_index_.end(), count_tag, [](const TagIndexEntry& e, std::uint32_t t) {
      return e.key < t;
    });
  if (it != group_index_.end() && it->key == count_tag) {
    return &group_defs_.entries()[it->index];
  }
  return nullptr;
}

auto
NormalizedDictionaryView::message_rule_allows_tag(const MessageDefRecord& record, std::uint32_t tag) const -> bool
{
  const auto* base = sorted_message_rules_.data() + record.first_field_rule;
  const auto* end = base + record.field_rule_count;
  auto it = std::lower_bound(base, end, tag, [](const TagRuleEntry& e, std::uint32_t t) { return e.tag < t; });
  return it != end && it->tag == tag;
}

auto
NormalizedDictionaryView::group_rule_allows_tag(const GroupDefRecord& record, std::uint32_t tag) const -> bool
{
  const auto* base = sorted_group_rules_.data() + record.first_field_rule;
  const auto* end = base + record.field_rule_count;
  auto it = std::lower_bound(base, end, tag, [](const TagRuleEntry& e, std::uint32_t t) { return e.tag < t; });
  return it != end && it->tag == tag;
}

auto
NormalizedDictionaryView::message_rule_index(const MessageDefRecord& record, std::uint32_t tag) const -> int
{
  const auto* base = sorted_message_rules_.data() + record.first_field_rule;
  const auto* end = base + record.field_rule_count;
  auto it = std::lower_bound(base, end, tag, [](const TagRuleEntry& e, std::uint32_t t) { return e.tag < t; });
  if (it != end && it->tag == tag) {
    return static_cast<int>(it->original_index);
  }
  return -1;
}

auto
NormalizedDictionaryView::group_rule_index(const GroupDefRecord& record, std::uint32_t tag) const -> int
{
  const auto* base = sorted_group_rules_.data() + record.first_field_rule;
  const auto* end = base + record.field_rule_count;
  auto it = std::lower_bound(base, end, tag, [](const TagRuleEntry& e, std::uint32_t t) { return e.tag < t; });
  if (it != end && it->tag == tag) {
    return static_cast<int>(it->original_index);
  }
  return -1;
}

auto
NormalizedDictionaryView::message_field_rules(const MessageDefRecord& record) const -> std::span<const FieldRuleRecord>
{
  return message_field_rules_.entries().subspan(record.first_field_rule, record.field_rule_count);
}

auto
NormalizedDictionaryView::group_field_rules(const GroupDefRecord& record) const -> std::span<const FieldRuleRecord>
{
  return group_field_rules_.entries().subspan(record.first_field_rule, record.field_rule_count);
}

auto
NormalizedDictionaryView::is_enum_value_allowed(const FieldDefRecord& field_def, std::string_view value) const -> bool
{
  if (field_def.enum_count == 0U || value.empty()) {
    return true;
  }

  const auto field_index =
    static_cast<std::size_t>(&field_def - field_defs_.entries().data());
  if (field_index < enum_bitmaps_.size() && enum_bitmaps_[field_index].all_single_char) {
    if (value.size() != 1U) {
      return false;
    }
    const auto ch = static_cast<std::uint8_t>(value[0]);
    return (enum_bitmaps_[field_index].bits[ch >> 3U] & (1U << (ch & 7U))) != 0U;
  }

  const auto enum_vals = enum_values_.entries();
  const auto begin = static_cast<std::size_t>(field_def.enum_offset);
  const auto count = static_cast<std::size_t>(field_def.enum_count);
  if (begin >= enum_vals.size()) {
    return true;
  }
  const auto end = std::min(begin + count, enum_vals.size());
  for (std::size_t index = begin; index < end; ++index) {
    const auto allowed = string_table_.string_at(enum_vals[index].value_offset);
    if (allowed.has_value() && *allowed == value) {
      return true;
    }
  }
  return false;
}

} // namespace nimble::profile
