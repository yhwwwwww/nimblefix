#include "fastfix/profile/overlay.h"

#include <algorithm>
#include <array>

namespace fastfix::profile {

namespace {

constexpr std::array<std::uint32_t, 7> kCoreSessionTags = { 8, 9, 10, 34, 35, 49, 56 };

auto
IsCoreSessionTag(std::uint32_t tag) -> bool
{
  for (const auto core : kCoreSessionTags) {
    if (tag == core) {
      return true;
    }
  }
  return false;
}

auto
FindFieldRule(std::vector<FieldRule>& rules, std::uint32_t tag)
{
  return std::find_if(rules.begin(), rules.end(), [&](const auto& existing) { return existing.tag == tag; });
}

auto
MergeFieldRules(std::vector<FieldRule>* rules, std::span<const FieldRule> overlay_rules) -> void
{
  for (std::size_t index = 0; index < overlay_rules.size(); ++index) {
    const auto& rule = overlay_rules[index];
    auto existing = FindFieldRule(*rules, rule.tag);
    if (existing != rules->end()) {
      existing->flags = rule.flags;
      continue;
    }

    auto insert_pos = rules->end();
    for (std::size_t next = index + 1U; next < overlay_rules.size(); ++next) {
      auto right_anchor = FindFieldRule(*rules, overlay_rules[next].tag);
      if (right_anchor != rules->end()) {
        insert_pos = right_anchor;
        break;
      }
    }

    if (insert_pos != rules->end()) {
      rules->insert(insert_pos, rule);
      continue;
    }

    for (std::size_t prev = index; prev > 0U; --prev) {
      auto left_anchor = FindFieldRule(*rules, overlay_rules[prev - 1U].tag);
      if (left_anchor != rules->end()) {
        rules->insert(std::next(left_anchor), rule);
        insert_pos = rules->end() - 1;
        break;
      }
    }

    if (insert_pos == rules->end()) {
      rules->push_back(rule);
    }
  }
}

} // namespace

auto
ApplyOverlay(const NormalizedDictionary& baseline, const NormalizedDictionary& overlay)
  -> base::Result<NormalizedDictionary>
{
  auto merged = baseline;

  for (const auto& overlay_field : overlay.fields) {
    auto it = std::find_if(
      merged.fields.begin(), merged.fields.end(), [&](const auto& field) { return field.tag == overlay_field.tag; });
    if (it == merged.fields.end()) {
      merged.fields.push_back(overlay_field);
      continue;
    }

    if (!overlay_field.name.empty()) {
      it->name = overlay_field.name;
    }
    if (overlay_field.value_type != ValueType::kUnknown &&
        (it->flags & static_cast<std::uint32_t>(FieldFlags::kAllowTypeOverride)) != 0U) {
      it->value_type = overlay_field.value_type;
    }
    it->flags |= overlay_field.flags;
  }

  for (const auto& overlay_msg : overlay.messages) {
    auto it = std::find_if(merged.messages.begin(), merged.messages.end(), [&](const auto& message) {
      return message.msg_type == overlay_msg.msg_type;
    });
    if (it == merged.messages.end()) {
      MessageDef message;
      message.msg_type = overlay_msg.msg_type;
      message.name = overlay_msg.name;
      message.flags = overlay_msg.flags;
      message.field_rules = overlay_msg.field_rules;
      merged.messages.push_back(std::move(message));
      continue;
    }

    if (!overlay_msg.name.empty()) {
      it->name = overlay_msg.name;
    }
    it->flags |= overlay_msg.flags;
    MergeFieldRules(&it->field_rules, overlay_msg.field_rules);
  }

  for (const auto& overlay_group : overlay.groups) {
    auto it = std::find_if(merged.groups.begin(), merged.groups.end(), [&](const auto& group) {
      return group.count_tag == overlay_group.count_tag;
    });
    if (it == merged.groups.end()) {
      GroupDef group;
      group.count_tag = overlay_group.count_tag;
      group.delimiter_tag = overlay_group.delimiter_tag;
      group.name = overlay_group.name;
      group.flags = overlay_group.flags;
      group.field_rules = overlay_group.field_rules;
      merged.groups.push_back(std::move(group));
      continue;
    }

    if (overlay_group.delimiter_tag != 0) {
      if (it->delimiter_tag != 0 && overlay_group.delimiter_tag != it->delimiter_tag &&
          (overlay_group.flags & static_cast<std::uint32_t>(GroupFlags::kAllowDelimiterOverride)) == 0U) {
        return base::Status::InvalidArgument("group overlay delimiter change requires explicit "
                                             "allow-delimiter-override flag");
      }
      it->delimiter_tag = overlay_group.delimiter_tag;
    }
    if (!overlay_group.name.empty()) {
      it->name = overlay_group.name;
    }
    it->flags |= overlay_group.flags;
    MergeFieldRules(&it->field_rules, overlay_group.field_rules);
  }

  // Merge header fields — reject core session tags in overlay.
  for (const auto& rule : overlay.header_fields) {
    if (IsCoreSessionTag(rule.tag)) {
      return base::Status::InvalidArgument("overlay may not add core session tag " + std::to_string(rule.tag) +
                                           " to header");
    }
  }
  MergeFieldRules(&merged.header_fields, overlay.header_fields);

  // Merge trailer fields — reject core session tags in overlay.
  for (const auto& rule : overlay.trailer_fields) {
    if (IsCoreSessionTag(rule.tag)) {
      return base::Status::InvalidArgument("overlay may not add core session tag " + std::to_string(rule.tag) +
                                           " to trailer");
    }
  }
  MergeFieldRules(&merged.trailer_fields, overlay.trailer_fields);

  std::sort(
    merged.fields.begin(), merged.fields.end(), [](const auto& lhs, const auto& rhs) { return lhs.tag < rhs.tag; });
  std::sort(merged.messages.begin(), merged.messages.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.msg_type < rhs.msg_type;
  });
  std::sort(merged.groups.begin(), merged.groups.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.count_tag < rhs.count_tag;
  });

  return merged;
}

} // namespace fastfix::profile
