#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
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

/// Advanced dynamic writer for schema-agnostic tools and tests.
///
/// Normal application code should prefer generated profile builders and
/// `runtime::Session<Profile>::send<Msg>()`. This writer intentionally stays
/// under `advanced/` because callers must manage tags, field types, ordering,
/// and repeating-group structure themselves.
class GroupEntryDataWriter
{
public:
  GroupEntryDataWriter() = default;

  auto set_string(std::uint32_t tag, std::string_view value) -> GroupEntryDataWriter&;
  auto set_int(std::uint32_t tag, std::int64_t value) -> GroupEntryDataWriter&;
  auto set_char(std::uint32_t tag, char value) -> GroupEntryDataWriter&;
  auto set_float(std::uint32_t tag, double value) -> GroupEntryDataWriter&;
  auto set_boolean(std::uint32_t tag, bool value) -> GroupEntryDataWriter&;

  auto set(std::uint32_t tag, std::string_view value) -> GroupEntryDataWriter& { return set_string(tag, value); }
  auto set(std::uint32_t tag, std::int64_t value) -> GroupEntryDataWriter& { return set_int(tag, value); }
  auto set(std::uint32_t tag, char value) -> GroupEntryDataWriter& { return set_char(tag, value); }
  auto set(std::uint32_t tag, double value) -> GroupEntryDataWriter& { return set_float(tag, value); }
  auto set(std::uint32_t tag, bool value) -> GroupEntryDataWriter& { return set_boolean(tag, value); }

  auto reserve_fields(std::size_t count) -> GroupEntryDataWriter&;
  auto reserve_groups(std::size_t count) -> GroupEntryDataWriter&;
  auto reserve_group_entries(std::uint32_t count_tag, std::size_t count) -> GroupEntryDataWriter&;
  auto add_group_entry(std::uint32_t count_tag) -> GroupEntryDataWriter;

private:
  friend class MessageDataWriter;

  struct PathSegment
  {
    std::uint32_t count_tag{ 0 };
    std::size_t entry_index{ 0U };
  };

  explicit GroupEntryDataWriter(MessageData* root)
    : root_(root)
  {
  }

  GroupEntryDataWriter(MessageData* root, std::vector<PathSegment> path)
    : root_(root)
    , path_(std::move(path))
  {
  }

  auto resolve() -> MessageData*;
  auto upsert_field(FieldValue value) -> GroupEntryDataWriter&;
  auto ensure_group(std::uint32_t count_tag) -> GroupData*;

  MessageData* root_{ nullptr };
  std::vector<PathSegment> path_;
};

class MessageDataWriter
{
public:
  explicit MessageDataWriter(std::string msg_type);

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

  auto set_string(std::uint32_t tag, std::string_view value) -> MessageDataWriter&;
  auto set_int(std::uint32_t tag, std::int64_t value) -> MessageDataWriter&;
  auto set_char(std::uint32_t tag, char value) -> MessageDataWriter&;
  auto set_float(std::uint32_t tag, double value) -> MessageDataWriter&;
  auto set_boolean(std::uint32_t tag, bool value) -> MessageDataWriter&;

  auto set(std::uint32_t tag, std::string_view value) -> MessageDataWriter& { return set_string(tag, value); }
  auto set(std::uint32_t tag, std::int64_t value) -> MessageDataWriter& { return set_int(tag, value); }
  auto set(std::uint32_t tag, char value) -> MessageDataWriter& { return set_char(tag, value); }
  auto set(std::uint32_t tag, double value) -> MessageDataWriter& { return set_float(tag, value); }
  auto set(std::uint32_t tag, bool value) -> MessageDataWriter& { return set_boolean(tag, value); }

  auto add_string(std::uint32_t tag, std::string_view value) -> MessageDataWriter&;

  auto reserve_fields(std::size_t count) -> MessageDataWriter&;
  auto reserve_groups(std::size_t count) -> MessageDataWriter&;
  auto reserve_group_entries(std::uint32_t count_tag, std::size_t count) -> MessageDataWriter&;
  auto add_group_entry(std::uint32_t count_tag) -> GroupEntryDataWriter;

  auto build() && -> Message;
  auto reset() -> void;

private:
  auto upsert_field(FieldValue value) -> MessageDataWriter&;
  auto ensure_group(std::uint32_t count_tag) -> GroupData&;

  MessageData data_;
};

} // namespace nimble::message
