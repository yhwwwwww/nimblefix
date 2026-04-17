#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "fastfix/codec/fix_tags.h"
#include "fastfix/message/message.h"
#include "fastfix/profile/normalized_dictionary.h"

namespace fastfix::codec {

/// Pre-compiled per-field metadata for a known message type.
/// Replaces runtime dictionary lookups with direct array indexing.
struct CompiledFieldSlot
{
  std::uint32_t tag{ 0 };
  message::FieldTypeIndex field_type{ message::kFieldString };
  bool is_group_count{ false };
  const profile::GroupDefRecord* group_def{ nullptr };
};

/// Pre-compiled decoder for a single known message type.
/// Maps tag → slot index via a compact direct-address table,
/// replacing per-field binary searches through dictionary indices.
class CompiledMessageDecoder
{
public:
  static constexpr std::size_t kTableSize = 256U;
  static constexpr std::uint8_t kInvalidSlot = 0xFFU;
  static constexpr std::size_t kMaxSlots = 128U;

  CompiledMessageDecoder() { tag_table_.fill(kInvalidSlot); }

  /// Build a decoder for a specific message type using the dictionary.
  static auto Build(const profile::NormalizedDictionaryView& dictionary, const profile::MessageDefRecord& message_def)
    -> CompiledMessageDecoder;

  /// Look up a body field tag. Returns slot index or kInvalidSlot.
  [[nodiscard]] auto lookup(std::uint32_t tag) const -> std::uint8_t
  {
    const auto primary = static_cast<std::uint8_t>(tag & 0xFFU);
    const auto idx = tag_table_[primary];
    if (idx != kInvalidSlot && slots_[idx].tag == tag) {
      return idx;
    }
    // Collision or miss — check overflow
    for (std::uint8_t i = 0; i < overflow_count_; ++i) {
      if (overflow_tags_[i] == tag) {
        return overflow_slots_[i];
      }
    }
    return kInvalidSlot;
  }

  [[nodiscard]] auto slot(std::uint8_t index) const -> const CompiledFieldSlot& { return slots_[index]; }

  [[nodiscard]] auto slot_count() const -> std::uint8_t { return slot_count_; }

  /// Returns true if the overflow chain was exceeded during Build(),
  /// meaning some fields could not be indexed.  Callers should fall
  /// back to the generic (non-compiled) decode path when this is set.
  [[nodiscard]] auto has_overflow() const -> bool { return overflow_exceeded_; }

  /// Check if a tag is a known session-header field handled by the header
  /// extraction path.
  [[nodiscard]] static auto is_header_tag(std::uint32_t tag) -> bool
  {
    return tags::IsAggregateSessionEnvelopeTag(tag);
  }

private:
  std::array<std::uint8_t, kTableSize> tag_table_;
  std::array<CompiledFieldSlot, kMaxSlots> slots_;
  std::uint8_t slot_count_{ 0 };

  // Overflow for tag collisions (tags with same low 8 bits)
  std::array<std::uint32_t, 16> overflow_tags_{};
  std::array<std::uint8_t, 16> overflow_slots_{};
  std::uint8_t overflow_count_{ 0 };
  bool overflow_exceeded_{ false };
};

/// Table of pre-compiled decoders, one per known message type in the
/// dictionary. Constructed once at dictionary load time, shared across all
/// decode calls.
class CompiledDecoderTable
{
public:
  CompiledDecoderTable() = default;

  static auto Build(const profile::NormalizedDictionaryView& dictionary) -> CompiledDecoderTable;

  /// Find the pre-compiled decoder for a message type. Returns nullptr if
  /// unknown.
  [[nodiscard]] auto find(std::string_view msg_type) const -> const CompiledMessageDecoder*;

  [[nodiscard]] auto empty() const -> bool { return entries_.empty(); }
  [[nodiscard]] auto size() const -> std::size_t { return entries_.size(); }

private:
  struct Entry
  {
    std::string msg_type;
    CompiledMessageDecoder decoder;
  };
  std::vector<Entry> entries_;
};

} // namespace fastfix::codec
