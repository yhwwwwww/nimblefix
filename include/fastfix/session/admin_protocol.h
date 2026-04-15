#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fastfix/base/result.h"
#include "fastfix/codec/compiled_decoder.h"
#include "fastfix/session/encoded_frame.h"
#include "fastfix/codec/fix_codec.h"
#include "fastfix/message/message.h"
#include "fastfix/profile/normalized_dictionary.h"
#include "fastfix/session/session_core.h"
#include "fastfix/session/transport_profile.h"
#include "fastfix/session/validation_policy.h"
#include "fastfix/store/session_store.h"

namespace fastfix::session {

inline constexpr std::size_t kProtocolEventOutboundFrameInlineCapacity = 4U;
using ProtocolFrameList = base::InlineSplitVector<EncodedFrame, kProtocolEventOutboundFrameInlineCapacity>;
inline constexpr std::size_t kReplayFrameBufferPoolSize = 4U;

class ProtocolFrameCollection {
  public:
    class iterator {
      public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = const EncodedFrame;
        using difference_type = std::ptrdiff_t;
        using pointer = const EncodedFrame*;
        using reference = const EncodedFrame&;

        iterator() = default;

        auto operator*() const -> reference {
            return owner_->operator[](index_);
        }

        auto operator->() const -> pointer {
            return &owner_->operator[](index_);
        }

        auto operator++() -> iterator& {
            ++index_;
            return *this;
        }

        auto operator++(int) -> iterator {
            auto copy = *this;
            ++(*this);
            return copy;
        }

        [[nodiscard]] auto operator==(const iterator& other) const -> bool {
            return owner_ == other.owner_ && index_ == other.index_;
        }

        [[nodiscard]] auto operator!=(const iterator& other) const -> bool {
            return !(*this == other);
        }

      private:
        friend class ProtocolFrameCollection;

        iterator(const ProtocolFrameCollection* owner, std::size_t index)
            : owner_(owner), index_(index) {
        }

        const ProtocolFrameCollection* owner_{nullptr};
        std::size_t index_{0U};
    };

    [[nodiscard]] auto empty() const -> bool {
        return ActiveFrames().empty();
    }

    [[nodiscard]] auto size() const -> std::size_t {
        return ActiveFrames().size();
    }

    auto begin() const -> iterator {
        return iterator(this, 0U);
    }

    auto end() const -> iterator {
        return iterator(this, size());
    }

    auto clear() -> void {
        borrowed_frames_.reset();
        owned_frames_.clear();
    }

    auto reserve(std::size_t count) -> void {
        EnsureOwned();
        owned_frames_.reserve(count);
    }

    auto push_back(const EncodedFrame& value) -> void {
        EnsureOwned();
        owned_frames_.push_back(value);
    }

    auto push_back(EncodedFrame&& value) -> void {
        EnsureOwned();
        owned_frames_.push_back(std::move(value));
    }

    [[nodiscard]] auto front() const -> const EncodedFrame& {
        return ActiveFrames().front();
    }

    [[nodiscard]] auto back() const -> const EncodedFrame& {
        return ActiveFrames().back();
    }

    [[nodiscard]] auto operator[](std::size_t index) const -> const EncodedFrame& {
        return ActiveFrames()[index];
    }

    auto operator=(const ProtocolFrameList& frames) -> ProtocolFrameCollection& {
        borrowed_frames_.reset();
        owned_frames_ = frames;
        return *this;
    }

    auto operator=(ProtocolFrameList&& frames) -> ProtocolFrameCollection& {
        borrowed_frames_.reset();
        owned_frames_ = std::move(frames);
        return *this;
    }

    auto borrow(std::shared_ptr<ProtocolFrameList> frames) -> void {
        owned_frames_.clear();
        borrowed_frames_ = std::move(frames);
    }

    operator const ProtocolFrameList&() const {
        return ActiveFrames();
    }

  private:
    auto EnsureOwned() -> void {
        if (!borrowed_frames_) {
            return;
        }
        owned_frames_ = *borrowed_frames_;
        borrowed_frames_.reset();
    }

    [[nodiscard]] auto ActiveFrames() const -> const ProtocolFrameList& {
        if (borrowed_frames_) {
            return *borrowed_frames_;
        }
        return owned_frames_;
    }

    ProtocolFrameList owned_frames_{};
    std::shared_ptr<ProtocolFrameList> borrowed_frames_{};
};

class ProtocolMessageList {
  public:
    class iterator {
      public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = message::MessageRef;
        using difference_type = std::ptrdiff_t;
        using pointer = message::MessageRef*;
        using reference = message::MessageRef&;

        iterator() = default;

        auto operator*() const -> reference {
            return owner_->at(index_);
        }

        auto operator->() const -> pointer {
            return &owner_->at(index_);
        }

        auto operator++() -> iterator& {
            ++index_;
            return *this;
        }

        auto operator++(int) -> iterator {
            auto copy = *this;
            ++(*this);
            return copy;
        }

        [[nodiscard]] auto operator==(const iterator& other) const -> bool {
            return owner_ == other.owner_ && index_ == other.index_;
        }

        [[nodiscard]] auto operator!=(const iterator& other) const -> bool {
            return !(*this == other);
        }

      private:
        friend class ProtocolMessageList;

        iterator(ProtocolMessageList* owner, std::size_t index)
            : owner_(owner), index_(index) {
        }

        ProtocolMessageList* owner_{nullptr};
        std::size_t index_{0};
    };

    class const_iterator {
      public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = const message::MessageRef;
        using difference_type = std::ptrdiff_t;
        using pointer = const message::MessageRef*;
        using reference = const message::MessageRef&;

        const_iterator() = default;
        const_iterator(iterator other)
            : owner_(other.owner_), index_(other.index_) {
        }

        auto operator*() const -> reference {
            return owner_->at(index_);
        }

        auto operator->() const -> pointer {
            return &owner_->at(index_);
        }

        auto operator++() -> const_iterator& {
            ++index_;
            return *this;
        }

        auto operator++(int) -> const_iterator {
            auto copy = *this;
            ++(*this);
            return copy;
        }

        [[nodiscard]] auto operator==(const const_iterator& other) const -> bool {
            return owner_ == other.owner_ && index_ == other.index_;
        }

        [[nodiscard]] auto operator!=(const const_iterator& other) const -> bool {
            return !(*this == other);
        }

      private:
        friend class ProtocolMessageList;

        const_iterator(const ProtocolMessageList* owner, std::size_t index)
            : owner_(owner), index_(index) {
        }

        const ProtocolMessageList* owner_{nullptr};
        std::size_t index_{0};
    };

    auto push_back(message::MessageRef message) -> void {
        if (!inline_message_.has_value()) {
            inline_message_ = std::move(message);
            return;
        }
        overflow_.push_back(std::move(message));
    }

    [[nodiscard]] auto empty() const -> bool {
        return !inline_message_.has_value();
    }

    [[nodiscard]] auto size() const -> std::size_t {
        return inline_message_.has_value() ? 1U + overflow_.size() : 0U;
    }

    auto clear() -> void {
        inline_message_.reset();
        overflow_.clear();
    }

    auto front() -> message::MessageRef& {
        return inline_message_.value();
    }

    auto front() const -> const message::MessageRef& {
        return inline_message_.value();
    }

    auto begin() -> iterator {
        return iterator(this, 0U);
    }

    auto end() -> iterator {
        return iterator(this, size());
    }

    auto begin() const -> const_iterator {
        return const_iterator(this, 0U);
    }

    auto end() const -> const_iterator {
        return const_iterator(this, size());
    }

    auto at(std::size_t index) -> message::MessageRef& {
        return index == 0U ? inline_message_.value() : overflow_[index - 1U];
    }

    auto at(std::size_t index) const -> const message::MessageRef& {
        return index == 0U ? inline_message_.value() : overflow_[index - 1U];
    }

  private:
    std::optional<message::MessageRef> inline_message_;
    std::vector<message::MessageRef> overflow_;
};

struct ProtocolEvent {
    struct OwnedApplicationMessage {
        std::vector<std::byte> raw;
        message::ParsedMessage parsed;
    };

    ProtocolEvent() = default;

    ProtocolEvent(const ProtocolEvent& other)
        : outbound_frames(other.outbound_frames),
          application_messages(other.application_messages),
          session_active(other.session_active),
          disconnect(other.disconnect),
          poss_resend(other.poss_resend),
          session_reject(other.session_reject),
          owned_application_message_(other.owned_application_message_) {
        RebindOwnedApplicationMessage();
    }

    ProtocolEvent(ProtocolEvent&& other) noexcept
        : outbound_frames(std::move(other.outbound_frames)),
          application_messages(std::move(other.application_messages)),
          session_active(other.session_active),
          disconnect(other.disconnect),
          poss_resend(other.poss_resend),
          session_reject(other.session_reject),
          owned_application_message_(std::move(other.owned_application_message_)) {
        RebindOwnedApplicationMessage();
    }

    auto operator=(const ProtocolEvent& other) -> ProtocolEvent& {
        if (this == &other) {
            return *this;
        }
        outbound_frames = other.outbound_frames;
        application_messages = other.application_messages;
        session_active = other.session_active;
        disconnect = other.disconnect;
        poss_resend = other.poss_resend;
        session_reject = other.session_reject;
        owned_application_message_ = other.owned_application_message_;
        RebindOwnedApplicationMessage();
        return *this;
    }

    auto operator=(ProtocolEvent&& other) noexcept -> ProtocolEvent& {
        if (this == &other) {
            return *this;
        }
        outbound_frames = std::move(other.outbound_frames);
        application_messages = std::move(other.application_messages);
        session_active = other.session_active;
        disconnect = other.disconnect;
        poss_resend = other.poss_resend;
        session_reject = other.session_reject;
        owned_application_message_ = std::move(other.owned_application_message_);
        RebindOwnedApplicationMessage();
        return *this;
    }

    ProtocolFrameCollection outbound_frames;
    ProtocolMessageList application_messages;
    bool session_active{false};
    bool disconnect{false};
    bool poss_resend{false};
    bool session_reject{false};

    auto MaterializeApplicationMessages() -> void {
        for (auto& message : application_messages) {
            if (!message.valid() || message.owns_message()) {
                continue;
            }
            message = message::MessageRef::Own(message.view());
        }
        if (!application_messages.empty() && application_messages.front().owns_message()) {
            owned_application_message_.reset();
        }
    }

    auto AdoptParsedApplicationMessage(message::ParsedMessage parsed, std::span<const std::byte> raw) -> void {
        std::vector<std::byte> owned_raw;
        owned_raw.assign(raw.begin(), raw.end());
        AdoptParsedApplicationMessage(std::move(parsed), std::move(owned_raw));
    }

    auto AdoptParsedApplicationMessage(message::ParsedMessage parsed, std::vector<std::byte> raw) -> void {
        if (application_messages.size() != 1U) {
            return;
        }
        auto& message = application_messages.front();
        if (!message.valid() || message.owns_message()) {
            return;
        }

        owned_application_message_.emplace();
        owned_application_message_->raw = std::move(raw);
        parsed.RebindRaw(std::span<const std::byte>(
            owned_application_message_->raw.data(),
            owned_application_message_->raw.size()));
        owned_application_message_->parsed = std::move(parsed);
        message = message::MessageRef(owned_application_message_->parsed.view());
    }

  private:
    auto RebindOwnedApplicationMessage() -> void {
        if (!owned_application_message_.has_value()) {
            return;
        }

        owned_application_message_->parsed.RebindRaw(std::span<const std::byte>(
            owned_application_message_->raw.data(),
            owned_application_message_->raw.size()));

        if (application_messages.size() != 1U) {
            return;
        }

        auto& message = application_messages.front();
        if (!message.valid() || message.owns_message()) {
            return;
        }
        message = message::MessageRef(owned_application_message_->parsed.view());
    }

    std::optional<OwnedApplicationMessage> owned_application_message_;
};

struct AdminProtocolConfig {
    SessionConfig session;
    TransportSessionProfile transport_profile;
    std::string begin_string{"FIX.4.4"};
    std::string sender_comp_id;
    std::string target_comp_id;
    std::string default_appl_ver_id;
    std::uint32_t heartbeat_interval_seconds{30};
    bool reset_seq_num_on_logon{false};
    bool reset_seq_num_on_logout{false};
    bool reset_seq_num_on_disconnect{false};
    bool refresh_on_logon{false};
    bool send_next_expected_msg_seq_num{false};
    ValidationPolicy validation_policy{ValidationPolicy::Strict()};
};

class AdminProtocol {
  public:
    AdminProtocol(
        AdminProtocolConfig config,
        const profile::NormalizedDictionaryView& dictionary,
        store::SessionStore* store);

    auto OnTransportConnected(std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent>;
    auto OnTransportClosed() -> base::Status;
    auto OnInbound(std::span<const std::byte> frame, std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent>;
    auto OnInbound(std::vector<std::byte>&& frame, std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent>;
    auto OnInbound(const codec::DecodedMessageView& decoded, std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent>;
    auto OnTimer(std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent>;
    [[nodiscard]] auto NextTimerDeadline(std::uint64_t timestamp_ns) const -> std::optional<std::uint64_t>;
    auto SendApplication(const message::Message& message, std::uint64_t timestamp_ns) -> base::Result<EncodedFrame>;
    auto SendApplication(message::MessageView message, std::uint64_t timestamp_ns) -> base::Result<EncodedFrame>;
    auto SendApplication(const message::MessageRef& message, std::uint64_t timestamp_ns) -> base::Result<EncodedFrame>;
    auto BeginLogout(std::string text, std::uint64_t timestamp_ns) -> base::Result<EncodedFrame>;
    auto ReserveReplayStorage(std::size_t frame_count) -> void;

    [[nodiscard]] const SessionCore& session() const {
        return session_;
    }

    [[nodiscard]] SessionCore& mutable_session() {
        return session_;
    }

  private:
    auto BuildLogonFrame(std::uint64_t timestamp_ns, bool reset_seq_num) -> base::Result<EncodedFrame>;
        auto BuildHeartbeatFrame(std::uint64_t timestamp_ns, std::string_view test_request_id) -> base::Result<EncodedFrame>;
        auto BuildTestRequestFrame(std::uint64_t timestamp_ns, std::string_view test_request_id) -> base::Result<EncodedFrame>;
    auto BuildResendRequestFrame(
        std::uint32_t begin_seq,
        std::uint32_t end_seq,
        std::uint64_t timestamp_ns) -> base::Result<EncodedFrame>;
    auto BuildGapFillFrame(
        std::uint32_t begin_seq,
        std::uint32_t new_seq_num,
        std::uint64_t timestamp_ns) -> base::Result<EncodedFrame>;
    auto BuildRejectFrame(
        std::uint32_t ref_seq_num,
        std::string_view ref_msg_type,
        std::uint32_t ref_tag_id,
        std::uint32_t reject_reason,
        std::string text,
        std::uint64_t timestamp_ns) -> base::Result<EncodedFrame>;
    auto EncodeFrame(
        const message::Message& message,
        bool admin,
        std::uint64_t timestamp_ns,
        bool persist,
        bool poss_dup,
        bool allocate_seq,
        std::uint16_t extra_record_flags,
        std::uint32_t seq_override = 0,
        std::string_view orig_sending_time = {}) -> base::Result<EncodedFrame>;
    auto EncodeFrame(
        message::MessageView message,
        bool admin,
        std::uint64_t timestamp_ns,
        bool persist,
        bool poss_dup,
        bool allocate_seq,
        std::uint16_t extra_record_flags,
        std::uint32_t seq_override = 0,
        std::string_view orig_sending_time = {}) -> base::Result<EncodedFrame>;
    auto ResolveEncodeTemplate(std::string_view msg_type) -> const codec::FrameEncodeTemplate*;
    auto ReplayOutbound(
        std::uint32_t begin_seq,
        std::uint32_t end_seq,
        std::uint64_t timestamp_ns,
        ProtocolFrameList* frames) -> base::Status;
    auto AcquireReplayFrameBuffer() -> std::shared_ptr<ProtocolFrameList>;
    auto PersistRecoveryState() -> base::Status;
    auto RefreshSessionStateFromStore() -> base::Status;
    auto ResetSessionState(
        std::uint32_t next_in_seq,
        std::uint32_t next_out_seq,
        bool reset_store) -> base::Status;
    auto ReplayCounterpartyExpectedRange(
        std::uint32_t counterparty_next_expected,
        std::uint32_t pre_logon_next_out,
        std::uint64_t timestamp_ns,
        ProtocolEvent* event) -> base::Status;
    auto RejectInbound(
        const codec::DecodedMessageView& decoded,
        std::uint32_t ref_tag_id,
        std::uint32_t reject_reason,
        std::string_view text,
        std::uint64_t timestamp_ns,
        bool disconnect) -> base::Result<ProtocolEvent>;
    auto ValidateCompIds(
        const codec::DecodedMessageView& decoded,
        std::uint32_t* ref_tag_id,
        std::uint32_t* reject_reason,
        std::string* text,
        bool* disconnect) const -> bool;
    auto ValidatePossDup(const codec::DecodedMessageView& decoded) const -> base::Status;
    auto ValidateAdministrativeMessage(
        const codec::DecodedMessageView& decoded,
        std::uint32_t* ref_tag_id,
        std::uint32_t* reject_reason,
        std::string* text,
        bool* disconnect) const -> bool;
    auto ValidateApplicationMessage(
        const codec::DecodedMessageView& decoded,
        std::uint32_t* ref_tag_id,
        std::uint32_t* reject_reason,
        std::string* text) const -> bool;
    auto EnsureInitialized() const -> base::Status;
    auto DrainDeferredGapFrames(std::uint64_t timestamp_ns, ProtocolEvent* event) -> base::Status;

    AdminProtocolConfig config_;
    const profile::NormalizedDictionaryView& dictionary_;
    store::SessionStore* store_{nullptr};
    SessionCore session_;
    std::string outstanding_test_request_id_;
    bool logout_sent_{false};
    std::uint64_t test_request_sent_ns_{0};
    std::uint64_t logout_sent_ns_{0};
    std::optional<base::Status> initialization_error_;
    codec::PrecompiledTemplateTable encode_templates_;
    codec::CompiledDecoderTable decode_table_;
    codec::DecodedMessageView inbound_decode_scratch_;
    codec::EncodeBuffer encode_buffer_;
    store::MessageRecordViewRange replay_range_buffer_;
    std::array<std::shared_ptr<ProtocolFrameList>, kReplayFrameBufferPoolSize> replay_frame_buffers_{};
    std::size_t replay_frame_buffer_cursor_{0U};
    std::vector<std::vector<std::byte>> deferred_gap_frames_;
};

}  // namespace fastfix::session