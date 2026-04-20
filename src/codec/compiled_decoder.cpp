#include "nimblefix/codec/compiled_decoder.h"

#include <algorithm>

namespace nimble::codec {

namespace {

auto
FieldTypeFromValueType(profile::ValueType vt) -> message::FieldTypeIndex
{
  switch (vt) {
    case profile::ValueType::kInt:
      return message::kFieldInt;
    case profile::ValueType::kChar:
      return message::kFieldChar;
    case profile::ValueType::kFloat:
      return message::kFieldFloat;
    case profile::ValueType::kBoolean:
      return message::kFieldBoolean;
    default:
      return message::kFieldString;
  }
}

} // namespace

auto
CompiledMessageDecoder::Build(const profile::NormalizedDictionaryView& dictionary,
                              const profile::MessageDefRecord& message_def) -> CompiledMessageDecoder
{
  CompiledMessageDecoder decoder;
  const auto rules = dictionary.message_field_rules(message_def);

  for (const auto& rule : rules) {
    const auto tag = rule.tag;

    // Skip tags that are handled by the header-extraction path in the decode
    // loop.
    if (is_header_tag(tag)) {
      continue;
    }

    if (decoder.slot_count_ >= kMaxSlots) {
      break;
    }

    const auto slot_idx = decoder.slot_count_;
    auto& slot = decoder.slots_[slot_idx];
    slot.tag = tag;

    // Resolve field type from dictionary definition.
    const auto* field_def = dictionary.find_field(tag);
    if (field_def != nullptr) {
      slot.field_type = FieldTypeFromValueType(static_cast<profile::ValueType>(field_def->value_type));
    }

    // Check if this tag is a group count field.
    const auto* group_def = dictionary.find_group(tag);
    if (group_def != nullptr) {
      slot.is_group_count = true;
      slot.group_def = group_def;
    }

    // Insert into the direct-address table or overflow.
    const auto primary = static_cast<std::uint8_t>(tag & 0xFFU);
    if (decoder.tag_table_[primary] == kInvalidSlot) {
      decoder.tag_table_[primary] = slot_idx;
    } else {
      // Collision — use overflow list.
      if (decoder.overflow_count_ < decoder.overflow_tags_.size()) {
        decoder.overflow_tags_[decoder.overflow_count_] = tag;
        decoder.overflow_slots_[decoder.overflow_count_] = slot_idx;
        ++decoder.overflow_count_;
      } else {
        decoder.overflow_exceeded_ = true;
      }
    }

    ++decoder.slot_count_;
  }

  return decoder;
}

auto
CompiledDecoderTable::Build(const profile::NormalizedDictionaryView& dictionary) -> CompiledDecoderTable
{
  CompiledDecoderTable table;
  const auto messages = dictionary.messages();
  table.entries_.reserve(messages.size());

  for (const auto& msg_def : messages) {
    auto msg_type_opt = dictionary.message_type(msg_def);
    if (!msg_type_opt.has_value()) {
      continue;
    }
    table.entries_.push_back(Entry{
      .msg_type = std::string(msg_type_opt.value()),
      .decoder = CompiledMessageDecoder::Build(dictionary, msg_def),
    });
  }

  // Sort by msg_type for binary search in find().
  std::sort(table.entries_.begin(), table.entries_.end(), [](const Entry& a, const Entry& b) {
    return a.msg_type < b.msg_type;
  });

  return table;
}

auto
CompiledDecoderTable::find(std::string_view msg_type) const -> const CompiledMessageDecoder*
{
  auto it = std::lower_bound(
    entries_.begin(), entries_.end(), msg_type, [](const Entry& e, std::string_view t) { return e.msg_type < t; });
  if (it != entries_.end() && it->msg_type == msg_type) {
    return &it->decoder;
  }
  return nullptr;
}

} // namespace nimble::codec
