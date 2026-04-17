#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "fastfix/base/result.h"
#include "fastfix/base/status.h"
#include "fastfix/message/message.h"
#include "fastfix/profile/normalized_dictionary.h"

namespace fastfix::message {

enum class ValidationMode : std::uint8_t
{
  kStrict = 0,
  kTrusted = 1,
};

class TypedGroupView;

class TypedMessageView
{
public:
  [[nodiscard]] static auto Bind(const profile::NormalizedDictionaryView& dictionary, MessageView message)
    -> base::Result<TypedMessageView>;

  [[nodiscard]] static auto FromParts(const profile::NormalizedDictionaryView& dictionary,
                                      MessageView message,
                                      const profile::MessageDefRecord* message_def) -> TypedMessageView
  {
    return TypedMessageView(&dictionary, message, message_def);
  }

  [[nodiscard]] auto validate_required_fields(std::uint32_t* missing_tag = nullptr) const -> base::Status;
  [[nodiscard]] auto get_string(std::uint32_t tag) const -> std::optional<std::string_view>;
  [[nodiscard]] auto get_timestamp(std::uint32_t tag) const -> std::optional<std::string_view>;
  [[nodiscard]] auto get_int(std::uint32_t tag) const -> std::optional<std::int64_t>;
  [[nodiscard]] auto get_char(std::uint32_t tag) const -> std::optional<char>;
  [[nodiscard]] auto get_float(std::uint32_t tag) const -> std::optional<double>;
  [[nodiscard]] auto get_boolean(std::uint32_t tag) const -> std::optional<bool>;
  [[nodiscard]] auto group(std::uint32_t count_tag) const -> std::optional<TypedGroupView>;

private:
  TypedMessageView(const profile::NormalizedDictionaryView* dictionary,
                   MessageView message,
                   const profile::MessageDefRecord* message_def)
    : dictionary_(dictionary)
    , message_(message)
    , message_def_(message_def)
  {
  }

  const profile::NormalizedDictionaryView* dictionary_{ nullptr };
  MessageView message_{};
  const profile::MessageDefRecord* message_def_{ nullptr };
};

class TypedGroupEntryView
{
public:
  TypedGroupEntryView() = default;

  TypedGroupEntryView(const profile::NormalizedDictionaryView* dictionary,
                      MessageView entry,
                      const profile::GroupDefRecord* group_def)
    : dictionary_(dictionary)
    , entry_(entry)
    , group_def_(group_def)
  {
  }

  [[nodiscard]] auto get_string(std::uint32_t tag) const -> std::optional<std::string_view>;
  [[nodiscard]] auto get_timestamp(std::uint32_t tag) const -> std::optional<std::string_view>;
  [[nodiscard]] auto get_int(std::uint32_t tag) const -> std::optional<std::int64_t>;
  [[nodiscard]] auto get_char(std::uint32_t tag) const -> std::optional<char>;
  [[nodiscard]] auto get_float(std::uint32_t tag) const -> std::optional<double>;
  [[nodiscard]] auto get_boolean(std::uint32_t tag) const -> std::optional<bool>;
  [[nodiscard]] auto group(std::uint32_t count_tag) const -> std::optional<TypedGroupView>;

private:
  const profile::NormalizedDictionaryView* dictionary_{ nullptr };
  MessageView entry_{};
  const profile::GroupDefRecord* group_def_{ nullptr };
};

class TypedGroupView
{
public:
  class Iterator
  {
  public:
    Iterator() = default;

    Iterator(const profile::NormalizedDictionaryView* dictionary,
             GroupView group,
             const profile::GroupDefRecord* group_def,
             std::size_t index)
      : dictionary_(dictionary)
      , group_(group)
      , group_def_(group_def)
      , index_(index)
    {
    }

    [[nodiscard]] auto operator*() const -> TypedGroupEntryView;

    auto operator++() -> Iterator&
    {
      ++index_;
      return *this;
    }

    [[nodiscard]] bool operator==(const Iterator& other) const
    {
      return dictionary_ == other.dictionary_ && group_def_ == other.group_def_ && index_ == other.index_ &&
             group_.size() == other.group_.size();
    }

  private:
    const profile::NormalizedDictionaryView* dictionary_{ nullptr };
    GroupView group_{};
    const profile::GroupDefRecord* group_def_{ nullptr };
    std::size_t index_{ 0 };
  };

  TypedGroupView() = default;

  TypedGroupView(const profile::NormalizedDictionaryView* dictionary,
                 GroupView group,
                 const profile::GroupDefRecord* group_def)
    : dictionary_(dictionary)
    , group_(group)
    , group_def_(group_def)
  {
  }

  [[nodiscard]] bool valid() const { return dictionary_ != nullptr && group_.valid() && group_def_ != nullptr; }

  [[nodiscard]] std::size_t size() const { return group_.size(); }

  [[nodiscard]] auto operator[](std::size_t index) const -> TypedGroupEntryView;
  [[nodiscard]] auto begin() const -> Iterator;
  [[nodiscard]] auto end() const -> Iterator;

private:
  const profile::NormalizedDictionaryView* dictionary_{ nullptr };
  GroupView group_{};
  const profile::GroupDefRecord* group_def_{ nullptr };
};

} // namespace fastfix::message