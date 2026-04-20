#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "nimblefix/base/inline_split_vector.h"
#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/profile/normalized_dictionary.h"

namespace nimble::codec {

struct EncodeOptions;
struct EncodeBuffer;
class PrecompiledTemplateTable;

} // namespace nimble::codec

namespace nimble::message {

/// Type index for field values — matches
/// std::variant<string,int64,double,char,bool> index.
using FieldTypeIndex = std::uint8_t;
inline constexpr FieldTypeIndex kFieldString = 0;
inline constexpr FieldTypeIndex kFieldInt = 1;
inline constexpr FieldTypeIndex kFieldFloat = 2;
inline constexpr FieldTypeIndex kFieldChar = 3;
inline constexpr FieldTypeIndex kFieldBoolean = 4;

struct FieldValue
{
  std::uint32_t tag{ 0 };
  std::variant<std::string, std::int64_t, double, char, bool> value;

  [[nodiscard]] auto type_index() const -> FieldTypeIndex { return static_cast<FieldTypeIndex>(value.index()); }

  [[nodiscard]] auto& as_string() const { return std::get<std::string>(value); }
  [[nodiscard]] auto as_int() const { return std::get<std::int64_t>(value); }
  [[nodiscard]] auto as_float() const { return std::get<double>(value); }
  [[nodiscard]] auto as_char() const { return std::get<char>(value); }
  [[nodiscard]] auto as_bool() const { return std::get<bool>(value); }
};

struct FieldView
{
  std::uint32_t tag{ 0 };
  FieldTypeIndex type{ kFieldString };
  std::string_view string_value;
  std::int64_t int_value{ 0 };
  double float_value{ 0.0 };
  char char_value{ '\0' };
  bool bool_value{ false };

  [[nodiscard]] auto to_owned() const -> FieldValue
  {
    FieldValue v;
    v.tag = tag;
    switch (type) {
      case kFieldInt:
        v.value = int_value;
        break;
      case kFieldFloat:
        v.value = float_value;
        break;
      case kFieldChar:
        v.value = char_value;
        break;
      case kFieldBoolean:
        v.value = bool_value;
        break;
      default:
        v.value = std::string(string_value);
        break;
    }
    return v;
  }
};

inline constexpr std::uint32_t kInvalidParsedIndex = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint16_t kInvalidHashSlot = 0xFFFFU;
inline constexpr std::size_t kFieldHashTableSize = 1024U;
inline constexpr std::size_t kParsedFieldSlotInlineCapacity = 32U;
inline constexpr std::size_t kParsedEntryInlineCapacity = 4U;
inline constexpr std::size_t kParsedGroupInlineCapacity = 4U;

/// Quick cache for session-critical tags. Index by QuickCacheSlot enum.
enum class QuickCacheSlot : std::uint8_t
{
  kBeginString = 0,  // tag 8
  kBodyLength = 1,   // tag 9
  kMsgType = 2,      // tag 35
  kMsgSeqNum = 3,    // tag 34
  kSenderCompID = 4, // tag 49
  kTargetCompID = 5, // tag 56
  kSendingTime = 6,  // tag 52
  kCheckSum = 7,     // tag 10
  kCount = 8
};

inline constexpr std::size_t kQuickCacheSize = static_cast<std::size_t>(QuickCacheSlot::kCount);

/// Returns the quick-cache slot for a session tag, or nullopt if the tag
/// is not one of the 8 cached session tags.
[[nodiscard]] inline auto
QuickCacheSlotForTag(std::uint32_t tag) -> std::optional<QuickCacheSlot>
{
  switch (tag) {
    case codec::tags::kBeginString:
      return QuickCacheSlot::kBeginString;
    case codec::tags::kBodyLength:
      return QuickCacheSlot::kBodyLength;
    case codec::tags::kMsgType:
      return QuickCacheSlot::kMsgType;
    case codec::tags::kMsgSeqNum:
      return QuickCacheSlot::kMsgSeqNum;
    case codec::tags::kSenderCompID:
      return QuickCacheSlot::kSenderCompID;
    case codec::tags::kTargetCompID:
      return QuickCacheSlot::kTargetCompID;
    case codec::tags::kSendingTime:
      return QuickCacheSlot::kSendingTime;
    case codec::tags::kCheckSum:
      return QuickCacheSlot::kCheckSum;
    default:
      return std::nullopt;
  }
}

struct ParsedFieldSlot
{
  std::uint32_t tag{ 0 };
  std::uint32_t value_offset{ 0 };
  std::uint16_t value_length{ 0 };
  std::uint16_t flags{ 0 }; // lower 3 bits: FieldTypeIndex

  [[nodiscard]] auto type() const -> FieldTypeIndex { return static_cast<FieldTypeIndex>(flags & 0x07U); }
  auto set_type(FieldTypeIndex t) -> void
  {
    flags = static_cast<std::uint16_t>((flags & ~0x07U) | (static_cast<std::uint16_t>(t) & 0x07U));
  }
};

struct ParsedEntryData
{
  std::uint32_t first_field_index{ kInvalidParsedIndex };
  std::uint16_t field_count{ 0 };
  std::uint32_t first_group{ kInvalidParsedIndex };
  std::uint32_t last_group{ kInvalidParsedIndex };
  std::uint16_t group_count{ 0 };
  std::uint32_t next_entry{ kInvalidParsedIndex };
};

struct ParsedGroupFrame
{
  std::uint32_t count_tag{ 0 };
  std::uint32_t first_entry{ kInvalidParsedIndex };
  std::uint32_t last_entry{ kInvalidParsedIndex };
  std::uint16_t entry_count{ 0 };
  std::uint16_t depth{ 0 };
  std::uint32_t next_group{ kInvalidParsedIndex };
};

struct ParsedMessageData
{
  std::span<const std::byte> raw;
  std::string_view msg_type;
  ParsedEntryData root;
  base::InlineSplitVector<ParsedFieldSlot, kParsedFieldSlotInlineCapacity> field_slots;
  base::InlineSplitVector<ParsedEntryData, kParsedEntryInlineCapacity> entries;
  base::InlineSplitVector<ParsedGroupFrame, kParsedGroupInlineCapacity> groups;
  /// Direct-address lookup table for root-level fields.  For tags 0..1023
  /// the index IS the tag number.  Each entry packs a 16-bit generation
  /// counter (high half) and a 16-bit slot index (low half) into a uint32_t.
  /// A lookup is valid only when the stored generation matches
  /// `field_generation`.  Tags >= 1024 fall back to `field_hash_overflow`.
  std::array<std::uint32_t, kFieldHashTableSize> field_hash_table{};
  std::vector<std::uint16_t> field_hash_overflow;
  std::uint16_t field_generation{ 1 };
  std::array<std::uint16_t, kQuickCacheSize> quick_cache;

  ParsedMessageData()
  {
    // field_hash_table is zero-initialized; generation 0 in every entry
    // won't match field_generation == 1, so no explicit fill is needed.
    quick_cache.fill(kInvalidHashSlot);
  }

  /// Prepare for reuse without clearing the 4 KB direct-address table.
  /// Just bumps the generation counter (and handles wrap-around).
  auto ResetForNewDecode() -> void
  {
    ++field_generation;
    if (field_generation == 0U) {
      // Wrapped around to 0 which matches zero-initialized entries.
      field_hash_table.fill(0U);
      field_generation = 1U;
    }
    field_hash_overflow.clear();
    quick_cache.fill(kInvalidHashSlot);
    msg_type = {};
    root = {};
    field_slots.clear();
    entries.clear();
    groups.clear();
  }
};

struct MessageData;

struct GroupData
{
  std::uint32_t count_tag{ 0 };
  std::vector<MessageData> entries;
};

struct MessageData
{
  std::string msg_type;
  std::vector<FieldValue> fields;
  std::vector<GroupData> groups;
};

class MessageView;
class Message;
class RawGroupView;

class ParsedMessage
{
public:
  ParsedMessage() = default;

  explicit ParsedMessage(ParsedMessageData data)
    : data_(std::move(data))
  {
  }

  [[nodiscard]] auto view() const -> MessageView;
  [[nodiscard]] const ParsedMessageData& data() const { return data_; }
  auto RebindRaw(std::span<const std::byte> raw) -> void;
  [[nodiscard]] auto valid() const -> bool
  {
    return !data_.msg_type.empty() || data_.root.field_count != 0U || data_.root.group_count != 0U;
  }
  [[nodiscard]] auto mutable_data() -> ParsedMessageData& { return data_; }
  [[nodiscard]] auto ToOwned() const -> Message;

private:
  ParsedMessageData data_;
};

class GroupEntryBuilder
{
public:
  GroupEntryBuilder() = default;

  auto set_string(std::uint32_t tag, std::string_view value) -> GroupEntryBuilder&;
  auto set_int(std::uint32_t tag, std::int64_t value) -> GroupEntryBuilder&;
  auto set_char(std::uint32_t tag, char value) -> GroupEntryBuilder&;
  auto set_float(std::uint32_t tag, double value) -> GroupEntryBuilder&;
  auto set_boolean(std::uint32_t tag, bool value) -> GroupEntryBuilder&;

  auto set(std::uint32_t tag, std::string_view value) -> GroupEntryBuilder& { return set_string(tag, value); }
  auto set(std::uint32_t tag, std::int64_t value) -> GroupEntryBuilder& { return set_int(tag, value); }
  auto set(std::uint32_t tag, char value) -> GroupEntryBuilder& { return set_char(tag, value); }
  auto set(std::uint32_t tag, double value) -> GroupEntryBuilder& { return set_float(tag, value); }
  auto set(std::uint32_t tag, bool value) -> GroupEntryBuilder& { return set_boolean(tag, value); }
  auto reserve_fields(std::size_t count) -> GroupEntryBuilder&;
  auto reserve_groups(std::size_t count) -> GroupEntryBuilder&;
  auto reserve_group_entries(std::uint32_t count_tag, std::size_t count) -> GroupEntryBuilder&;
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

class Message
{
public:
  Message() = default;

  explicit Message(MessageData data)
    : data_(std::move(data))
  {
  }

  [[nodiscard]] auto view() const -> MessageView;
  [[nodiscard]] bool valid() const { return !data_.msg_type.empty() || !data_.fields.empty() || !data_.groups.empty(); }
  [[nodiscard]] const MessageData& data() const { return data_; }

private:
  MessageData data_;
};

class MessageBuilder
{
public:
  explicit MessageBuilder(std::string msg_type);

  [[nodiscard]] auto view() const -> MessageView;
  auto encode_to_buffer(const profile::NormalizedDictionaryView& dictionary,
                        const codec::EncodeOptions& options,
                        codec::EncodeBuffer* buffer) const -> base::Status;
  auto encode_to_buffer(const profile::NormalizedDictionaryView& dictionary,
                        const codec::EncodeOptions& options,
                        codec::EncodeBuffer* buffer,
                        const codec::PrecompiledTemplateTable* precompiled) const -> base::Status;
  auto encode(const profile::NormalizedDictionaryView& dictionary, const codec::EncodeOptions& options) const
    -> base::Result<std::vector<std::byte>>;

  auto set_string(std::uint32_t tag, std::string_view value) -> MessageBuilder&;
  auto set_int(std::uint32_t tag, std::int64_t value) -> MessageBuilder&;
  auto set_char(std::uint32_t tag, char value) -> MessageBuilder&;
  auto set_float(std::uint32_t tag, double value) -> MessageBuilder&;
  auto set_boolean(std::uint32_t tag, bool value) -> MessageBuilder&;

  auto set(std::uint32_t tag, std::string_view value) -> MessageBuilder& { return set_string(tag, value); }
  auto set(std::uint32_t tag, std::int64_t value) -> MessageBuilder& { return set_int(tag, value); }
  auto set(std::uint32_t tag, char value) -> MessageBuilder& { return set_char(tag, value); }
  auto set(std::uint32_t tag, double value) -> MessageBuilder& { return set_float(tag, value); }
  auto set(std::uint32_t tag, bool value) -> MessageBuilder& { return set_boolean(tag, value); }
  auto reserve_fields(std::size_t count) -> MessageBuilder&;
  auto reserve_groups(std::size_t count) -> MessageBuilder&;
  auto reserve_group_entries(std::uint32_t count_tag, std::size_t count) -> MessageBuilder&;
  auto add_group_entry(std::uint32_t count_tag) -> GroupEntryBuilder;

  auto build() && -> Message;

  /// Clear fields and groups but preserve allocated capacity for reuse.
  auto reset() -> void;

private:
  auto upsert_field(FieldValue value) -> MessageBuilder&;
  auto ensure_group(std::uint32_t count_tag) -> GroupData&;

  MessageData data_;
};

struct RawFieldView
{
  std::uint32_t tag{ 0 };
  std::string_view value;
};

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

  [[nodiscard]] auto field_count() const -> std::size_t;
  [[nodiscard]] auto field_at(std::size_t index) const -> std::optional<RawFieldView>;
  [[nodiscard]] auto field(std::uint32_t tag) const -> std::optional<std::string_view>;
  [[nodiscard]] auto group(std::uint32_t count_tag) const -> std::optional<RawGroupView>;

private:
  const ParsedMessageData* parsed_{ nullptr };
  const ParsedEntryData* parsed_entry_{ nullptr };
};

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

  [[nodiscard]] auto count_tag() const -> std::uint32_t
  {
    return parsed_group_ == nullptr ? 0U : parsed_group_->count_tag;
  }

  [[nodiscard]] auto size() const -> std::size_t { return parsed_group_ == nullptr ? 0U : parsed_group_->entry_count; }

  [[nodiscard]] auto operator[](std::size_t index) const -> RawGroupEntryView;
  [[nodiscard]] auto begin() const -> Iterator;
  [[nodiscard]] auto end() const -> Iterator;

private:
  const ParsedMessageData* parsed_{ nullptr };
  const ParsedGroupFrame* parsed_group_{ nullptr };
};

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

  [[nodiscard]] std::uint32_t count_tag() const
  {
    if (group_ != nullptr) {
      return group_->count_tag;
    }
    return parsed_group_ == nullptr ? 0U : parsed_group_->count_tag;
  }

  [[nodiscard]] std::size_t size() const
  {
    if (group_ != nullptr) {
      return group_->entries.size();
    }
    return parsed_group_ == nullptr ? 0U : parsed_group_->entry_count;
  }

  [[nodiscard]] auto operator[](std::size_t index) const -> MessageView;
  [[nodiscard]] auto begin() const -> Iterator;
  [[nodiscard]] auto end() const -> Iterator;

private:
  const GroupData* group_{ nullptr };
  const ParsedMessageData* parsed_{ nullptr };
  const ParsedGroupFrame* parsed_group_{ nullptr };
};

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

  [[nodiscard]] auto field_count() const -> std::size_t;
  [[nodiscard]] auto field_at(std::size_t index) const -> std::optional<FieldView>;
  [[nodiscard]] auto group_count() const -> std::size_t;
  [[nodiscard]] auto group_at(std::size_t index) const -> std::optional<GroupView>;
  [[nodiscard]] auto find_field_view(std::uint32_t tag) const -> std::optional<FieldView>;
  [[nodiscard]] bool has_field(std::uint32_t tag) const;
  [[nodiscard]] auto find_field(std::uint32_t tag) const -> const FieldValue*;
  [[nodiscard]] auto get_string(std::uint32_t tag) const -> std::optional<std::string_view>;
  [[nodiscard]] auto get_int(std::uint32_t tag) const -> std::optional<std::int64_t>;
  [[nodiscard]] auto get_char(std::uint32_t tag) const -> std::optional<char>;
  [[nodiscard]] auto get_float(std::uint32_t tag) const -> std::optional<double>;
  [[nodiscard]] auto get_boolean(std::uint32_t tag) const -> std::optional<bool>;
  [[nodiscard]] auto group(std::uint32_t count_tag) const -> std::optional<GroupView>;
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

auto
MaterializeMessage(MessageView view) -> Message;

class MessageRef
{
public:
  MessageRef() = default;
  explicit MessageRef(Message message);
  explicit MessageRef(MessageView view);
  ~MessageRef();
  MessageRef(const MessageRef& other);
  MessageRef(MessageRef&& other) noexcept;

  static auto Own(MessageView view) -> MessageRef;
  static auto OwnParsed(ParsedMessage parsed, std::span<const std::byte> raw) -> MessageRef;
  static auto OwnParsed(ParsedMessage parsed, std::vector<std::byte> raw) -> MessageRef;

  auto operator=(const MessageRef& other) -> MessageRef&;
  auto operator=(MessageRef&& other) noexcept -> MessageRef&;

  [[nodiscard]] bool valid() const
  {
    if (owned_ != nullptr) {
      return owned_->valid();
    }
    if (parsed_owned_ != nullptr) {
      return parsed_owned_->parsed.valid();
    }
    return view_.valid();
  }

  [[nodiscard]] bool owns_message() const { return owned_ != nullptr || parsed_owned_ != nullptr; }

  [[nodiscard]] auto view() const -> MessageView
  {
    if (owned_ != nullptr) {
      return owned_->view();
    }
    if (parsed_owned_ != nullptr) {
      return parsed_owned_->parsed.view();
    }
    return view_;
  }

  [[nodiscard]] auto ToOwned() const -> Message;

private:
  struct ParsedStorage
  {
    std::vector<std::byte> raw;
    ParsedMessage parsed;
  };

  explicit MessageRef(std::shared_ptr<const ParsedStorage> parsed_owned)
    : parsed_owned_(std::move(parsed_owned))
  {
  }

  std::shared_ptr<const Message> owned_{};
  std::shared_ptr<const ParsedStorage> parsed_owned_{};
  MessageView view_{};
};

} // namespace nimble::message