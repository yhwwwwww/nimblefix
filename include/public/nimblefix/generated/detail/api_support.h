#pragma once

#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/advanced/message_builder.h"
#include "nimblefix/message/message_view.h"

namespace nimble::generated::detail {

template<std::size_t FieldCount>
using RequiredFieldMask = std::array<std::uint64_t, (FieldCount + 63U) / 64U>;

template<std::size_t FieldCount>
class FieldPresence
{
public:
  auto clear() -> void { words_.fill(0U); }

  auto set(std::size_t index) -> void
  {
    if constexpr (FieldCount == 0U) {
      (void)index;
      return;
    }
    words_[index / 64U] |= (std::uint64_t{ 1 } << (index % 64U));
  }

  [[nodiscard]] auto has(std::size_t index) const -> bool
  {
    if constexpr (FieldCount == 0U) {
      (void)index;
      return false;
    }
    return (words_[index / 64U] & (std::uint64_t{ 1 } << (index % 64U))) != 0U;
  }

  [[nodiscard]] auto first_missing(const RequiredFieldMask<FieldCount>& required) const -> std::optional<std::size_t>
  {
    for (std::size_t word_index = 0U; word_index < required.size(); ++word_index) {
      const auto missing = required[word_index] & ~words_[word_index];
      if (missing != 0U) {
        return (word_index * 64U) + static_cast<std::size_t>(std::countr_zero(missing));
      }
    }
    return std::nullopt;
  }

private:
  RequiredFieldMask<FieldCount> words_{};
};

template<typename Builder>
inline auto SetField(Builder& builder, std::uint32_t tag, std::string_view value) -> void
{
  builder.set_string(tag, value);
}

template<typename Builder>
inline auto AddField(Builder& builder, std::uint32_t tag, std::string_view value) -> void
{
  SetField(builder, tag, value);
}

template<typename Builder>
inline auto SetField(Builder& builder, std::uint32_t tag, std::int64_t value) -> void
{
  builder.set_int(tag, value);
}

template<typename Builder>
inline auto AddField(Builder& builder, std::uint32_t tag, std::int64_t value) -> void
{
  SetField(builder, tag, value);
}

template<typename Builder>
inline auto SetField(Builder& builder, std::uint32_t tag, char value) -> void
{
  builder.set_char(tag, value);
}

template<typename Builder>
inline auto AddField(Builder& builder, std::uint32_t tag, char value) -> void
{
  SetField(builder, tag, value);
}

template<typename Builder>
inline auto SetField(Builder& builder, std::uint32_t tag, double value) -> void
{
  builder.set_float(tag, value);
}

template<typename Builder>
inline auto AddField(Builder& builder, std::uint32_t tag, double value) -> void
{
  SetField(builder, tag, value);
}

template<typename Builder>
inline auto SetField(Builder& builder, std::uint32_t tag, bool value) -> void
{
  builder.set_boolean(tag, value);
}

template<typename Builder>
inline auto AddField(Builder& builder, std::uint32_t tag, bool value) -> void
{
  SetField(builder, tag, value);
}

template<class Builder, class Entries, class AppendEntry>
inline auto AppendGroupEntries(Builder& builder,
                               std::uint32_t count_tag,
                               const Entries& entries,
                               AppendEntry&& append_entry) -> base::Status
{
  if (entries.empty()) {
    return base::Status::Ok();
  }

  builder.reserve_group_entries(count_tag, entries.size());
  auto&& append = append_entry;
  for (const auto& entry : entries) {
    auto group_entry = builder.add_group_entry(count_tag);
    auto status = append(group_entry, entry);
    if (!status.ok()) {
      return status;
    }
  }
  return base::Status::Ok();
}

template<class BuildFields>
inline auto BuildOwnedMessage(std::string_view msg_type,
                              std::size_t scalar_field_count,
                              std::size_t group_count,
                              BuildFields&& build_fields) -> base::Result<message::Message>
{
  message::MessageBuilder builder{ std::string(msg_type) };
  builder.reserve_fields(scalar_field_count).reserve_groups(group_count);
  auto status = build_fields(builder);
  if (!status.ok()) {
    return status;
  }
  return std::move(builder).build();
}

inline auto MissingRequiredField(std::string_view owner_name, std::string_view field_name) -> base::Status
{
  return base::Status::InvalidArgument(std::string(owner_name) + " is missing required field " + std::string(field_name));
}

inline auto MissingField(std::uint32_t tag) -> base::Status
{
  return base::Status::NotFound("missing field " + std::to_string(tag));
}

template<typename Enum>
inline auto EnumParseError(std::string_view field_name) -> base::Status
{
  return base::Status::FormatError(std::string("unknown enum value for field ") + std::string(field_name));
}

template<std::size_t FieldCount>
inline auto ValidateRequiredFields(std::string_view owner_name,
                                   const FieldPresence<FieldCount>& presence,
                                   const RequiredFieldMask<FieldCount>& required_fields,
                                   const std::array<std::string_view, FieldCount>& field_names) -> base::Status
{
  const auto missing = presence.first_missing(required_fields);
  if (!missing.has_value()) {
    return base::Status::Ok();
  }
  return MissingRequiredField(owner_name, field_names[*missing]);
}

template<typename Enum, typename Wire>
struct EnumWireEntry
{
  Enum value{};
  Wire wire{};
};

template<typename Enum, typename Wire, std::size_t N>
[[nodiscard]] constexpr auto EnumToWire(Enum value, const std::array<EnumWireEntry<Enum, Wire>, N>& entries) -> Wire
{
  for (const auto& entry : entries) {
    if (entry.value == value) {
      return entry.wire;
    }
  }
  return Wire{};
}

template<typename Enum, typename Wire, std::size_t N>
[[nodiscard]] constexpr auto TryParseEnum(Wire wire, const std::array<EnumWireEntry<Enum, Wire>, N>& entries)
  -> std::optional<Enum>
{
  for (const auto& entry : entries) {
    if (entry.wire == wire) {
      return entry.value;
    }
  }
  return std::nullopt;
}

template<typename ViewType>
inline auto ValidateMsgType(message::MessageView view, std::string_view expected) -> base::Result<ViewType>
{
  if (!view.valid()) {
    return base::Status::InvalidArgument("message view is invalid");
  }
  if (view.msg_type() != expected) {
    return base::Status::InvalidArgument(std::string("expected MsgType=") + std::string(expected));
  }
  return ViewType(view);
}

template<typename ViewType>
inline auto BindMessageView(message::MessageView view, std::string_view expected) -> base::Result<ViewType>
{
  return ValidateMsgType<ViewType>(view, expected);
}

template<typename Enum, typename Wire, typename Parser>
inline auto ParseRequiredEnumField(const std::optional<Wire>& raw,
                                   std::uint32_t tag,
                                   std::string_view field_name,
                                   Parser parser) -> base::Result<Enum>
{
  if (!raw.has_value()) {
    return MissingField(tag);
  }
  auto parsed = parser(*raw);
  if (!parsed.has_value()) {
    return EnumParseError<Enum>(field_name);
  }
  return *parsed;
}

inline auto GroupSize(message::GroupView group) -> std::size_t
{
  return group.valid() ? group.size() : 0U;
}

template<class EntryView>
class GroupIterator
{
public:
  GroupIterator() = default;

  explicit GroupIterator(message::GroupView::Iterator iterator)
    : iterator_(iterator)
  {
  }

  [[nodiscard]] auto operator*() const -> EntryView { return EntryView(*iterator_); }

  auto operator++() -> GroupIterator&
  {
    ++iterator_;
    return *this;
  }

  [[nodiscard]] bool operator==(const GroupIterator& other) const { return iterator_ == other.iterator_; }

private:
  message::GroupView::Iterator iterator_{};
};

template<class Handler, class Session, class View>
using DispatchHandlerMethod = base::Status (Handler::*)(Session&, View);

template<class View, class Handler, class Session>
inline auto DispatchToHandler(message::MessageView message,
                              Session& session,
                              Handler& handler,
                              DispatchHandlerMethod<Handler, Session, View> method) -> base::Status
{
  auto bound = View::Bind(message);
  if (!bound.ok()) {
    return bound.status();
  }
  return (handler.*method)(session, std::move(bound).value());
}

/// Pre-encoded application body buffer for the typed encode fast path.
///
/// Serializes FIX tag=value fields directly into a contiguous byte buffer,
/// bypassing MessageBuilder and OwnedMessage materialization. The buffer
/// contents are passed to EncodedApplicationMessage for session-managed
/// header/trailer wrapping.
///
/// Uses inline storage (768 bytes) to avoid heap allocation for typical FIX
/// messages. Falls back to std::string for oversized bodies.
class BodyEncodeBuffer
{
  static constexpr std::size_t kInlineCapacity = 768;

public:
  auto clear() -> void
  {
    size_ = 0;
    overflow_.clear();
  }

  auto reserve(std::size_t capacity) -> void
  {
    if (capacity > kInlineCapacity && overflow_.empty()) {
      overflow_.reserve(capacity);
    }
  }

  auto append_string_field(std::string_view prefix, std::string_view value) -> void
  {
    raw_append(prefix);
    raw_append(value);
    raw_push_back('\x01');
  }

  auto append_int_field(std::string_view prefix, std::int64_t value) -> void
  {
    raw_append(prefix);
    std::array<char, 20> buf{};
    const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    if (ec == std::errc()) {
      raw_append(std::string_view(buf.data(), static_cast<std::size_t>(ptr - buf.data())));
    }
    raw_push_back('\x01');
  }

  auto append_char_field(std::string_view prefix, char value) -> void
  {
    raw_append(prefix);
    raw_push_back(value);
    raw_push_back('\x01');
  }

  auto append_float_field(std::string_view prefix, double value) -> void
  {
    raw_append(prefix);
    std::array<char, 32> buf{};
    const auto [ptr, ec] =
      std::to_chars(buf.data(), buf.data() + buf.size(), value, std::chars_format::general, 12);
    if (ec == std::errc()) {
      raw_append(std::string_view(buf.data(), static_cast<std::size_t>(ptr - buf.data())));
    }
    raw_push_back('\x01');
  }

  auto append_bool_field(std::string_view prefix, bool value) -> void
  {
    raw_append(prefix);
    raw_push_back(value ? 'Y' : 'N');
    raw_push_back('\x01');
  }

  auto append_count_field(std::string_view prefix, std::size_t count) -> void
  {
    append_int_field(prefix, static_cast<std::int64_t>(count));
  }

  [[nodiscard]] auto data() const -> std::string_view
  {
    if (!overflow_.empty()) {
      return overflow_;
    }
    return std::string_view(inline_storage_.data(), size_);
  }

  [[nodiscard]] auto bytes() const -> std::span<const std::byte>
  {
    if (!overflow_.empty()) {
      return std::span<const std::byte>(reinterpret_cast<const std::byte*>(overflow_.data()), overflow_.size());
    }
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(inline_storage_.data()), size_);
  }

private:
  auto raw_append(std::string_view sv) -> void
  {
    if (!overflow_.empty()) {
      overflow_.append(sv);
      return;
    }
    if (size_ + sv.size() > kInlineCapacity) {
      spill_to_overflow();
      overflow_.append(sv);
      return;
    }
    std::memcpy(inline_storage_.data() + size_, sv.data(), sv.size());
    size_ += sv.size();
  }

  auto raw_push_back(char c) -> void
  {
    if (!overflow_.empty()) {
      overflow_.push_back(c);
      return;
    }
    if (size_ >= kInlineCapacity) {
      spill_to_overflow();
      overflow_.push_back(c);
      return;
    }
    inline_storage_[size_++] = c;
  }

  auto spill_to_overflow() -> void
  {
    overflow_.assign(inline_storage_.data(), size_);
  }

  std::array<char, kInlineCapacity> inline_storage_{};
  std::size_t size_{ 0 };
  std::string overflow_;
};

} // namespace nimble::generated::detail
