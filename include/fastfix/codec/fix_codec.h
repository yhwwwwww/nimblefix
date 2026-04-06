#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fastfix/base/result.h"
#include "fastfix/message/message.h"
#include "fastfix/profile/normalized_dictionary.h"

namespace fastfix::codec {

inline constexpr char kFixSoh = '\x01';
inline constexpr std::size_t kUtcTimestampLength = 21U;

enum class ValidationIssueKind : std::uint32_t {
    kNone = 0,
    kUnknownField,
    kFieldNotAllowed,
    kDuplicateField,
    kFieldOutOfOrder,
    kIncorrectNumInGroupCount,
};

struct ValidationIssue {
    ValidationIssueKind kind{ValidationIssueKind::kNone};
    std::uint32_t tag{0};
    std::string text;

    [[nodiscard]] bool present() const {
        return kind != ValidationIssueKind::kNone;
    }
};

struct SessionHeader {
    std::string begin_string;
    std::string msg_type;
    std::string sender_comp_id;
    std::string target_comp_id;
    std::string default_appl_ver_id;
    std::string sending_time;
    std::string orig_sending_time;
    std::uint32_t body_length{0};
    std::uint32_t msg_seq_num{0};
    std::uint32_t checksum{0};
    bool poss_dup{false};
};

struct SessionHeaderView {
    std::string_view begin_string;
    std::string_view msg_type;
    std::string_view sender_comp_id;
    std::string_view target_comp_id;
    std::string_view default_appl_ver_id;
    std::string_view sending_time;
    std::string_view orig_sending_time;
    std::uint32_t body_length{0};
    std::uint32_t msg_seq_num{0};
    std::uint32_t checksum{0};
    bool poss_dup{false};

    [[nodiscard]] auto ToOwned() const -> SessionHeader {
        SessionHeader header;
        header.begin_string = std::string(begin_string);
        header.msg_type = std::string(msg_type);
        header.sender_comp_id = std::string(sender_comp_id);
        header.target_comp_id = std::string(target_comp_id);
        header.default_appl_ver_id = std::string(default_appl_ver_id);
        header.sending_time = std::string(sending_time);
        header.orig_sending_time = std::string(orig_sending_time);
        header.body_length = body_length;
        header.msg_seq_num = msg_seq_num;
        header.checksum = checksum;
        header.poss_dup = poss_dup;
        return header;
    }
};

struct EncodeOptions {
    std::string begin_string{"FIX.4.4"};
    std::string sender_comp_id;
    std::string target_comp_id;
    std::string default_appl_ver_id;
    std::string_view sending_time;
    std::string_view orig_sending_time;
    std::uint32_t msg_seq_num{0};
    bool poss_dup{false};
    char delimiter{kFixSoh};
};

struct UtcTimestampBuffer {
    std::array<char, kUtcTimestampLength> storage{};

    [[nodiscard]] auto view() const -> std::string_view {
        return std::string_view(storage.data(), storage.size());
    }
};

struct EncodeTemplateConfig {
    std::string begin_string{"FIX.4.4"};
    std::string sender_comp_id;
    std::string target_comp_id;
    std::string default_appl_ver_id;
    char delimiter{kFixSoh};
};

struct EncodeBuffer {
    std::string storage;

    auto clear() -> void {
        storage.clear();
    }

    auto reserve(std::size_t capacity) -> void {
        storage.reserve(capacity);
    }

    [[nodiscard]] auto empty() const -> bool {
        return storage.empty();
    }

    [[nodiscard]] auto size() const -> std::size_t {
        return storage.size();
    }

    [[nodiscard]] auto text() const -> std::string_view {
        return storage;
    }

    [[nodiscard]] auto bytes() const -> std::span<const std::byte> {
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(storage.data()), storage.size());
    }
};

struct DecodedMessage {
    message::Message message;
    SessionHeader header;
    std::vector<std::byte> raw;
    ValidationIssue validation_issue;
};

struct DecodedMessageView {
    message::ParsedMessage message;
    SessionHeaderView header;
    std::span<const std::byte> raw;
    ValidationIssue validation_issue;

    auto ToOwned() && -> DecodedMessage {
        DecodedMessage decoded;
        decoded.message = message.ToOwned();
        decoded.header = header.ToOwned();
        decoded.raw = std::vector<std::byte>(raw.begin(), raw.end());
        decoded.validation_issue = std::move(validation_issue);
        return decoded;
    }
};

class FrameEncodeTemplate {
  public:
    FrameEncodeTemplate() = default;

    [[nodiscard]] auto valid() const -> bool {
        return state_ != nullptr;
    }

    [[nodiscard]] auto msg_type() const -> std::string_view;

    auto EncodeToBuffer(
        const message::Message& message,
        const EncodeOptions& options,
        EncodeBuffer* buffer) const -> base::Status;
    auto EncodeToBuffer(
        message::MessageView message,
        const EncodeOptions& options,
        EncodeBuffer* buffer) const -> base::Status;

    auto Encode(
        const message::Message& message,
        const EncodeOptions& options) const -> base::Result<std::vector<std::byte>>;
    auto Encode(
        message::MessageView message,
        const EncodeOptions& options) const -> base::Result<std::vector<std::byte>>;

  private:
    struct State;

    explicit FrameEncodeTemplate(std::shared_ptr<const State> state)
        : state_(std::move(state)) {
    }

    std::shared_ptr<const State> state_;

    friend auto CompileFrameEncodeTemplate(
        const profile::NormalizedDictionaryView& dictionary,
        std::string_view msg_type,
        const EncodeTemplateConfig& config) -> base::Result<FrameEncodeTemplate>;

    friend class PrecompiledTemplateTable;
};

class PrecompiledTemplateTable {
  public:
    PrecompiledTemplateTable() = default;

    static auto Build(
        const profile::NormalizedDictionaryView& dictionary,
        const EncodeTemplateConfig& config) -> base::Result<PrecompiledTemplateTable>;

    [[nodiscard]] auto find(std::string_view msg_type) const -> const FrameEncodeTemplate*;

    [[nodiscard]] bool empty() const { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }

  private:
    struct Entry {
        std::string msg_type;
        FrameEncodeTemplate tmpl;
    };
    std::vector<Entry> entries_;
    mutable std::string_view last_hit_msg_type_{};
    mutable const FrameEncodeTemplate* last_hit_tmpl_{nullptr};
};

auto CurrentUtcTimestamp(UtcTimestampBuffer* buffer) -> std::string_view;

auto CurrentUtcTimestamp() -> std::string;

auto CompileFrameEncodeTemplate(
    const profile::NormalizedDictionaryView& dictionary,
    std::string_view msg_type,
    const EncodeTemplateConfig& config) -> base::Result<FrameEncodeTemplate>;

auto EncodeFixMessageToBuffer(
    const message::Message& message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options,
    EncodeBuffer* buffer) -> base::Status;

auto EncodeFixMessageToBuffer(
    message::MessageView message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options,
    EncodeBuffer* buffer) -> base::Status;

auto EncodeFixMessageToBuffer(
    const message::Message& message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options,
    EncodeBuffer* buffer,
    const PrecompiledTemplateTable* precompiled) -> base::Status;

auto EncodeFixMessageToBuffer(
    message::MessageView message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options,
    EncodeBuffer* buffer,
    const PrecompiledTemplateTable* precompiled) -> base::Status;

auto EncodeFixMessage(
    const message::Message& message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options) -> base::Result<std::vector<std::byte>>;

auto EncodeFixMessage(
    message::MessageView message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options) -> base::Result<std::vector<std::byte>>;

auto DecodeFixMessage(
    std::span<const std::byte> bytes,
    const profile::NormalizedDictionaryView& dictionary,
    char delimiter = kFixSoh) -> base::Result<DecodedMessage>;

auto DecodeFixMessageView(
    std::span<const std::byte> bytes,
    const profile::NormalizedDictionaryView& dictionary,
    char delimiter = kFixSoh) -> base::Result<DecodedMessageView>;

auto PeekSessionHeader(
    std::span<const std::byte> bytes,
    char delimiter = kFixSoh) -> base::Result<SessionHeader>;

auto PeekSessionHeaderView(
    std::span<const std::byte> bytes,
    char delimiter = kFixSoh) -> base::Result<SessionHeaderView>;

}  // namespace fastfix::codec