#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/message/message_view.h"
#include "nimblefix/profile/normalized_dictionary.h"

namespace nimble::codec {

struct EncodeOptions;
struct EncodeBuffer;
class PrecompiledTemplateTable;

} // namespace nimble::codec

namespace nimble::message {

/// Mutable builder for one repeating-group entry.
///
/// Design intent: let callers populate nested groups incrementally without
/// exposing `MessageData` internals. The builder stores a stable path of group
/// tags and entry indexes, so it keeps resolving the same logical entry even if
/// parent vectors reallocate.
///
/// Performance: this is a convenience API. Field and group lookups are linear,
/// and string setters copy into owned storage. For fixed-layout hot paths,
/// prefer `FixedLayoutWriter`.
///
/// Lifetime: the builder is valid only while the root `MessageBuilder` remains
/// alive. It is not thread-safe.
class GroupEntryBuilder
{
public:
  GroupEntryBuilder() = default;

  /// Upsert a string field in this group entry.
  ///
  /// Repeated calls for the same tag replace the previous value.
  ///
  /// \param tag FIX tag number.
  /// \param value Field payload copied into owned storage.
  /// \return `*this` for fluent chaining.
  auto set_string(std::uint32_t tag, std::string_view value) -> GroupEntryBuilder&;
  /// \param tag FIX tag number.
  /// \param value Signed integer field value.
  /// \return `*this` for fluent chaining.
  auto set_int(std::uint32_t tag, std::int64_t value) -> GroupEntryBuilder&;
  /// \param tag FIX tag number.
  /// \param value Single-character field value.
  /// \return `*this` for fluent chaining.
  auto set_char(std::uint32_t tag, char value) -> GroupEntryBuilder&;
  /// \param tag FIX tag number.
  /// \param value Floating-point field value.
  /// \return `*this` for fluent chaining.
  auto set_float(std::uint32_t tag, double value) -> GroupEntryBuilder&;
  /// \param tag FIX tag number.
  /// \param value Boolean field value encoded later as `Y`/`N`.
  /// \return `*this` for fluent chaining.
  auto set_boolean(std::uint32_t tag, bool value) -> GroupEntryBuilder&;

  auto set(std::uint32_t tag, std::string_view value) -> GroupEntryBuilder& { return set_string(tag, value); }
  auto set(std::uint32_t tag, std::int64_t value) -> GroupEntryBuilder& { return set_int(tag, value); }
  auto set(std::uint32_t tag, char value) -> GroupEntryBuilder& { return set_char(tag, value); }
  auto set(std::uint32_t tag, double value) -> GroupEntryBuilder& { return set_float(tag, value); }
  auto set(std::uint32_t tag, bool value) -> GroupEntryBuilder& { return set_boolean(tag, value); }

  /// Reserve scalar-field capacity for this entry.
  ///
  /// \param count Desired total field capacity.
  /// \return `*this` for fluent chaining.
  auto reserve_fields(std::size_t count) -> GroupEntryBuilder&;

  /// Reserve nested-group capacity for this entry.
  ///
  /// \param count Desired total nested-group capacity.
  /// \return `*this` for fluent chaining.
  auto reserve_groups(std::size_t count) -> GroupEntryBuilder&;

  /// Reserve entry capacity for one nested repeating group.
  ///
  /// \param count_tag Repeating-group count tag.
  /// \param count Desired total entry capacity for that group.
  /// \return `*this` for fluent chaining.
  auto reserve_group_entries(std::uint32_t count_tag, std::size_t count) -> GroupEntryBuilder&;

  /// Append one nested repeating-group entry.
  ///
  /// Boundary condition: if this builder no longer resolves to a live root
  /// message, the returned builder is invalid and future writes are ignored.
  ///
  /// \param count_tag Repeating-group count tag.
  /// \return Builder bound to the appended entry.
  auto add_group_entry(std::uint32_t count_tag) -> GroupEntryBuilder;

private:
  friend class MessageBuilder;

  struct PathSegment
  {
    std::uint32_t count_tag{ 0 };
    std::size_t entry_index{ 0U };
  };

  explicit GroupEntryBuilder(MessageData* root)
    : root_(root)
  {
  }

  GroupEntryBuilder(MessageData* root, std::vector<PathSegment> path)
    : root_(root)
    , path_(std::move(path))
  {
  }

  auto resolve() -> MessageData*;
  auto upsert_field(FieldValue value) -> GroupEntryBuilder&;
  auto ensure_group(std::uint32_t count_tag) -> GroupData*;

  MessageData* root_{ nullptr };
  std::vector<PathSegment> path_;
};

/// Convenience builder for one owned FIX application message.
///
/// Design intent: make it easy to assemble a message in application code,
/// queue payloads across threads, and hand the result to `SessionHandle` or
/// the encoder without touching internal storage types.
///
/// Performance: field/group upserts are linear scans and string setters copy
/// into owned storage. `reset()` preserves capacity and group shells for reuse,
/// which is efficient for medium-rate paths. For the tightest send loops with a
/// stable schema, prefer `FixedLayoutWriter`.
///
/// Lifetime: `view()` and any `GroupEntryBuilder` returned from this object
/// borrow `data_` and become invalid after `build()` or destruction.
class MessageBuilder
{
public:
  /// Create a builder for one `MsgType(35)`.
  ///
  /// \param msg_type Application message type, copied into owned storage.
  explicit MessageBuilder(std::string msg_type);

  /// Borrow the current contents as a read-only view.
  ///
  /// \return Non-owning `MessageView` into the builder's storage.
  [[nodiscard]] auto view() const -> MessageView;

  /// Encode directly into an existing FIX buffer.
  ///
  /// \param dictionary Normalized dictionary used for field ordering and validation.
  /// \param options Encode options such as session header/trailer handling.
  /// \param buffer Destination buffer that receives wire bytes.
  /// \return `Ok()` on success, otherwise an encode/validation status.
  auto encode_to_buffer(const profile::NormalizedDictionaryView& dictionary,
                        const codec::EncodeOptions& options,
                        codec::EncodeBuffer* buffer) const -> base::Status;

  /// Encode directly into an existing FIX buffer using precompiled templates.
  ///
  /// \param dictionary Normalized dictionary used for field ordering and validation.
  /// \param options Encode options such as session header/trailer handling.
  /// \param buffer Destination buffer that receives wire bytes.
  /// \param precompiled Optional precompiled template table for lower encode overhead.
  /// \return `Ok()` on success, otherwise an encode/validation status.
  auto encode_to_buffer(const profile::NormalizedDictionaryView& dictionary,
                        const codec::EncodeOptions& options,
                        codec::EncodeBuffer* buffer,
                        const codec::PrecompiledTemplateTable* precompiled) const -> base::Status;

  /// Encode into owned wire bytes.
  ///
  /// \param dictionary Normalized dictionary used for field ordering and validation.
  /// \param options Encode options such as session header/trailer handling.
  /// \return Owned byte vector on success, otherwise an encode/validation status.
  auto encode(const profile::NormalizedDictionaryView& dictionary, const codec::EncodeOptions& options) const
    -> base::Result<std::vector<std::byte>>;

  /// Upsert a string field.
  ///
  /// Repeated calls for the same tag replace the previous value.
  ///
  /// \param tag FIX tag number.
  /// \param value Field payload copied into owned storage.
  /// \return `*this` for fluent chaining.
  auto set_string(std::uint32_t tag, std::string_view value) -> MessageBuilder&;
  /// \param tag FIX tag number.
  /// \param value Signed integer field value.
  /// \return `*this` for fluent chaining.
  auto set_int(std::uint32_t tag, std::int64_t value) -> MessageBuilder&;
  /// \param tag FIX tag number.
  /// \param value Single-character field value.
  /// \return `*this` for fluent chaining.
  auto set_char(std::uint32_t tag, char value) -> MessageBuilder&;
  /// \param tag FIX tag number.
  /// \param value Floating-point field value.
  /// \return `*this` for fluent chaining.
  auto set_float(std::uint32_t tag, double value) -> MessageBuilder&;
  /// \param tag FIX tag number.
  /// \param value Boolean field value encoded later as `Y`/`N`.
  /// \return `*this` for fluent chaining.
  auto set_boolean(std::uint32_t tag, bool value) -> MessageBuilder&;

  auto set(std::uint32_t tag, std::string_view value) -> MessageBuilder& { return set_string(tag, value); }
  auto set(std::uint32_t tag, std::int64_t value) -> MessageBuilder& { return set_int(tag, value); }
  auto set(std::uint32_t tag, char value) -> MessageBuilder& { return set_char(tag, value); }
  auto set(std::uint32_t tag, double value) -> MessageBuilder& { return set_float(tag, value); }
  auto set(std::uint32_t tag, bool value) -> MessageBuilder& { return set_boolean(tag, value); }

  /// Reserve scalar-field capacity.
  ///
  /// \param count Desired total field capacity.
  /// \return `*this` for fluent chaining.
  auto reserve_fields(std::size_t count) -> MessageBuilder&;

  /// Reserve top-level repeating-group capacity.
  ///
  /// \param count Desired total group capacity.
  /// \return `*this` for fluent chaining.
  auto reserve_groups(std::size_t count) -> MessageBuilder&;

  /// Reserve entry capacity for one top-level repeating group.
  ///
  /// \param count_tag Repeating-group count tag.
  /// \param count Desired total entry capacity for that group.
  /// \return `*this` for fluent chaining.
  auto reserve_group_entries(std::uint32_t count_tag, std::size_t count) -> MessageBuilder&;

  /// Append one repeating-group entry and return a builder for it.
  ///
  /// \param count_tag Repeating-group count tag.
  /// \return Builder bound to the appended entry.
  auto add_group_entry(std::uint32_t count_tag) -> GroupEntryBuilder;

  /// Move the accumulated message out of the builder.
  ///
  /// Boundary condition: after `build()`, previously borrowed views/builders are
  /// invalid and the builder is left moved-from.
  ///
  /// \return Owned `Message` containing the current fields and groups.
  auto build() && -> Message;

  /// Clear fields and groups but preserve allocated capacity for reuse.
  ///
  /// `msg_type` remains unchanged. Existing top-level groups stay allocated but
  /// their entries are cleared.
  auto reset() -> void;

private:
  auto upsert_field(FieldValue value) -> MessageBuilder&;
  auto ensure_group(std::uint32_t count_tag) -> GroupData&;

  MessageData data_;
};

} // namespace nimble::message