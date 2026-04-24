#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "nimblefix/message/message_structs.h"

namespace nimble::message {

class Message;
class MessageRef;
class MessageView;
class RawGroupView;

/// Parsed FIX message plus parser-owned indexes.
///
/// Design intent: keep the parser's structural indexes and raw-byte-backed
/// string views together so downstream code can inspect a decoded message
/// without immediately copying it into owned `Message` storage.
///
/// Performance: `view()` is non-allocating and preserves parser-backed field
/// slices. Converting to `Message` with `ToOwned()` is an explicit copy.
///
/// Lifetime: any `MessageView` produced by `view()` remains valid only while
/// this object and its rebound raw storage remain alive. After moving the raw
/// frame bytes, call `RebindRaw()` before reusing existing views.
class ParsedMessage
{
public:
  ParsedMessage() = default;

  explicit ParsedMessage(ParsedMessageData data)
    : data_(std::move(data))
  {
  }

  /// Borrow this parsed message as a read-only `MessageView`.
  ///
  /// \return Non-owning view over the parsed structure and raw bytes.
  [[nodiscard]] auto view() const -> MessageView;
  [[nodiscard]] const ParsedMessageData& data() const { return data_; }

  /// Rebind all raw string views after the underlying frame bytes move.
  ///
  /// Boundary condition: `raw` must contain the same wire payload that was used
  /// to produce `data_`. Passing unrelated bytes leaves existing field/group
  /// views semantically invalid.
  ///
  /// \param raw Raw FIX frame bytes that now own the parser-backed slices.
  auto RebindRaw(std::span<const std::byte> raw) -> void;
  [[nodiscard]] auto valid() const -> bool
  {
    return !data_.msg_type.empty() || data_.root.field_count != 0U || data_.root.group_count != 0U;
  }
  [[nodiscard]] auto mutable_data() -> ParsedMessageData& { return data_; }

  /// Materialize this parsed message into owned storage.
  ///
  /// \return Deep-copied `Message` whose field strings no longer borrow the raw frame.
  [[nodiscard]] auto ToOwned() const -> Message;

private:
  ParsedMessageData data_;
};

/// Owned, mutable-style FIX application message representation.
///
/// Design intent: provide a stable heap-owned form for application code,
/// builders, and long-lived queue handoff. `MessageView` can read from this
/// object or from a `ParsedMessage` using the same accessor surface.
///
/// Performance: creating a `view()` is free; field/group lookups are
/// non-allocating and operate on the pre-built `MessageData` indexes.
///
/// Lifetime: views borrowed from this object stay valid until the `Message`
/// is destroyed or moved-from.
class Message
{
public:
  Message() = default;

  explicit Message(MessageData data)
    : data_(std::move(data))
  {
  }

  /// Borrow this owned message as a read-only view.
  ///
  /// \return Non-owning view into `data_`.
  [[nodiscard]] auto view() const -> MessageView;
  [[nodiscard]] bool valid() const { return !data_.msg_type.empty() || !data_.fields.empty() || !data_.groups.empty(); }
  [[nodiscard]] const MessageData& data() const { return data_; }

private:
  MessageData data_;
};

/// Raw tag/value pair borrowed directly from a parsed FIX frame.
///
/// `value` preserves the original wire bytes without type conversion or
/// normalization. It is useful for low-level inspection and for generated
/// typed wrappers that need exact raw slices.
struct RawFieldView
{
  std::uint32_t tag{ 0 };
  std::string_view value;
};

/// Read-only view over one parsed repeating-group entry.
///
/// Unlike `MessageView`, this API exposes raw string slices exactly as parsed
/// from the wire. Use it when you need byte-for-byte values or when the group
/// definition is not known at compile time.
///
/// Lifetime: valid only while the originating `ParsedMessage` storage remains
/// alive.
class RawGroupEntryView
{
public:
  RawGroupEntryView() = default;

  RawGroupEntryView(const ParsedMessageData* parsed, const ParsedEntryData* parsed_entry)
    : parsed_(parsed)
    , parsed_entry_(parsed_entry)
  {
  }

  [[nodiscard]] auto valid() const -> bool { return parsed_ != nullptr && parsed_entry_ != nullptr; }

  /// \return Number of scalar fields in this group entry.
  [[nodiscard]] auto field_count() const -> std::size_t;

  /// Return the `index`th raw scalar field in wire/group order.
  ///
  /// \param index Zero-based field index.
  /// \return Field view when `index < field_count()`, otherwise `std::nullopt`.
  [[nodiscard]] auto field_at(std::size_t index) const -> std::optional<RawFieldView>;

  /// Find a raw scalar field by FIX tag.
  ///
  /// \param tag FIX tag number to inspect inside this group entry.
  /// \return Raw string slice when present, otherwise `std::nullopt`.
  [[nodiscard]] auto field(std::uint32_t tag) const -> std::optional<std::string_view>;

  /// Find a nested parsed repeating group by its count tag.
  ///
  /// \param count_tag Repeating-group count tag such as `453`.
  /// \return Borrowed group view when present, otherwise `std::nullopt`.
  [[nodiscard]] auto group(std::uint32_t count_tag) const -> std::optional<RawGroupView>;

private:
  const ParsedMessageData* parsed_{ nullptr };
  const ParsedEntryData* parsed_entry_{ nullptr };
};

/// Read-only range view over a parsed repeating group.
///
/// Design intent: preserve wire-order traversal for parsed groups without
/// copying entries into owned `Message` objects. This is the lowest-cost way to
/// walk a repeating group after decode.
///
/// Performance: iteration is non-allocating. Dereferencing an iterator creates
/// a lightweight `RawGroupEntryView` wrapper.
///
/// Lifetime: invalidated when the originating `ParsedMessage` raw storage is
/// destroyed or rebound.
class RawGroupView
{
public:
  class Iterator
  {
  public:
    Iterator() = default;

    Iterator(const ParsedMessageData* parsed, const ParsedGroupFrame* group, std::uint32_t entry_index)
      : parsed_(parsed)
      , parsed_group_(group)
      , parsed_entry_index_(entry_index)
    {
    }

    [[nodiscard]] auto operator*() const -> RawGroupEntryView;

    auto operator++() -> Iterator&
    {
      if (parsed_ != nullptr && parsed_entry_index_ != kInvalidParsedIndex) {
        parsed_entry_index_ = parsed_->entries[parsed_entry_index_].next_entry;
      }
      return *this;
    }

    [[nodiscard]] bool operator==(const Iterator& other) const
    {
      return parsed_ == other.parsed_ && parsed_group_ == other.parsed_group_ &&
             parsed_entry_index_ == other.parsed_entry_index_;
    }

  private:
    const ParsedMessageData* parsed_{ nullptr };
    const ParsedGroupFrame* parsed_group_{ nullptr };
    std::uint32_t parsed_entry_index_{ kInvalidParsedIndex };
  };

  RawGroupView() = default;

  RawGroupView(const ParsedMessageData* parsed, const ParsedGroupFrame* group)
    : parsed_(parsed)
    , parsed_group_(group)
  {
  }

  [[nodiscard]] auto valid() const -> bool { return parsed_ != nullptr && parsed_group_ != nullptr; }

  /// \return The FIX count tag that owns this repeating group.
  [[nodiscard]] auto count_tag() const -> std::uint32_t
  {
    return parsed_group_ == nullptr ? 0U : parsed_group_->count_tag;
  }

  /// \return Number of entries in the group.
  [[nodiscard]] auto size() const -> std::size_t { return parsed_group_ == nullptr ? 0U : parsed_group_->entry_count; }

  /// Random-access one group entry by ordinal position.
  ///
  /// Boundary condition: out-of-range access returns an invalid
  /// `RawGroupEntryView` instead of throwing.
  ///
  /// \param index Zero-based entry index.
  /// \return Borrowed raw entry view or an invalid view on bounds failure.
  [[nodiscard]] auto operator[](std::size_t index) const -> RawGroupEntryView;
  [[nodiscard]] auto begin() const -> Iterator;
  [[nodiscard]] auto end() const -> Iterator;

private:
  const ParsedMessageData* parsed_{ nullptr };
  const ParsedGroupFrame* parsed_group_{ nullptr };
};

/// Read-only repeating-group view that works for both owned and parsed
/// messages.
///
/// Design intent: give higher-level application code a uniform traversal API
/// regardless of whether the message came from a builder or from the decoder.
///
/// Performance: iteration is non-allocating. Dereferencing a parsed entry keeps
/// borrowing parser-backed storage; dereferencing an owned entry borrows the
/// underlying `MessageData`.
class GroupView
{
public:
  class Iterator
  {
  public:
    Iterator() = default;

    Iterator(const std::vector<MessageData>* entries, std::size_t index)
      : entries_(entries)
      , index_(index)
    {
    }

    Iterator(const ParsedMessageData* parsed, const ParsedGroupFrame* group, std::uint32_t entry_index)
      : parsed_(parsed)
      , parsed_group_(group)
      , parsed_entry_index_(entry_index)
    {
    }

    [[nodiscard]] auto operator*() const -> MessageView;

    auto operator++() -> Iterator&
    {
      if (entries_ != nullptr) {
        ++index_;
        return *this;
      }
      if (parsed_ != nullptr && parsed_entry_index_ != kInvalidParsedIndex) {
        parsed_entry_index_ = parsed_->entries[parsed_entry_index_].next_entry;
      }
      return *this;
    }

    [[nodiscard]] bool operator==(const Iterator& other) const
    {
      return entries_ == other.entries_ && index_ == other.index_ && parsed_ == other.parsed_ &&
             parsed_group_ == other.parsed_group_ && parsed_entry_index_ == other.parsed_entry_index_;
    }

  private:
    const std::vector<MessageData>* entries_{ nullptr };
    std::size_t index_{ 0 };
    const ParsedMessageData* parsed_{ nullptr };
    const ParsedGroupFrame* parsed_group_{ nullptr };
    std::uint32_t parsed_entry_index_{ kInvalidParsedIndex };
  };

  GroupView() = default;

  explicit GroupView(const GroupData* group)
    : group_(group)
  {
  }

  GroupView(const ParsedMessageData* parsed, const ParsedGroupFrame* group)
    : parsed_(parsed)
    , parsed_group_(group)
  {
  }

  [[nodiscard]] bool valid() const { return group_ != nullptr || (parsed_ != nullptr && parsed_group_ != nullptr); }

  /// \return The FIX count tag associated with this group.
  [[nodiscard]] std::uint32_t count_tag() const
  {
    if (group_ != nullptr) {
      return group_->count_tag;
    }
    return parsed_group_ == nullptr ? 0U : parsed_group_->count_tag;
  }

  /// \return Number of entries currently exposed by the view.
  [[nodiscard]] std::size_t size() const
  {
    if (group_ != nullptr) {
      return group_->entries.size();
    }
    return parsed_group_ == nullptr ? 0U : parsed_group_->entry_count;
  }

  /// Random-access one entry as a `MessageView`.
  ///
  /// Boundary condition: out-of-range access returns an invalid `MessageView`.
  ///
  /// \param index Zero-based entry index.
  /// \return Borrowed entry view or an invalid view on bounds failure.
  [[nodiscard]] auto operator[](std::size_t index) const -> MessageView;
  [[nodiscard]] auto begin() const -> Iterator;
  [[nodiscard]] auto end() const -> Iterator;

private:
  const GroupData* group_{ nullptr };
  const ParsedMessageData* parsed_{ nullptr };
  const ParsedGroupFrame* parsed_group_{ nullptr };
};

/// Unified read-only view over either owned `Message` storage or parser-backed
/// `ParsedMessage` storage.
///
/// Design intent: most read paths should not care whether a message is owned or
/// still borrowing the decoder's raw frame. `MessageView` hides that storage
/// choice while keeping accessors non-allocating.
///
/// Performance promise: the accessor family (`field_at`, `find_field_view`,
/// typed getters, `group`) never copies field payloads. Missing tags and type
/// mismatches return `std::nullopt` instead of throwing.
///
/// Lifetime: a view is valid only while the originating `Message`,
/// `ParsedMessage`, or `MessageRef` keeps its backing storage alive. Views built
/// from parsed messages also become invalid after `ParsedMessage::RebindRaw()`.
class MessageView
{
public:
  MessageView() = default;

  explicit MessageView(const MessageData* data)
    : data_(data)
  {
  }

  MessageView(const ParsedMessageData* parsed, std::string_view msg_type, const ParsedEntryData* parsed_entry)
    : parsed_(parsed)
    , parsed_msg_type_(msg_type)
    , parsed_entry_(parsed_entry)
  {
  }

  [[nodiscard]] bool valid() const { return data_ != nullptr || (parsed_ != nullptr && parsed_entry_ != nullptr); }

  /// \return `MsgType(35)` from the owning message or parsed frame.
  [[nodiscard]] std::string_view msg_type() const
  {
    if (data_ != nullptr) {
      return std::string_view(data_->msg_type);
    }
    return parsed_msg_type_;
  }

  [[nodiscard]] auto fields() const -> const std::vector<FieldValue>&
  {
    static const std::vector<FieldValue> empty;
    return data_ == nullptr ? empty : data_->fields;
  }

  [[nodiscard]] auto groups() const -> const std::vector<GroupData>&
  {
    static const std::vector<GroupData> empty;
    return data_ == nullptr ? empty : data_->groups;
  }

  /// \return Number of scalar fields in this entry.
  [[nodiscard]] auto field_count() const -> std::size_t;

  /// Return the `index`th scalar field.
  ///
  /// \param index Zero-based field index in storage order.
  /// \return Field view when `index < field_count()`, otherwise `std::nullopt`.
  [[nodiscard]] auto field_at(std::size_t index) const -> std::optional<FieldView>;

  /// \return Number of repeating groups attached to this entry.
  [[nodiscard]] auto group_count() const -> std::size_t;

  /// Return the `index`th repeating group.
  ///
  /// \param index Zero-based group index in storage order.
  /// \return Group view when `index < group_count()`, otherwise `std::nullopt`.
  [[nodiscard]] auto group_at(std::size_t index) const -> std::optional<GroupView>;

  /// Find one field by FIX tag.
  ///
  /// \param tag FIX tag number.
  /// \return Borrowed field view when present, otherwise `std::nullopt`.
  [[nodiscard]] auto find_field_view(std::uint32_t tag) const -> std::optional<FieldView>;

  /// Fast existence check for a scalar field.
  ///
  /// \param tag FIX tag number.
  /// \return `true` when the tag is present.
  [[nodiscard]] bool has_field(std::uint32_t tag) const;

  /// Look up one owned field object.
  ///
  /// Boundary condition: this only succeeds for views backed by owned
  /// `MessageData`. Parsed-message views return `nullptr` even when the tag is
  /// present; use `find_field_view()` for storage-agnostic access.
  ///
  /// \param tag FIX tag number.
  /// \return Pointer to the owned field record, or `nullptr`.
  [[nodiscard]] auto find_field(std::uint32_t tag) const -> const FieldValue*;

  /// Typed getter for string-like fields.
  ///
  /// \param tag FIX tag number.
  /// \return Field value when present and string-typed, otherwise `std::nullopt`.
  [[nodiscard]] auto get_string(std::uint32_t tag) const -> std::optional<std::string_view>;

  /// Typed getter for integer fields.
  ///
  /// \param tag FIX tag number.
  /// \return Field value when present and integer-typed, otherwise `std::nullopt`.
  [[nodiscard]] auto get_int(std::uint32_t tag) const -> std::optional<std::int64_t>;

  /// Typed getter for char fields.
  ///
  /// \param tag FIX tag number.
  /// \return Field value when present and char-typed, otherwise `std::nullopt`.
  [[nodiscard]] auto get_char(std::uint32_t tag) const -> std::optional<char>;

  /// Typed getter for float fields.
  ///
  /// \param tag FIX tag number.
  /// \return Field value when present and float-typed, otherwise `std::nullopt`.
  [[nodiscard]] auto get_float(std::uint32_t tag) const -> std::optional<double>;

  /// Typed getter for boolean fields.
  ///
  /// \param tag FIX tag number.
  /// \return Field value when present and boolean-typed, otherwise `std::nullopt`.
  [[nodiscard]] auto get_boolean(std::uint32_t tag) const -> std::optional<bool>;

  /// Find a repeating group by its FIX count tag.
  ///
  /// \param count_tag Repeating-group count tag.
  /// \return Borrowed group view when present, otherwise `std::nullopt`.
  [[nodiscard]] auto group(std::uint32_t count_tag) const -> std::optional<GroupView>;

  /// Find a parsed repeating group without materializing typed field values.
  ///
  /// Boundary condition: only parser-backed views can expose raw groups. Owned
  /// messages always return `std::nullopt` here.
  ///
  /// \param count_tag Repeating-group count tag.
  /// \return Borrowed raw group view when the underlying storage is parsed and
  /// the group exists.
  [[nodiscard]] auto raw_group(std::uint32_t count_tag) const -> std::optional<RawGroupView>;
  /// Check the quick cache before falling back to the full hash lookup.
  [[nodiscard]] auto find_quick_cached(std::uint32_t tag) const -> std::optional<FieldView>;

private:
  const MessageData* data_{ nullptr };
  const ParsedMessageData* parsed_{ nullptr };
  std::string_view parsed_msg_type_;
  const ParsedEntryData* parsed_entry_{ nullptr };

  friend class MessageRef;
};

/// Deep-copy any `MessageView` into owned `Message` storage.
///
/// \param view Borrowed message view to materialize.
/// \return Owned `Message` whose lifetime no longer depends on `view`.
auto
MaterializeMessage(MessageView view) -> Message;

} // namespace nimble::message