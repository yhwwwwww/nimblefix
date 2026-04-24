#pragma once

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

#include "nimblefix/base/inline_split_vector.h"
#include "nimblefix/base/result.h"
#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/message/message_ref.h"
#include "nimblefix/session/encoded_frame.h"
#include "nimblefix/session/session_core.h"
#include "nimblefix/session/session_send_envelope.h"
#include "nimblefix/session/transport_profile.h"
#include "nimblefix/session/validation_policy.h"

namespace nimble::profile {

class NormalizedDictionaryView;

} // namespace nimble::profile

namespace nimble::store {

class SessionStore;

} // namespace nimble::store

namespace nimble::session {

inline constexpr std::size_t kProtocolEventOutboundFrameInlineCapacity = 4U;
using ProtocolFrameList = base::InlineSplitVector<EncodedFrame, kProtocolEventOutboundFrameInlineCapacity>;
inline constexpr std::size_t kReplayFrameBufferPoolSize = 4U;

/// Copy-on-write collection of outbound protocol frames.
///
/// Design intent: let replay paths hand a shared frame buffer into the runtime
/// without copying, while still giving callers a mutable vector-like API when
/// they need to append or rewrite frames.
///
/// Lifetime: `borrow()` pins a shared `ProtocolFrameList`. The first mutating
/// operation materializes an owned copy and invalidates prior iterators.
class ProtocolFrameCollection
{
public:
  class iterator
  {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = const EncodedFrame;
    using difference_type = std::ptrdiff_t;
    using pointer = const EncodedFrame*;
    using reference = const EncodedFrame&;

    iterator() = default;
    auto operator*() const -> reference { return owner_->operator[](index_); }
    auto operator->() const -> pointer { return &owner_->operator[](index_); }

    auto operator++() -> iterator&
    {
      ++index_;
      return *this;
    }

    auto operator++(int) -> iterator
    {
      auto copy = *this;
      ++(*this);
      return copy;
    }

    [[nodiscard]] auto operator==(const iterator& other) const -> bool
    {
      return owner_ == other.owner_ && index_ == other.index_;
    }

    [[nodiscard]] auto operator!=(const iterator& other) const -> bool { return !(*this == other); }

  private:
    friend class ProtocolFrameCollection;

    iterator(const ProtocolFrameCollection* owner, std::size_t index)
      : owner_(owner)
      , index_(index)
    {
    }

    const ProtocolFrameCollection* owner_{ nullptr };
    std::size_t index_{ 0U };
  };

  [[nodiscard]] auto empty() const -> bool { return ActiveFrames().empty(); }
  [[nodiscard]] auto size() const -> std::size_t { return ActiveFrames().size(); }
  auto begin() const -> iterator { return iterator(this, 0U); }
  auto end() const -> iterator { return iterator(this, size()); }

  /// Drop both borrowed and owned frame storage.
  auto clear() -> void
  {
    borrowed_frames_.reset();
    owned_frames_.clear();
  }

  /// Reserve owned frame capacity, materializing if currently borrowed.
  ///
  /// \param count Desired frame capacity.
  auto reserve(std::size_t count) -> void
  {
    EnsureOwned();
    owned_frames_.reserve(count);
  }

  /// Append one frame, materializing if currently borrowed.
  ///
  /// \param value Frame to append.
  auto push_back(const EncodedFrame& value) -> void
  {
    EnsureOwned();
    owned_frames_.push_back(value);
  }

  /// Append one moved frame, materializing if currently borrowed.
  ///
  /// \param value Frame to append.
  auto push_back(EncodedFrame&& value) -> void
  {
    EnsureOwned();
    owned_frames_.push_back(std::move(value));
  }

  [[nodiscard]] auto front() const -> const EncodedFrame& { return ActiveFrames().front(); }
  [[nodiscard]] auto back() const -> const EncodedFrame& { return ActiveFrames().back(); }
  [[nodiscard]] auto operator[](std::size_t index) const -> const EncodedFrame& { return ActiveFrames()[index]; }

  auto operator=(const ProtocolFrameList& frames) -> ProtocolFrameCollection&
  {
    borrowed_frames_.reset();
    owned_frames_ = frames;
    return *this;
  }

  auto operator=(ProtocolFrameList&& frames) -> ProtocolFrameCollection&
  {
    borrowed_frames_.reset();
    owned_frames_ = std::move(frames);
    return *this;
  }

  /// Borrow a shared replay/output buffer without copying.
  ///
  /// \param frames Shared frame list whose lifetime is pinned until the next mutation or clear.
  auto borrow(std::shared_ptr<ProtocolFrameList> frames) -> void
  {
    owned_frames_.clear();
    borrowed_frames_ = std::move(frames);
  }

  operator const ProtocolFrameList&() const { return ActiveFrames(); }

private:
  auto EnsureOwned() -> void
  {
    if (!borrowed_frames_) {
      return;
    }
    owned_frames_ = *borrowed_frames_;
    borrowed_frames_.reset();
  }

  [[nodiscard]] auto ActiveFrames() const -> const ProtocolFrameList&
  {
    if (borrowed_frames_) {
      return *borrowed_frames_;
    }
    return owned_frames_;
  }

  ProtocolFrameList owned_frames_{};
  std::shared_ptr<ProtocolFrameList> borrowed_frames_{};
};

/// Small-message optimized list of application messages.
///
/// Design intent: the common protocol event carries zero or one application
/// message, so the first message stays inline and the overflow vector is only
/// paid when needed.
class ProtocolMessageList
{
public:
  class iterator
  {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = message::MessageRef;
    using difference_type = std::ptrdiff_t;
    using pointer = message::MessageRef*;
    using reference = message::MessageRef&;

    iterator() = default;
    auto operator*() const -> reference { return owner_->at(index_); }
    auto operator->() const -> pointer { return &owner_->at(index_); }

    auto operator++() -> iterator&
    {
      ++index_;
      return *this;
    }

    auto operator++(int) -> iterator
    {
      auto copy = *this;
      ++(*this);
      return copy;
    }

    [[nodiscard]] auto operator==(const iterator& other) const -> bool
    {
      return owner_ == other.owner_ && index_ == other.index_;
    }

    [[nodiscard]] auto operator!=(const iterator& other) const -> bool { return !(*this == other); }

  private:
    friend class ProtocolMessageList;

    iterator(ProtocolMessageList* owner, std::size_t index)
      : owner_(owner)
      , index_(index)
    {
    }

    ProtocolMessageList* owner_{ nullptr };
    std::size_t index_{ 0 };
  };

  class const_iterator
  {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = const message::MessageRef;
    using difference_type = std::ptrdiff_t;
    using pointer = const message::MessageRef*;
    using reference = const message::MessageRef&;

    const_iterator() = default;
    const_iterator(iterator other)
      : owner_(other.owner_)
      , index_(other.index_)
    {
    }

    auto operator*() const -> reference { return owner_->at(index_); }
    auto operator->() const -> pointer { return &owner_->at(index_); }

    auto operator++() -> const_iterator&
    {
      ++index_;
      return *this;
    }

    auto operator++(int) -> const_iterator
    {
      auto copy = *this;
      ++(*this);
      return copy;
    }

    [[nodiscard]] auto operator==(const const_iterator& other) const -> bool
    {
      return owner_ == other.owner_ && index_ == other.index_;
    }

    [[nodiscard]] auto operator!=(const const_iterator& other) const -> bool { return !(*this == other); }

  private:
    friend class ProtocolMessageList;

    const_iterator(const ProtocolMessageList* owner, std::size_t index)
      : owner_(owner)
      , index_(index)
    {
    }

    const ProtocolMessageList* owner_{ nullptr };
    std::size_t index_{ 0 };
  };

  /// Append one application message.
  ///
  /// The first message stays in `inline_message_`; later messages spill into
  /// `overflow_`.
  ///
  /// \param message Message reference to append.
  auto push_back(message::MessageRef message) -> void
  {
    if (!inline_message_.has_value()) {
      inline_message_ = std::move(message);
      return;
    }
    overflow_.push_back(std::move(message));
  }

  [[nodiscard]] auto empty() const -> bool { return !inline_message_.has_value(); }
  [[nodiscard]] auto size() const -> std::size_t { return inline_message_.has_value() ? 1U + overflow_.size() : 0U; }

  auto clear() -> void
  {
    inline_message_.reset();
    overflow_.clear();
  }

  auto front() -> message::MessageRef& { return inline_message_.value(); }
  auto front() const -> const message::MessageRef& { return inline_message_.value(); }
  auto begin() -> iterator { return iterator(this, 0U); }
  auto end() -> iterator { return iterator(this, size()); }
  auto begin() const -> const_iterator { return const_iterator(this, 0U); }
  auto end() const -> const_iterator { return const_iterator(this, size()); }

  auto at(std::size_t index) -> message::MessageRef&
  {
    return index == 0U ? inline_message_.value() : overflow_[index - 1U];
  }

  auto at(std::size_t index) const -> const message::MessageRef&
  {
    return index == 0U ? inline_message_.value() : overflow_[index - 1U];
  }

private:
  std::optional<message::MessageRef> inline_message_;
  std::vector<message::MessageRef> overflow_;
};

/// Aggregate result from one `AdminProtocol` step.
///
/// A protocol event may contain outbound frames to send, decoded application
/// messages to deliver, and session-state flags such as `session_active` or
/// `disconnect`. The struct is self-contained: when it needs to keep a parsed
/// application message alive beyond the caller's input frame lifetime, it can
/// adopt or materialize owned storage.
struct ProtocolEvent
{
  struct OwnedApplicationMessage
  {
    std::vector<std::byte> raw;
    message::ParsedMessage parsed;
  };

  ProtocolEvent() = default;

  ProtocolEvent(const ProtocolEvent& other)
    : outbound_frames(other.outbound_frames)
    , application_messages(other.application_messages)
    , session_active(other.session_active)
    , disconnect(other.disconnect)
    , poss_resend(other.poss_resend)
    , session_reject(other.session_reject)
    , owned_application_message_(other.owned_application_message_)
  {
    RebindOwnedApplicationMessage();
  }

  ProtocolEvent(ProtocolEvent&& other) noexcept
    : outbound_frames(std::move(other.outbound_frames))
    , application_messages(std::move(other.application_messages))
    , session_active(other.session_active)
    , disconnect(other.disconnect)
    , poss_resend(other.poss_resend)
    , session_reject(other.session_reject)
    , owned_application_message_(std::move(other.owned_application_message_))
  {
    RebindOwnedApplicationMessage();
  }

  auto operator=(const ProtocolEvent& other) -> ProtocolEvent&
  {
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

  auto operator=(ProtocolEvent&& other) noexcept -> ProtocolEvent&
  {
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
  bool session_active{ false };
  bool disconnect{ false };
  bool poss_resend{ false };
  bool session_reject{ false };

  /// Deep-copy any borrowed application messages into owned storage.
  auto MaterializeApplicationMessages() -> void
  {
    for (auto& message : application_messages) {
      if (!message.valid() || message.owns_storage()) {
        continue;
      }
      message = message::MessageRef::Copy(message.view());
    }
    if (!application_messages.empty() && application_messages.front().owns_storage()) {
      owned_application_message_.reset();
    }
  }

  /// Adopt a parsed application message by copying the supplied raw frame.
  ///
  /// \param parsed Parsed application message.
  /// \param raw Borrowed raw frame bytes to copy and pin.
  auto AdoptParsedApplicationMessage(message::ParsedMessage parsed, std::span<const std::byte> raw) -> void
  {
    std::vector<std::byte> owned_raw;
    owned_raw.assign(raw.begin(), raw.end());
    AdoptParsedApplicationMessage(std::move(parsed), std::move(owned_raw));
  }

  /// Adopt a parsed application message together with owned raw bytes.
  ///
  /// \param parsed Parsed application message.
  /// \param raw Owned raw frame bytes that back parser string views.
  auto AdoptParsedApplicationMessage(message::ParsedMessage parsed, std::vector<std::byte> raw) -> void
  {
    if (application_messages.size() != 1U) {
      return;
    }
    auto& message = application_messages.front();
    if (!message.valid() || message.owns_storage()) {
      return;
    }

    owned_application_message_.emplace();
    owned_application_message_->raw = std::move(raw);
    parsed.RebindRaw(
      std::span<const std::byte>(owned_application_message_->raw.data(), owned_application_message_->raw.size()));
    owned_application_message_->parsed = std::move(parsed);
    message = message::MessageRef::Borrow(owned_application_message_->parsed.view());
  }

private:
  auto RebindOwnedApplicationMessage() -> void
  {
    if (!owned_application_message_.has_value()) {
      return;
    }

    owned_application_message_->parsed.RebindRaw(
      std::span<const std::byte>(owned_application_message_->raw.data(), owned_application_message_->raw.size()));

    if (application_messages.size() != 1U) {
      return;
    }

    auto& message = application_messages.front();
    if (!message.valid() || message.owns_storage()) {
      return;
    }
    message = message::MessageRef::Borrow(owned_application_message_->parsed.view());
  }

  std::optional<OwnedApplicationMessage> owned_application_message_;
};

/// Static configuration for one session admin state machine.
///
/// Design intent: package the transport profile, validation policy, and reset
/// knobs that affect logon/logout/replay semantics for a single runtime
/// session.
struct AdminProtocolConfig
{
  SessionConfig session;
  TransportSessionProfile transport_profile;
  std::string begin_string{ "FIX.4.4" };
  std::string sender_comp_id;
  std::string target_comp_id;
  std::string default_appl_ver_id;
  std::uint32_t heartbeat_interval_seconds{ 30 };
  bool reset_seq_num_on_logon{ false };
  bool reset_seq_num_on_logout{ false };
  bool reset_seq_num_on_disconnect{ false };
  bool refresh_on_logon{ false };
  bool send_next_expected_msg_seq_num{ false };
  ValidationPolicy validation_policy{ ValidationPolicy::Strict() };
};

/// Single-session FIX admin state machine and frame finalizer.
///
/// Design intent: own the session-level protocol contract above raw transport:
/// logon/logout, heartbeats, sequence management, resend/gap-fill, validation,
/// and final wire framing for application messages.
///
/// Performance/lifecycle contract:
/// - this type is single-threaded; drive it from one worker in timestamp order
/// - borrowed inputs (`std::span`, `MessageView`, `EncodedApplicationMessageView`)
///   only need to outlive the call
/// - returned `ProtocolEvent`/`EncodedFrame` own or pin any data that must live
///   past the call boundary
/// - `SendEncodedApplication()` accepts only the pre-encoded application body;
///   session-managed FIX header/trailer fields are still finalized here
class AdminProtocol
{
public:
  /// \param config Session admin protocol configuration.
  /// \param dictionary Dictionary used for decode/encode validation and ordering.
  /// \param store Persistent store used for replay and recovery.
  AdminProtocol(AdminProtocolConfig config,
                const profile::NormalizedDictionaryView& dictionary,
                store::SessionStore* store);
  ~AdminProtocol();

  AdminProtocol(const AdminProtocol&) = delete;
  auto operator=(const AdminProtocol&) -> AdminProtocol& = delete;
  AdminProtocol(AdminProtocol&& other) noexcept;
  auto operator=(AdminProtocol&& other) noexcept -> AdminProtocol&;

  /// Notify the state machine that the transport connected.
  ///
  /// \param timestamp_ns Wall-clock timestamp in nanoseconds.
  /// \return Protocol event containing any immediate outbound Logon/heartbeat work.
  auto OnTransportConnected(std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent>;

  /// Notify the state machine that the transport closed.
  auto OnTransportClosed() -> base::Status;

  /// Decode and process one inbound raw frame.
  ///
  /// \param frame Borrowed inbound FIX frame bytes.
  /// \param timestamp_ns Wall-clock timestamp in nanoseconds.
  /// \return Protocol event describing outbound work and application delivery.
  auto OnInbound(std::span<const std::byte> frame, std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent>;

  /// Process one inbound raw frame with ownership transfer.
  ///
  /// This path can avoid an extra copy when the returned protocol event needs to
  /// keep parser-backed application payloads alive.
  ///
  /// \param frame Owned inbound FIX frame bytes.
  /// \param timestamp_ns Wall-clock timestamp in nanoseconds.
  /// \return Protocol event describing outbound work and application delivery.
  auto OnInbound(std::vector<std::byte>&& frame, std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent>;

  /// Process one inbound message that was already decoded elsewhere.
  ///
  /// \param decoded Borrowed decoded message view.
  /// \param timestamp_ns Wall-clock timestamp in nanoseconds.
  /// \return Protocol event describing outbound work and application delivery.
  auto OnInbound(const codec::DecodedMessageView& decoded, std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent>;

  /// Drive timer-based protocol work such as heartbeats and test requests.
  ///
  /// \param timestamp_ns Wall-clock timestamp in nanoseconds.
  /// \return Protocol event containing any outbound timer-driven frames.
  auto OnTimer(std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent>;

  /// Query the next timer deadline in wall-clock nanoseconds.
  ///
  /// \param timestamp_ns Current wall-clock timestamp in nanoseconds.
  /// \return Absolute deadline when timer work should run next, or `std::nullopt` when idle.
  [[nodiscard]] auto NextTimerDeadline(std::uint64_t timestamp_ns) const -> std::optional<std::uint64_t>;

  /// Encode and finalize one decoded application message.
  ///
  /// \param message Owned decoded message.
  /// \param timestamp_ns Wall-clock timestamp in nanoseconds.
  /// \param envelope Optional `50/57` header overrides.
  /// \return Finalized outbound frame on success, otherwise an error status.
  auto SendApplication(const message::Message& message,
                       std::uint64_t timestamp_ns,
                       SessionSendEnvelopeView envelope = {}) -> base::Result<EncodedFrame>;

  /// Encode and finalize one borrowed decoded application message.
  ///
  /// \param message Borrowed decoded message view.
  /// \param timestamp_ns Wall-clock timestamp in nanoseconds.
  /// \param envelope Optional `50/57` header overrides.
  /// \return Finalized outbound frame on success, otherwise an error status.
  auto SendApplication(message::MessageView message, std::uint64_t timestamp_ns, SessionSendEnvelopeView envelope = {})
    -> base::Result<EncodedFrame>;

  /// Encode and finalize one owned-or-borrowed decoded application message.
  ///
  /// \param message Message reference.
  /// \param timestamp_ns Wall-clock timestamp in nanoseconds.
  /// \param envelope Optional `50/57` header overrides.
  /// \return Finalized outbound frame on success, otherwise an error status.
  auto SendApplication(const message::MessageRef& message,
                       std::uint64_t timestamp_ns,
                       SessionSendEnvelopeView envelope = {}) -> base::Result<EncodedFrame>;

  /// Finalize one owned pre-encoded application body into a full FIX frame.
  ///
  /// \param message Owned pre-encoded application body.
  /// \param timestamp_ns Wall-clock timestamp in nanoseconds.
  /// \param envelope Optional `50/57` header overrides.
  /// \return Finalized outbound frame on success, otherwise an error status.
  auto SendEncodedApplication(const EncodedApplicationMessage& message,
                              std::uint64_t timestamp_ns,
                              SessionSendEnvelopeView envelope = {}) -> base::Result<EncodedFrame>;

  /// Finalize one borrowed pre-encoded application body into a full FIX frame.
  ///
  /// \param message Borrowed pre-encoded application body.
  /// \param timestamp_ns Wall-clock timestamp in nanoseconds.
  /// \param envelope Optional `50/57` header overrides.
  /// \return Finalized outbound frame on success, otherwise an error status.
  auto SendEncodedApplication(EncodedApplicationMessageView message,
                              std::uint64_t timestamp_ns,
                              SessionSendEnvelopeView envelope = {}) -> base::Result<EncodedFrame>;

  /// Finalize one owned-or-borrowed pre-encoded application body into a full FIX frame.
  ///
  /// \param message Encoded application message reference.
  /// \param timestamp_ns Wall-clock timestamp in nanoseconds.
  /// \param envelope Optional `50/57` header overrides.
  /// \return Finalized outbound frame on success, otherwise an error status.
  auto SendEncodedApplication(const EncodedApplicationMessageRef& message,
                              std::uint64_t timestamp_ns,
                              SessionSendEnvelopeView envelope = {}) -> base::Result<EncodedFrame>;

  /// Begin a graceful Logout sequence.
  ///
  /// \param text Logout text payload.
  /// \param timestamp_ns Wall-clock timestamp in nanoseconds.
  /// \return Finalized Logout frame on success, otherwise an error status.
  auto BeginLogout(std::string text, std::uint64_t timestamp_ns) -> base::Result<EncodedFrame>;

  /// Reserve replay frame capacity for an expected resend burst.
  ///
  /// \param frame_count Expected number of outbound replay frames.
  auto ReserveReplayStorage(std::size_t frame_count) -> void;

  /// \return Read-only access to the underlying session state machine.
  [[nodiscard]] auto session() const -> const SessionCore&;
  /// \return Mutable access to the underlying session state machine.
  [[nodiscard]] auto mutable_session() -> SessionCore&;

private:
  auto BuildLogonFrame(std::uint64_t timestamp_ns, bool reset_seq_num) -> base::Result<EncodedFrame>;
  auto BuildHeartbeatFrame(std::uint64_t timestamp_ns, std::string_view test_request_id) -> base::Result<EncodedFrame>;
  auto BuildTestRequestFrame(std::uint64_t timestamp_ns, std::string_view test_request_id)
    -> base::Result<EncodedFrame>;
  auto BuildResendRequestFrame(std::uint32_t begin_seq, std::uint32_t end_seq, std::uint64_t timestamp_ns)
    -> base::Result<EncodedFrame>;
  auto BuildGapFillFrame(std::uint32_t begin_seq, std::uint32_t new_seq_num, std::uint64_t timestamp_ns)
    -> base::Result<EncodedFrame>;
  auto BuildRejectFrame(std::uint32_t ref_seq_num,
                        std::string_view ref_msg_type,
                        std::uint32_t ref_tag_id,
                        std::uint32_t reject_reason,
                        std::string text,
                        std::uint64_t timestamp_ns) -> base::Result<EncodedFrame>;
  auto EncodeFrame(const message::Message& message,
                   bool admin,
                   std::uint64_t timestamp_ns,
                   bool persist,
                   bool poss_dup,
                   bool allocate_seq,
                   std::uint16_t extra_record_flags,
                   std::uint32_t seq_override = 0,
                   std::string_view orig_sending_time = {},
                   SessionSendEnvelopeView envelope = {}) -> base::Result<EncodedFrame>;
  auto EncodeFrame(message::MessageView message,
                   bool admin,
                   std::uint64_t timestamp_ns,
                   bool persist,
                   bool poss_dup,
                   bool allocate_seq,
                   std::uint16_t extra_record_flags,
                   std::uint32_t seq_override = 0,
                   std::string_view orig_sending_time = {},
                   SessionSendEnvelopeView envelope = {}) -> base::Result<EncodedFrame>;
  auto EncodeFrame(EncodedApplicationMessageView message,
                   bool admin,
                   std::uint64_t timestamp_ns,
                   bool persist,
                   bool poss_dup,
                   bool allocate_seq,
                   std::uint16_t extra_record_flags,
                   std::uint32_t seq_override = 0,
                   std::string_view orig_sending_time = {},
                   SessionSendEnvelopeView envelope = {}) -> base::Result<EncodedFrame>;
  auto FinalizeEncodedFrame(std::string_view msg_type,
                            bool admin,
                            std::uint64_t timestamp_ns,
                            bool persist,
                            bool poss_dup,
                            std::uint16_t extra_record_flags,
                            std::uint32_t seq_num) -> base::Result<EncodedFrame>;
  auto ResolveEncodeTemplate(std::string_view msg_type) -> const codec::FrameEncodeTemplate*;
  auto ReplayOutbound(std::uint32_t begin_seq,
                      std::uint32_t end_seq,
                      std::uint64_t timestamp_ns,
                      ProtocolFrameList* frames) -> base::Status;
  auto AcquireReplayFrameBuffer() -> std::shared_ptr<ProtocolFrameList>;
  auto PersistRecoveryState() -> base::Status;
  auto RefreshSessionStateFromStore() -> base::Status;
  auto ResetSessionState(std::uint32_t next_in_seq, std::uint32_t next_out_seq, bool reset_store) -> base::Status;
  auto ReplayCounterpartyExpectedRange(std::uint32_t counterparty_next_expected,
                                       std::uint32_t pre_logon_next_out,
                                       std::uint64_t timestamp_ns,
                                       ProtocolEvent* event) -> base::Status;
  auto RejectInbound(const codec::DecodedMessageView& decoded,
                     std::uint32_t ref_tag_id,
                     std::uint32_t reject_reason,
                     std::string_view text,
                     std::uint64_t timestamp_ns,
                     bool disconnect) -> base::Result<ProtocolEvent>;
  auto ValidateCompIds(const codec::DecodedMessageView& decoded,
                       std::uint32_t* ref_tag_id,
                       std::uint32_t* reject_reason,
                       std::string* text,
                       bool* disconnect) const -> bool;
  auto ValidatePossDup(const codec::DecodedMessageView& decoded) const -> base::Status;
  auto ValidateAdministrativeMessage(const codec::DecodedMessageView& decoded,
                                     std::uint32_t* ref_tag_id,
                                     std::uint32_t* reject_reason,
                                     std::string* text,
                                     bool* disconnect) const -> bool;
  auto ValidateApplicationMessage(const codec::DecodedMessageView& decoded,
                                  std::uint32_t* ref_tag_id,
                                  std::uint32_t* reject_reason,
                                  std::string* text) const -> bool;
  auto EnsureInitialized() const -> base::Status;
  auto DrainDeferredGapFrames(std::uint64_t timestamp_ns, ProtocolEvent* event) -> base::Status;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace nimble::session
