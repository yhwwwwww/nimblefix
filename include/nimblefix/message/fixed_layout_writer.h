#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nimblefix/base/inline_split_vector.h"
#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/profile/normalized_dictionary.h"

namespace nimble::message {

class FixedLayoutWriter;

/// Pre-computed layout for a message profile, built once and shared.
/// Maps each field tag to a contiguous slot index for O(1) write access.
class FixedLayout
{
public:
  struct EncodeStep
  {
    enum class Kind : std::uint8_t
    {
      kField,
      kGroup
    };
    Kind kind;
    std::uint32_t tag;
    std::uint32_t rule_index;
    std::uint32_t slot_index;
    std::array<char, 16> prefix_data{}; // "TAG=" chars (e.g. "49=", "5001=")
    std::uint8_t prefix_length{ 0 };
    [[nodiscard]] auto prefix() const -> std::string_view { return { prefix_data.data(), prefix_length }; }
  };

  static auto Build(const profile::NormalizedDictionaryView& dictionary, std::string_view msg_type)
    -> base::Result<FixedLayout>;

  [[nodiscard]] std::string_view msg_type() const { return msg_type_; }
  [[nodiscard]] std::size_t field_count() const { return total_field_slots_; }
  [[nodiscard]] std::size_t group_count() const { return total_group_slots_; }

  /// Returns the slot index for a scalar field tag, or -1 if not in layout.
  [[nodiscard]] int slot_index(std::uint32_t tag) const;

  /// Returns the group slot index for a count_tag, or -1 if not in layout.
  [[nodiscard]] int group_slot_index(std::uint32_t count_tag) const;

private:
  friend class FixedLayoutWriter;

  struct FieldSlot
  {
    std::uint32_t tag;
    std::uint32_t slot_index;
  };
  struct GroupSlot
  {
    std::uint32_t count_tag;
    std::uint32_t slot_index;
  };

  std::string msg_type_;
  std::vector<FieldSlot> field_slots_; // sorted by tag for binary search
  std::vector<GroupSlot> group_slots_; // sorted by count_tag
  std::vector<int> tag_to_slot_;       // direct tag -> slot lookup (O(1))
  std::size_t total_field_slots_{ 0 };
  std::size_t total_group_slots_{ 0 };

  std::vector<EncodeStep> encode_order_; // fields + groups in dictionary rule order
  std::string msg_type_fragment_;        // Pre-computed "35={msg_type}\x01"
};

/// Pre-formatted bytes for a single group entry's scalar fields.
struct GroupEntryData
{
  std::string field_bytes; // Pre-formatted "tag=value\x01" for scalar fields
};

/// Encode-oriented group storage: count tag + vector of entries.
struct GroupEncodeData
{
  std::uint32_t count_tag;
  std::vector<GroupEntryData> entries;
  std::size_t active_count{ 0 }; // Number of entries in use (allows reuse without dealloc)
};

/// Fixed-layout group entry builder -- appends "tag=value\x01" directly to a
/// buffer.
class FixedGroupEntryBuilder
{
public:
  auto set_string(std::uint32_t tag, std::string_view value) -> FixedGroupEntryBuilder&;
  auto set_int(std::uint32_t tag, std::int64_t value) -> FixedGroupEntryBuilder&;
  auto set_char(std::uint32_t tag, char value) -> FixedGroupEntryBuilder&;
  auto set_float(std::uint32_t tag, double value) -> FixedGroupEntryBuilder&;
  auto set_boolean(std::uint32_t tag, bool value) -> FixedGroupEntryBuilder&;

  auto set(std::uint32_t tag, std::string_view value) -> FixedGroupEntryBuilder& { return set_string(tag, value); }
  auto set(std::uint32_t tag, std::int64_t value) -> FixedGroupEntryBuilder& { return set_int(tag, value); }
  auto set(std::uint32_t tag, char value) -> FixedGroupEntryBuilder& { return set_char(tag, value); }
  auto set(std::uint32_t tag, double value) -> FixedGroupEntryBuilder& { return set_float(tag, value); }
  auto set(std::uint32_t tag, bool value) -> FixedGroupEntryBuilder& { return set_boolean(tag, value); }

private:
  friend class FixedLayoutWriter;

  explicit FixedGroupEntryBuilder(std::string* buffer)
    : buffer_(buffer)
  {
  }

  std::string* buffer_;
};

/// Writer that uses pre-allocated slots for O(1) field writes.
/// Construct from a `FixedLayout`, populate fields, then `encode_to_buffer()`.
/// Supports reuse via `clear()` and session-constant pre-baking via
/// `bind_session()`.
class FixedLayoutWriter
{
public:
  explicit FixedLayoutWriter(const FixedLayout& layout);

  /// Reset all field data for reuse without reallocating internal buffers.
  auto clear() -> void;

  /// Pre-bake session-constant fields (BeginString, SenderCompID, TargetCompID)
  /// into a header fragment so that `encode_to_buffer()` uses memcpy instead of
  /// per-field formatting.  The binding survives `clear()`.
  auto bind_session(std::string_view begin_string, std::string_view sender_comp_id, std::string_view target_comp_id)
    -> void;

  // O(1) setters -- write directly to slot at known index.
  auto set_string(std::uint32_t tag, std::string_view value) -> FixedLayoutWriter&;
  auto set_int(std::uint32_t tag, std::int64_t value) -> FixedLayoutWriter&;
  auto set_char(std::uint32_t tag, char value) -> FixedLayoutWriter&;
  auto set_float(std::uint32_t tag, double value) -> FixedLayoutWriter&;
  auto set_boolean(std::uint32_t tag, bool value) -> FixedLayoutWriter&;

  auto set(std::uint32_t tag, std::string_view value) -> FixedLayoutWriter& { return set_string(tag, value); }
  auto set(std::uint32_t tag, std::int64_t value) -> FixedLayoutWriter& { return set_int(tag, value); }
  auto set(std::uint32_t tag, char value) -> FixedLayoutWriter& { return set_char(tag, value); }
  auto set(std::uint32_t tag, double value) -> FixedLayoutWriter& { return set_float(tag, value); }
  auto set(std::uint32_t tag, bool value) -> FixedLayoutWriter& { return set_boolean(tag, value); }

  /// Add a group entry, returning a builder for populating entry fields.
  auto add_group_entry(std::uint32_t count_tag) -> FixedGroupEntryBuilder;

  /// Reserve group entry storage for a count_tag.
  auto reserve_group_entries(std::uint32_t count_tag, std::size_t count) -> void;

  /// Encode directly to buffer.
  auto encode_to_buffer(const profile::NormalizedDictionaryView& dictionary,
                        const codec::EncodeOptions& options,
                        codec::EncodeBuffer* buffer) const -> base::Status;

  /// Encode directly to buffer while consuming pre-classified outbound extras.
  auto encode_to_buffer(const profile::NormalizedDictionaryView& dictionary,
                        const codec::EncodeOptions& options,
                        codec::EncodedOutboundExtrasView extras,
                        codec::EncodeBuffer* buffer) const -> base::Status;

  /// Encode directly to buffer with precompiled template table.
  auto encode_to_buffer(const profile::NormalizedDictionaryView& dictionary,
                        const codec::EncodeOptions& options,
                        codec::EncodeBuffer* buffer,
                        const codec::PrecompiledTemplateTable* precompiled) const -> base::Status;

  auto encode_to_buffer(const profile::NormalizedDictionaryView& dictionary,
                        const codec::EncodeOptions& options,
                        codec::EncodedOutboundExtrasView extras,
                        codec::EncodeBuffer* buffer,
                        const codec::PrecompiledTemplateTable* precompiled) const -> base::Status;

  /// Encode to owned bytes.
  auto encode(const profile::NormalizedDictionaryView& dictionary, const codec::EncodeOptions& options) const
    -> base::Result<std::vector<std::byte>>;

  auto encode(const profile::NormalizedDictionaryView& dictionary,
              const codec::EncodeOptions& options,
              codec::EncodedOutboundExtrasView extras) const -> base::Result<std::vector<std::byte>>;

private:
  struct SlotRange
  {
    std::uint32_t offset{ 0 };
    std::uint32_t length{ 0 }; // 0 means unset
  };

  struct SessionHeaderFragment
  {
    std::string header_prefix;           // "8={bs}\x01 9=0000000000\x01 35={mt}\x01"
    std::string sender_fragment;         // "49={sender}\x01"
    std::string target_fragment;         // "56={target}\x01"
    std::uint32_t static_checksum{ 0 };  // Checksum of static header bytes
    std::size_t body_length_offset{ 0 }; // Offset of placeholder within header_prefix
    std::size_t body_start_offset{ 0 };  // Offset right after "9=...\x01"
  };

  const FixedLayout* layout_;
  std::string slot_buffer_;
  base::InlineSplitVector<SlotRange, 64> slot_ranges_;
  std::vector<GroupEncodeData> groups_;
  SessionHeaderFragment session_header_;
  bool session_bound_{ false };
};

} // namespace nimble::message
