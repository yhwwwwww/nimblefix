#include "fastfix/message/message.h"

#include <charconv>

#include <cstring>

#include "fastfix/codec/fix_codec.h"
#include "fastfix/codec/fix_tags.h"

namespace fastfix::message {

namespace {

auto
FindField(std::vector<FieldValue>& fields, std::uint32_t tag) -> FieldValue*
{
  for (auto& field : fields) {
    if (field.tag == tag) {
      return &field;
    }
  }
  return nullptr;
}

auto
FindField(const std::vector<FieldValue>& fields, std::uint32_t tag) -> const FieldValue*
{
  for (const auto& field : fields) {
    if (field.tag == tag) {
      return &field;
    }
  }
  return nullptr;
}

auto
FindGroup(std::vector<GroupData>& groups, std::uint32_t count_tag) -> GroupData*
{
  for (auto& group : groups) {
    if (group.count_tag == count_tag) {
      return &group;
    }
  }
  return nullptr;
}

auto
FindGroup(const std::vector<GroupData>& groups, std::uint32_t count_tag) -> const GroupData*
{
  for (const auto& group : groups) {
    if (group.count_tag == count_tag) {
      return &group;
    }
  }
  return nullptr;
}

auto
UpsertField(std::vector<FieldValue>& fields, FieldValue value) -> void
{
  if (auto* existing = FindField(fields, value.tag); existing != nullptr) {
    *existing = std::move(value);
    return;
  }
  fields.push_back(std::move(value));
}

auto
ParsedSlotText(const ParsedMessageData& data, const ParsedFieldSlot& slot) -> std::string_view
{
  if (data.raw.empty() || slot.value_length == 0U) {
    return std::string_view{};
  }
  const auto* raw = reinterpret_cast<const char*>(data.raw.data());
  return std::string_view(raw + slot.value_offset, slot.value_length);
}

auto
ParsedFieldView(const ParsedMessageData& data, const ParsedFieldSlot& slot) -> FieldView
{
  FieldView view;
  view.tag = slot.tag;
  view.type = slot.type();
  const auto text = ParsedSlotText(data, slot);
  view.string_value = text;
  switch (slot.type()) {
    case kFieldInt: {
      std::int64_t value = 0;
      const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (ec == std::errc() && ptr == text.data() + text.size()) {
        view.int_value = value;
      }
      break;
    }
    case kFieldChar:
      if (text.size() == 1U) {
        view.char_value = text.front();
      }
      break;
    case kFieldFloat: {
      double value = 0.0;
      const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (ec == std::errc() && ptr == text.data() + text.size()) {
        view.float_value = value;
      }
      break;
    }
    case kFieldBoolean:
      view.bool_value = (text == "Y");
      break;
    default:
      break;
  }
  return view;
}

auto
FindParsedFieldInRoot(const ParsedMessageData& data, std::uint32_t tag) -> const ParsedFieldSlot*
{
  // Check quick cache first for session-critical tags.
  const auto qc_slot = QuickCacheSlotForTag(tag);
  if (qc_slot.has_value()) {
    const auto idx = data.quick_cache[static_cast<std::size_t>(*qc_slot)];
    if (idx != kInvalidHashSlot) {
      return &data.field_slots[idx];
    }
    return nullptr;
  }

  if (tag < kFieldHashTableSize) {
    const auto packed = data.field_hash_table[tag];
    const auto gen = static_cast<std::uint16_t>(packed >> 16U);
    if (gen == data.field_generation) {
      return &data.field_slots[packed & 0xFFFFU];
    }
    return nullptr;
  }
  // Overflow scan for tags >= 1024.
  for (std::size_t i = 0; i + 1U < data.field_hash_overflow.size(); i += 2U) {
    if (data.field_hash_overflow[i] == static_cast<std::uint16_t>(tag)) {
      return &data.field_slots[data.field_hash_overflow[i + 1U]];
    }
  }
  return nullptr;
}

auto
FindParsedField(const ParsedMessageData& data, const ParsedEntryData& entry, std::uint32_t tag)
  -> const ParsedFieldSlot*
{
  if (entry.first_field_index == kInvalidParsedIndex || entry.field_count == 0U) {
    return nullptr;
  }
  // For root entry, use hash table
  if (&entry == &data.root) {
    return FindParsedFieldInRoot(data, tag);
  }
  // For group entries, linear scan over contiguous range
  const auto end_index = entry.first_field_index + entry.field_count;
  for (auto i = entry.first_field_index; i < end_index; ++i) {
    if (data.field_slots[i].tag == tag) {
      return &data.field_slots[i];
    }
  }
  return nullptr;
}

auto
NthParsedField(const ParsedMessageData& data, const ParsedEntryData& entry, std::size_t index) -> const ParsedFieldSlot*
{
  if (entry.first_field_index == kInvalidParsedIndex || index >= entry.field_count) {
    return nullptr;
  }
  return &data.field_slots[entry.first_field_index + index];
}

auto
FindParsedGroup(const ParsedMessageData& data, const ParsedEntryData& entry, std::uint32_t count_tag)
  -> const ParsedGroupFrame*
{
  auto group_index = entry.first_group;
  while (group_index != kInvalidParsedIndex) {
    const auto& group = data.groups[group_index];
    if (group.count_tag == count_tag) {
      return &group;
    }
    group_index = group.next_group;
  }
  return nullptr;
}

auto
NthParsedGroup(const ParsedMessageData& data, const ParsedEntryData& entry, std::size_t index)
  -> const ParsedGroupFrame*
{
  auto group_index = entry.first_group;
  std::size_t current = 0U;
  while (group_index != kInvalidParsedIndex) {
    if (current == index) {
      return &data.groups[group_index];
    }
    group_index = data.groups[group_index].next_group;
    ++current;
  }
  return nullptr;
}

auto
NthParsedEntry(const ParsedMessageData& data, const ParsedGroupFrame& group, std::size_t index)
  -> const ParsedEntryData*
{
  auto entry_index = group.first_entry;
  std::size_t current = 0U;
  while (entry_index != kInvalidParsedIndex) {
    if (current == index) {
      return &data.entries[entry_index];
    }
    entry_index = data.entries[entry_index].next_entry;
    ++current;
  }
  return nullptr;
}

auto
CopyViewToOwned(MessageView view) -> MessageData
{
  MessageData data;
  data.msg_type = std::string(view.msg_type());
  data.fields.reserve(view.field_count());
  data.groups.reserve(view.group_count());
  for (std::size_t index = 0; index < view.field_count(); ++index) {
    const auto field = view.field_at(index);
    if (!field.has_value()) {
      continue;
    }
    data.fields.push_back(field->to_owned());
  }
  for (std::size_t index = 0; index < view.group_count(); ++index) {
    const auto group = view.group_at(index);
    if (!group.has_value()) {
      continue;
    }
    GroupData group_data;
    group_data.count_tag = group->count_tag();
    group_data.entries.reserve(group->size());
    for (const auto entry : *group) {
      group_data.entries.push_back(CopyViewToOwned(entry));
    }
    data.groups.push_back(std::move(group_data));
  }
  return data;
}

auto
RebindParsedMessageDataRaw(ParsedMessageData* data, std::span<const std::byte> raw) -> void
{
  if (data == nullptr) {
    return;
  }

  std::string_view msg_type;
  if (!data->msg_type.empty() && !data->raw.empty()) {
    const auto* old_raw = reinterpret_cast<const char*>(data->raw.data());
    const auto* old_msg_type = data->msg_type.data();
    const auto old_raw_size = static_cast<std::ptrdiff_t>(data->raw.size());
    if (old_msg_type >= old_raw &&
        old_msg_type + static_cast<std::ptrdiff_t>(data->msg_type.size()) <= old_raw + old_raw_size) {
      const auto offset = static_cast<std::size_t>(old_msg_type - old_raw);
      if (offset + data->msg_type.size() <= raw.size()) {
        const auto* new_raw = reinterpret_cast<const char*>(raw.data());
        msg_type = std::string_view(new_raw + offset, data->msg_type.size());
      }
    }
  }

  data->raw = raw;
  data->msg_type = msg_type;
}

} // namespace

auto
MessageBuilder::view() const -> MessageView
{
  return MessageView(&data_);
}

auto
MessageBuilder::encode_to_buffer(const profile::NormalizedDictionaryView& dictionary,
                                 const codec::EncodeOptions& options,
                                 codec::EncodeBuffer* buffer) const -> base::Status
{
  return codec::EncodeFixMessageToBuffer(view(), dictionary, options, buffer);
}

auto
MessageBuilder::encode_to_buffer(const profile::NormalizedDictionaryView& dictionary,
                                 const codec::EncodeOptions& options,
                                 codec::EncodeBuffer* buffer,
                                 const codec::PrecompiledTemplateTable* precompiled) const -> base::Status
{
  return codec::EncodeFixMessageToBuffer(view(), dictionary, options, buffer, precompiled);
}

auto
MessageBuilder::encode(const profile::NormalizedDictionaryView& dictionary, const codec::EncodeOptions& options) const
  -> base::Result<std::vector<std::byte>>
{
  return codec::EncodeFixMessage(view(), dictionary, options);
}

auto
GroupEntryBuilder::upsert_field(FieldValue value) -> GroupEntryBuilder&
{
  if (auto* data = resolve(); data != nullptr) {
    UpsertField(data->fields, std::move(value));
  }
  return *this;
}

auto
GroupEntryBuilder::set_string(std::uint32_t tag, std::string_view value) -> GroupEntryBuilder&
{
  return upsert_field(FieldValue{ .tag = tag, .value = std::string(value) });
}

auto
GroupEntryBuilder::set_int(std::uint32_t tag, std::int64_t value) -> GroupEntryBuilder&
{
  return upsert_field(FieldValue{ .tag = tag, .value = value });
}

auto
GroupEntryBuilder::set_char(std::uint32_t tag, char value) -> GroupEntryBuilder&
{
  return upsert_field(FieldValue{ .tag = tag, .value = value });
}

auto
GroupEntryBuilder::set_float(std::uint32_t tag, double value) -> GroupEntryBuilder&
{
  return upsert_field(FieldValue{ .tag = tag, .value = value });
}

auto
GroupEntryBuilder::set_boolean(std::uint32_t tag, bool value) -> GroupEntryBuilder&
{
  return upsert_field(FieldValue{ .tag = tag, .value = value });
}

auto
GroupEntryBuilder::reserve_fields(std::size_t count) -> GroupEntryBuilder&
{
  if (auto* data = resolve(); data != nullptr) {
    data->fields.reserve(count);
  }
  return *this;
}

auto
GroupEntryBuilder::reserve_groups(std::size_t count) -> GroupEntryBuilder&
{
  if (auto* data = resolve(); data != nullptr) {
    data->groups.reserve(count);
  }
  return *this;
}

auto
GroupEntryBuilder::reserve_group_entries(std::uint32_t count_tag, std::size_t count) -> GroupEntryBuilder&
{
  if (auto* group = ensure_group(count_tag); group != nullptr) {
    group->entries.reserve(count);
  }
  return *this;
}

auto
GroupEntryBuilder::resolve() -> MessageData*
{
  auto* data = root_;
  if (data == nullptr) {
    return nullptr;
  }

  for (const auto& segment : path_) {
    auto* group = FindGroup(data->groups, segment.count_tag);
    if (group == nullptr || segment.entry_index >= group->entries.size()) {
      return nullptr;
    }
    data = &group->entries[segment.entry_index];
  }

  return data;
}

auto
GroupEntryBuilder::ensure_group(std::uint32_t count_tag) -> GroupData*
{
  auto* data = resolve();
  if (data == nullptr) {
    return nullptr;
  }
  if (auto* group = FindGroup(data->groups, count_tag); group != nullptr) {
    return group;
  }

  data->groups.push_back(GroupData{ .count_tag = count_tag });
  return &data->groups.back();
}

auto
GroupEntryBuilder::add_group_entry(std::uint32_t count_tag) -> GroupEntryBuilder
{
  auto* group = ensure_group(count_tag);
  if (group == nullptr) {
    return {};
  }

  group->entries.push_back(MessageData{});
  auto child_path = path_;
  child_path.push_back(PathSegment{ .count_tag = count_tag, .entry_index = group->entries.size() - 1U });
  return GroupEntryBuilder(root_, std::move(child_path));
}

MessageBuilder::MessageBuilder(std::string msg_type)
{
  data_.msg_type = std::move(msg_type);
}

auto
MessageBuilder::upsert_field(FieldValue value) -> MessageBuilder&
{
  UpsertField(data_.fields, std::move(value));
  return *this;
}

auto
MessageBuilder::set_string(std::uint32_t tag, std::string_view value) -> MessageBuilder&
{
  return upsert_field(FieldValue{ .tag = tag, .value = std::string(value) });
}

auto
MessageBuilder::set_int(std::uint32_t tag, std::int64_t value) -> MessageBuilder&
{
  return upsert_field(FieldValue{ .tag = tag, .value = value });
}

auto
MessageBuilder::set_char(std::uint32_t tag, char value) -> MessageBuilder&
{
  return upsert_field(FieldValue{ .tag = tag, .value = value });
}

auto
MessageBuilder::set_float(std::uint32_t tag, double value) -> MessageBuilder&
{
  return upsert_field(FieldValue{ .tag = tag, .value = value });
}

auto
MessageBuilder::set_boolean(std::uint32_t tag, bool value) -> MessageBuilder&
{
  return upsert_field(FieldValue{ .tag = tag, .value = value });
}

auto
MessageBuilder::reserve_fields(std::size_t count) -> MessageBuilder&
{
  data_.fields.reserve(count);
  return *this;
}

auto
MessageBuilder::reserve_groups(std::size_t count) -> MessageBuilder&
{
  data_.groups.reserve(count);
  return *this;
}

auto
MessageBuilder::reserve_group_entries(std::uint32_t count_tag, std::size_t count) -> MessageBuilder&
{
  ensure_group(count_tag).entries.reserve(count);
  return *this;
}

auto
MessageBuilder::ensure_group(std::uint32_t count_tag) -> GroupData&
{
  if (auto* group = FindGroup(data_.groups, count_tag); group != nullptr) {
    return *group;
  }

  data_.groups.push_back(GroupData{ .count_tag = count_tag });
  return data_.groups.back();
}

auto
MessageBuilder::add_group_entry(std::uint32_t count_tag) -> GroupEntryBuilder
{
  auto& group = ensure_group(count_tag);
  group.entries.push_back(MessageData{});
  return GroupEntryBuilder(&data_,
                           std::vector<GroupEntryBuilder::PathSegment>{ GroupEntryBuilder::PathSegment{
                             .count_tag = count_tag, .entry_index = group.entries.size() - 1U } });
}

auto
MessageBuilder::build() && -> Message
{
  return Message(std::move(data_));
}

auto
MessageBuilder::reset() -> void
{
  data_.fields.clear();
  for (auto& group : data_.groups) {
    group.entries.clear();
  }
}

auto
RawGroupEntryView::field_count() const -> std::size_t
{
  return parsed_entry_ == nullptr ? 0U : parsed_entry_->field_count;
}

auto
RawGroupEntryView::field_at(std::size_t index) const -> std::optional<RawFieldView>
{
  if (parsed_ == nullptr || parsed_entry_ == nullptr) {
    return std::nullopt;
  }

  const auto* slot = NthParsedField(*parsed_, *parsed_entry_, index);
  if (slot == nullptr) {
    return std::nullopt;
  }

  return RawFieldView{ .tag = slot->tag, .value = ParsedSlotText(*parsed_, *slot) };
}

auto
RawGroupEntryView::field(std::uint32_t tag) const -> std::optional<std::string_view>
{
  if (parsed_ == nullptr || parsed_entry_ == nullptr) {
    return std::nullopt;
  }

  const auto* slot = FindParsedField(*parsed_, *parsed_entry_, tag);
  if (slot == nullptr) {
    return std::nullopt;
  }

  return ParsedSlotText(*parsed_, *slot);
}

auto
RawGroupEntryView::group(std::uint32_t count_tag) const -> std::optional<RawGroupView>
{
  if (parsed_ == nullptr || parsed_entry_ == nullptr) {
    return std::nullopt;
  }

  const auto* group = FindParsedGroup(*parsed_, *parsed_entry_, count_tag);
  if (group == nullptr) {
    return std::nullopt;
  }

  return RawGroupView(parsed_, group);
}

auto
RawGroupView::Iterator::operator*() const -> RawGroupEntryView
{
  if (parsed_ == nullptr || parsed_group_ == nullptr || parsed_entry_index_ == kInvalidParsedIndex) {
    return RawGroupEntryView();
  }
  return RawGroupEntryView(parsed_, &parsed_->entries[parsed_entry_index_]);
}

auto
RawGroupView::operator[](std::size_t index) const -> RawGroupEntryView
{
  if (parsed_ == nullptr || parsed_group_ == nullptr) {
    return RawGroupEntryView();
  }

  const auto* entry = NthParsedEntry(*parsed_, *parsed_group_, index);
  if (entry == nullptr) {
    return RawGroupEntryView();
  }

  return RawGroupEntryView(parsed_, entry);
}

auto
RawGroupView::begin() const -> Iterator
{
  return Iterator(parsed_, parsed_group_, parsed_group_ == nullptr ? kInvalidParsedIndex : parsed_group_->first_entry);
}

auto
RawGroupView::end() const -> Iterator
{
  return Iterator(parsed_, parsed_group_, kInvalidParsedIndex);
}

auto
Message::view() const -> MessageView
{
  return MessageView(&data_);
}

auto
GroupView::Iterator::operator*() const -> MessageView
{
  if (entries_ != nullptr) {
    return MessageView(&(*entries_)[index_]);
  }
  if (parsed_ == nullptr || parsed_entry_index_ == kInvalidParsedIndex) {
    return MessageView();
  }
  return MessageView(parsed_, std::string_view{}, &parsed_->entries[parsed_entry_index_]);
}

auto
ParsedMessage::view() const -> MessageView
{
  return MessageView(&data_, data_.msg_type, &data_.root);
}

auto
ParsedMessage::RebindRaw(std::span<const std::byte> raw) -> void
{
  RebindParsedMessageDataRaw(&data_, raw);
}

auto
ParsedMessage::ToOwned() const -> Message
{
  return Message(CopyViewToOwned(view()));
}

auto
MaterializeMessage(MessageView view) -> Message
{
  return Message(CopyViewToOwned(view));
}

MessageRef::MessageRef(Message message)
  : owned_(std::make_shared<Message>(std::move(message)))
{
}

MessageRef::MessageRef(MessageView view)
  : view_(view)
{
}

MessageRef::~MessageRef() = default;

auto
MessageRef::Own(MessageView view) -> MessageRef
{
  if (!view.valid()) {
    return MessageRef(view);
  }

  if (view.parsed_ != nullptr && view.parsed_entry_ == &view.parsed_->root) {
    ParsedMessageData copied = *view.parsed_;
    return OwnParsed(ParsedMessage(std::move(copied)), view.parsed_->raw);
  }

  return MessageRef(MaterializeMessage(view));
}

auto
MessageRef::OwnParsed(ParsedMessage parsed, std::span<const std::byte> raw) -> MessageRef
{
  std::vector<std::byte> owned_raw;
  owned_raw.assign(raw.begin(), raw.end());
  return OwnParsed(std::move(parsed), std::move(owned_raw));
}

auto
MessageRef::OwnParsed(ParsedMessage parsed, std::vector<std::byte> raw) -> MessageRef
{
  auto storage = std::make_shared<ParsedStorage>();
  storage->raw = std::move(raw);
  parsed.RebindRaw(std::span<const std::byte>(storage->raw.data(), storage->raw.size()));
  storage->parsed = std::move(parsed);
  return MessageRef(std::move(storage));
}

MessageRef::MessageRef(const MessageRef& other) = default;

MessageRef::MessageRef(MessageRef&& other) noexcept = default;

auto
MessageRef::operator=(const MessageRef& other) -> MessageRef& = default;

auto
MessageRef::operator=(MessageRef&& other) noexcept -> MessageRef& = default;

auto
MessageRef::ToOwned() const -> Message
{
  if (owned_ != nullptr) {
    return Message(owned_->data());
  }
  return MaterializeMessage(view());
}

auto
GroupView::operator[](std::size_t index) const -> MessageView
{
  if (group_ != nullptr) {
    return MessageView(&group_->entries[index]);
  }
  if (parsed_ == nullptr || parsed_group_ == nullptr) {
    return MessageView();
  }
  const auto* entry = NthParsedEntry(*parsed_, *parsed_group_, index);
  return entry == nullptr ? MessageView() : MessageView(parsed_, std::string_view{}, entry);
}

auto
GroupView::begin() const -> Iterator
{
  if (group_ != nullptr) {
    return Iterator(&group_->entries, 0U);
  }
  if (parsed_ == nullptr || parsed_group_ == nullptr || parsed_group_->entry_count == 0U) {
    return Iterator(parsed_, parsed_group_, kInvalidParsedIndex);
  }
  return Iterator(parsed_, parsed_group_, parsed_group_->first_entry);
}

auto
GroupView::end() const -> Iterator
{
  if (group_ != nullptr) {
    return Iterator(&group_->entries, size());
  }
  return Iterator(parsed_, parsed_group_, kInvalidParsedIndex);
}

auto
MessageView::field_count() const -> std::size_t
{
  if (data_ != nullptr) {
    return data_->fields.size();
  }
  return parsed_entry_ == nullptr ? 0U : parsed_entry_->field_count;
}

auto
MessageView::field_at(std::size_t index) const -> std::optional<FieldView>
{
  if (data_ != nullptr) {
    if (index >= data_->fields.size()) {
      return std::nullopt;
    }
    const auto& field = data_->fields[index];
    FieldView v;
    v.tag = field.tag;
    v.type = field.type_index();
    std::visit(
      [&](const auto& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::string>) {
          v.string_value = val;
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          v.int_value = val;
        } else if constexpr (std::is_same_v<T, double>) {
          v.float_value = val;
        } else if constexpr (std::is_same_v<T, char>) {
          v.char_value = val;
        } else if constexpr (std::is_same_v<T, bool>) {
          v.bool_value = val;
        }
      },
      field.value);
    return v;
  }
  if (parsed_ == nullptr || parsed_entry_ == nullptr) {
    return std::nullopt;
  }
  const auto* slot = NthParsedField(*parsed_, *parsed_entry_, index);
  if (slot == nullptr) {
    return std::nullopt;
  }
  return ParsedFieldView(*parsed_, *slot);
}

auto
MessageView::group_count() const -> std::size_t
{
  if (data_ != nullptr) {
    return data_->groups.size();
  }
  return parsed_entry_ == nullptr ? 0U : parsed_entry_->group_count;
}

auto
MessageView::group_at(std::size_t index) const -> std::optional<GroupView>
{
  if (data_ != nullptr) {
    if (index >= data_->groups.size()) {
      return std::nullopt;
    }
    return GroupView(&data_->groups[index]);
  }
  if (parsed_ == nullptr || parsed_entry_ == nullptr) {
    return std::nullopt;
  }
  const auto* group = NthParsedGroup(*parsed_, *parsed_entry_, index);
  if (group == nullptr) {
    return std::nullopt;
  }
  return GroupView(parsed_, group);
}

auto
MessageView::find_field_view(std::uint32_t tag) const -> std::optional<FieldView>
{
  if (data_ != nullptr) {
    const auto* field = FindField(data_->fields, tag);
    if (field != nullptr) {
      FieldView v;
      v.tag = field->tag;
      v.type = field->type_index();
      std::visit(
        [&](const auto& val) {
          using T = std::decay_t<decltype(val)>;
          if constexpr (std::is_same_v<T, std::string>) {
            v.string_value = val;
          } else if constexpr (std::is_same_v<T, std::int64_t>) {
            v.int_value = val;
          } else if constexpr (std::is_same_v<T, double>) {
            v.float_value = val;
          } else if constexpr (std::is_same_v<T, char>) {
            v.char_value = val;
          } else if constexpr (std::is_same_v<T, bool>) {
            v.bool_value = val;
          }
        },
        field->value);
      return v;
    }

    if (tag == codec::tags::kMsgType && !data_->msg_type.empty()) {
      return FieldView{ .tag = codec::tags::kMsgType, .type = kFieldString, .string_value = data_->msg_type };
    }

    return std::nullopt;
  }
  if (parsed_ == nullptr || parsed_entry_ == nullptr) {
    return std::nullopt;
  }
  const auto* slot = FindParsedField(*parsed_, *parsed_entry_, tag);
  if (slot != nullptr) {
    return ParsedFieldView(*parsed_, *slot);
  }

  if (tag == codec::tags::kMsgType && !parsed_msg_type_.empty()) {
    return FieldView{
      .tag = codec::tags::kMsgType,
      .type = kFieldString,
      .string_value = parsed_msg_type_,
    };
  }
  return std::nullopt;
}

auto
MessageView::find_field(std::uint32_t tag) const -> const FieldValue*
{
  if (data_ == nullptr) {
    return nullptr;
  }
  return FindField(data_->fields, tag);
}

auto
MessageView::has_field(std::uint32_t tag) const -> bool
{
  return find_field_view(tag).has_value();
}

auto
MessageView::get_string(std::uint32_t tag) const -> std::optional<std::string_view>
{
  const auto field = find_field_view(tag);
  if (!field.has_value() || field->type != kFieldString) {
    return std::nullopt;
  }
  return field->string_value;
}

auto
MessageView::get_int(std::uint32_t tag) const -> std::optional<std::int64_t>
{
  const auto field = find_field_view(tag);
  if (!field.has_value() || field->type != kFieldInt) {
    return std::nullopt;
  }
  return field->int_value;
}

auto
MessageView::get_char(std::uint32_t tag) const -> std::optional<char>
{
  const auto field = find_field_view(tag);
  if (!field.has_value() || field->type != kFieldChar) {
    return std::nullopt;
  }
  return field->char_value;
}

auto
MessageView::get_float(std::uint32_t tag) const -> std::optional<double>
{
  const auto field = find_field_view(tag);
  if (!field.has_value() || field->type != kFieldFloat) {
    return std::nullopt;
  }
  return field->float_value;
}

auto
MessageView::get_boolean(std::uint32_t tag) const -> std::optional<bool>
{
  const auto field = find_field_view(tag);
  if (!field.has_value() || field->type != kFieldBoolean) {
    return std::nullopt;
  }
  return field->bool_value;
}

auto
MessageView::group(std::uint32_t count_tag) const -> std::optional<GroupView>
{
  if (data_ != nullptr) {
    const auto* group = FindGroup(data_->groups, count_tag);
    if (group == nullptr) {
      return std::nullopt;
    }
    return GroupView(group);
  }

  if (parsed_ == nullptr || parsed_entry_ == nullptr) {
    return std::nullopt;
  }
  const auto* group = FindParsedGroup(*parsed_, *parsed_entry_, count_tag);
  if (group == nullptr) {
    return std::nullopt;
  }
  return GroupView(parsed_, group);
}

auto
MessageView::raw_group(std::uint32_t count_tag) const -> std::optional<RawGroupView>
{
  if (parsed_ == nullptr || parsed_entry_ == nullptr) {
    return std::nullopt;
  }

  const auto* group = FindParsedGroup(*parsed_, *parsed_entry_, count_tag);
  if (group == nullptr) {
    return std::nullopt;
  }

  return RawGroupView(parsed_, group);
}

} // namespace fastfix::message