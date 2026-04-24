#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/message/message_ref.h"
#include "nimblefix/session/encoded_application_message.h"
#include "nimblefix/session/session_key.h"
#include "nimblefix/session/session_send_envelope.h"
#include "nimblefix/session/session_snapshot.h"

namespace nimble::session {

/// Session-scoped notification categories delivered through subscriptions.
///
/// `kSessionBound` means the runtime has attached the FIX session to a worker
/// but recovery/logon completion may still be pending. `kSessionActive` means
/// application traffic may flow. Message variants carry the parsed admin/app
/// message in `SessionNotification::message`.
enum class SessionNotificationKind : std::uint32_t
{
  kSessionBound = 0,
  kSessionActive,
  kSessionClosed,
  kAdminMessage,
  kApplicationMessage,
};

/// Lightweight event payload delivered by `SessionSubscription`.
///
/// Design intent: expose the same high-level state transitions as runtime
/// callbacks without forcing the caller onto the worker thread.
///
/// Boundary condition: `message` is populated only for message kinds, and
/// `text` is meaningful only for events that attach explanatory text.
struct SessionNotification
{
  SessionNotificationKind kind{ SessionNotificationKind::kSessionBound };
  SessionSnapshot snapshot;
  SessionKey session_key;
  message::MessageRef message;
  std::string text;
  std::uint64_t timestamp_ns{ 0 };

  [[nodiscard]] auto message_view() const -> message::MessageView { return message.view(); }
};

/// Non-blocking event source for one subscribed session.
///
/// `TryPop()` returns immediately. A transport/runtime error is reported via the
/// `Result` status instead of blocking for recovery.
class SessionSubscriptionStream
{
public:
  virtual ~SessionSubscriptionStream() = default;

  /// Attempt to pop one queued notification.
  ///
  /// \return `std::nullopt` when no event is available yet, or a status on error.
  virtual auto TryPop() -> base::Result<std::optional<SessionNotification>> = 0;
};

/// Small owning handle around a subscription stream.
///
/// Default construction yields an invalid handle. Copies are cheap because the
/// underlying stream is reference counted.
class SessionSubscription
{
public:
  SessionSubscription() = default;

  explicit SessionSubscription(std::shared_ptr<SessionSubscriptionStream> stream)
    : stream_(std::move(stream))
  {
  }

  [[nodiscard]] auto valid() const -> bool { return stream_ != nullptr; }

  /// Attempt to pop one queued notification.
  ///
  /// \return `std::nullopt` when the queue is empty, or a status on error.
  auto TryPop() -> base::Result<std::optional<SessionNotification>>
  {
    if (stream_ == nullptr) {
      return base::Status::InvalidArgument("session subscription is invalid");
    }
    return stream_->TryPop();
  }

private:
  std::shared_ptr<SessionSubscriptionStream> stream_;
};

/// Runtime-owned command bridge behind `SessionHandle`.
///
/// Design intent: decouple the public handle API from the concrete runtime
/// queueing/execution model. Borrowed-send methods are synchronous probes: an
/// implementation must either finish consumption before returning or reject the
/// call, because the borrowed payload may die at the caller's next statement.
class SessionCommandSink
{
public:
  virtual ~SessionCommandSink() = default;

  /// Queue an owned decoded message for session-managed send.
  ///
  /// \param session_id Runtime session id.
  /// \param message Owned message payload.
  /// \return `Ok()` when accepted, otherwise an error status.
  virtual auto EnqueueOwnedMessage(std::uint64_t session_id, message::MessageRef message) -> base::Status = 0;

  /// Queue an owned decoded message plus per-send envelope overrides.
  ///
  /// \param session_id Runtime session id.
  /// \param message Owned message payload.
  /// \param envelope Optional `50/57` header overrides.
  /// \return `Ok()` when accepted, otherwise an error status.
  virtual auto EnqueueOwnedMessageWithEnvelope(std::uint64_t session_id,
                                               message::MessageRef message,
                                               SessionSendEnvelopeRef envelope) -> base::Status
  {
    if (!envelope.empty()) {
      return base::Status::InvalidArgument("session envelope send is unsupported by this runtime command sink");
    }
    return EnqueueOwnedMessage(session_id, std::move(message));
  }

  /// Attempt the inline borrowed decoded-message fast path.
  ///
  /// \param session_id Runtime session id.
  /// \param message Borrowed message view valid only for the duration of this call.
  /// \return `true` when consumed inline, `false` when the caller should fall back.
  virtual auto TrySendInlineBorrowedMessage(std::uint64_t session_id, const message::MessageRef& message)
    -> base::Result<bool>
  {
    (void)session_id;
    (void)message;
    return false;
  }

  /// Attempt the inline borrowed decoded-message fast path with envelope overrides.
  ///
  /// \param session_id Runtime session id.
  /// \param message Borrowed message view valid only for the duration of this call.
  /// \param envelope Optional `50/57` header overrides.
  /// \return `true` when consumed inline, `false` when the caller should fall back.
  virtual auto TrySendInlineBorrowedMessageWithEnvelope(std::uint64_t session_id,
                                                        const message::MessageRef& message,
                                                        SessionSendEnvelopeView envelope) -> base::Result<bool>
  {
    if (!envelope.empty()) {
      return base::Status::InvalidArgument("session envelope send is unsupported by this runtime command sink");
    }
    return TrySendInlineBorrowedMessage(session_id, message);
  }

  /// Queue an owned pre-encoded application body for session finalization.
  ///
  /// \param session_id Runtime session id.
  /// \param message Owned pre-encoded application message.
  /// \return `Ok()` when accepted, otherwise an error status.
  virtual auto EnqueueOwnedEncodedMessage(std::uint64_t session_id, EncodedApplicationMessageRef message)
    -> base::Status
  {
    (void)session_id;
    (void)message;
    return base::Status::InvalidArgument("encoded send is unsupported by this runtime command sink");
  }

  /// Queue an owned pre-encoded application body plus envelope overrides.
  ///
  /// \param session_id Runtime session id.
  /// \param message Owned pre-encoded application message.
  /// \param envelope Optional `50/57` header overrides.
  /// \return `Ok()` when accepted, otherwise an error status.
  virtual auto EnqueueOwnedEncodedMessageWithEnvelope(std::uint64_t session_id,
                                                      EncodedApplicationMessageRef message,
                                                      SessionSendEnvelopeRef envelope) -> base::Status
  {
    if (!envelope.empty()) {
      return base::Status::InvalidArgument("session envelope encoded send is unsupported by this runtime "
                                           "command sink");
    }
    return EnqueueOwnedEncodedMessage(session_id, std::move(message));
  }

  /// Attempt the inline borrowed pre-encoded fast path.
  ///
  /// \param session_id Runtime session id.
  /// \param message Borrowed pre-encoded body valid only for the duration of this call.
  /// \return `true` when consumed inline, `false` when the caller should fall back.
  virtual auto TrySendInlineBorrowedEncodedMessage(std::uint64_t session_id,
                                                   const EncodedApplicationMessageRef& message) -> base::Result<bool>
  {
    (void)session_id;
    (void)message;
    return false;
  }

  /// Attempt the inline borrowed pre-encoded fast path with envelope overrides.
  ///
  /// \param session_id Runtime session id.
  /// \param message Borrowed pre-encoded body valid only for the duration of this call.
  /// \param envelope Optional `50/57` header overrides.
  /// \return `true` when consumed inline, `false` when the caller should fall back.
  virtual auto TrySendInlineBorrowedEncodedMessageWithEnvelope(std::uint64_t session_id,
                                                               const EncodedApplicationMessageRef& message,
                                                               SessionSendEnvelopeView envelope) -> base::Result<bool>
  {
    if (!envelope.empty()) {
      return base::Status::InvalidArgument("session envelope encoded send is unsupported by this runtime "
                                           "command sink");
    }
    return TrySendInlineBorrowedEncodedMessage(session_id, message);
  }

  /// Load the latest runtime-maintained session snapshot.
  ///
  /// \param session_id Runtime session id.
  /// \return Snapshot on success, otherwise an error status.
  virtual auto LoadSnapshot(std::uint64_t session_id) const -> base::Result<SessionSnapshot> = 0;

  /// Subscribe to asynchronous notifications for one session.
  ///
  /// \param session_id Runtime session id.
  /// \param queue_capacity Per-session notification queue capacity.
  /// \return Subscription handle on success, otherwise an error status.
  virtual auto Subscribe(std::uint64_t session_id, std::size_t queue_capacity) -> base::Result<SessionSubscription> = 0;
};

/// Public send/control handle for one runtime session.
///
/// Design intent: hand application code a small value object that can snapshot,
/// subscribe, and send without exposing runtime internals.
///
/// Performance/lifecycle contract:
/// - all send methods share a single-producer command path; one producer thread
///   must own each handle
/// - `SendCopy()` copies payloads for safety
/// - `SendTake()` avoids an extra copy by transferring ownership
/// - `SendInlineBorrowed()` and encoded inline variants are the zero-copy fast
///   path, but only while executing inside a direct runtime inline callback
///
/// Boundary condition: a default-constructed or unbound handle reports
/// `InvalidArgument` instead of silently dropping work.
class SessionHandle
{
public:
  // SessionHandle send methods share a single-producer command path. Use one
  // producer thread per handle. Cross-thread send attempts fast-fail instead of
  // silently violating the runtime's SPSC queue contract.
  //
  // SendInlineBorrowed()/SendEncodedInlineBorrowed() are stricter: they are
  // valid only from direct runtime inline callbacks such as
  // ApplicationCallbacks::OnSessionEvent/OnAdminMessage/OnAppMessage. Queue
  // handlers and arbitrary application threads must use owned send variants.
  SessionHandle() = default;

  SessionHandle(std::uint64_t session_id, std::uint32_t worker_id)
    : session_id_(session_id)
    , worker_id_(worker_id)
  {
  }

  SessionHandle(std::uint64_t session_id, std::uint32_t worker_id, std::shared_ptr<SessionCommandSink> command_sink)
    : session_id_(session_id)
    , worker_id_(worker_id)
    , command_sink_(std::move(command_sink))
  {
  }

  [[nodiscard]] bool valid() const { return session_id_ != 0; }

  [[nodiscard]] bool sendable() const { return valid() && command_sink_ != nullptr; }

  [[nodiscard]] std::uint64_t session_id() const { return session_id_; }

  [[nodiscard]] std::uint32_t worker_id() const { return worker_id_; }

  /// Load the latest runtime-maintained snapshot for this session.
  ///
  /// \return Snapshot on success, otherwise an error status.
  auto Snapshot() const -> base::Result<SessionSnapshot>
  {
    if (!valid()) {
      return base::Status::InvalidArgument("session handle is invalid");
    }
    if (command_sink_ == nullptr) {
      return base::Status::InvalidArgument("session handle is not bound to a runtime command sink");
    }
    return command_sink_->LoadSnapshot(session_id_);
  }

  /// Subscribe to queued notifications for this session.
  ///
  /// \param queue_capacity Per-session queue capacity. Higher values tolerate
  /// short consumer stalls at the cost of memory.
  /// \return Subscription handle on success, otherwise an error status.
  auto Subscribe(std::size_t queue_capacity = 256U) const -> base::Result<SessionSubscription>
  {
    if (!valid()) {
      return base::Status::InvalidArgument("session handle is invalid");
    }
    if (command_sink_ == nullptr) {
      return base::Status::InvalidArgument("session handle is not bound to a runtime command sink");
    }
    return command_sink_->Subscribe(session_id_, queue_capacity);
  }

public:
  /// Copy a decoded message into the runtime send path.
  ///
  /// \param message Borrowed source view; payload is copied before the call returns.
  /// \param envelope Optional `50/57` header overrides.
  /// \return `Ok()` when accepted, otherwise an error status.
  auto SendCopy(message::MessageView message, SessionSendEnvelopeView envelope = {}) const -> base::Status
  {
    return SubmitOwnedMessage(message::MessageRef::Copy(message), SessionSendEnvelopeRef::Own(envelope));
  }

  /// Move an owned decoded message into the runtime send path.
  ///
  /// \param message Owned message transferred to the runtime.
  /// \param envelope Optional `50/57` header overrides.
  /// \return `Ok()` when accepted, otherwise an error status.
  auto SendTake(message::Message&& message, SessionSendEnvelopeView envelope = {}) const -> base::Status
  {
    return SubmitOwnedMessage(message::MessageRef::Take(std::move(message)), SessionSendEnvelopeRef::Own(envelope));
  }

  /// Attempt the inline borrowed decoded-message fast path.
  ///
  /// This path avoids an application-side copy but is only legal from direct
  /// runtime inline callbacks.
  ///
  /// \param message Borrowed decoded message view.
  /// \param envelope Optional `50/57` header overrides.
  /// \return `Ok()` when the runtime consumed the borrow inline, otherwise an error status.
  auto SendInlineBorrowed(message::MessageView message, SessionSendEnvelopeView envelope = {}) const -> base::Status
  {
    return SubmitInlineBorrowedMessage(message::MessageRef::Borrow(message), envelope);
  }

  /// Copy a pre-encoded application body into the runtime send path.
  ///
  /// The runtime still owns session-managed FIX header/trailer fields.
  ///
  /// \param message Borrowed pre-encoded application message.
  /// \param envelope Optional `50/57` header overrides.
  /// \return `Ok()` when accepted, otherwise an error status.
  auto SendEncodedCopy(EncodedApplicationMessageView message, SessionSendEnvelopeView envelope = {}) const
    -> base::Status
  {
    return SubmitOwnedEncodedMessage(EncodedApplicationMessageRef::Copy(message),
                                     SessionSendEnvelopeRef::Own(envelope));
  }

  /// Move an owned pre-encoded application body into the runtime send path.
  ///
  /// \param message Owned pre-encoded application message.
  /// \param envelope Optional `50/57` header overrides.
  /// \return `Ok()` when accepted, otherwise an error status.
  auto SendEncodedTake(EncodedApplicationMessage&& message, SessionSendEnvelopeView envelope = {}) const -> base::Status
  {
    return SubmitOwnedEncodedMessage(EncodedApplicationMessageRef::Take(std::move(message)),
                                     SessionSendEnvelopeRef::Own(envelope));
  }

  /// Attempt the inline borrowed pre-encoded fast path.
  ///
  /// This path is valid only from direct runtime inline callbacks.
  ///
  /// \param message Borrowed pre-encoded application message.
  /// \param envelope Optional `50/57` header overrides.
  /// \return `Ok()` when the runtime consumed the borrow inline, otherwise an error status.
  auto SendEncodedInlineBorrowed(EncodedApplicationMessageView message, SessionSendEnvelopeView envelope = {}) const
    -> base::Status
  {
    return SubmitInlineBorrowedEncodedMessage(EncodedApplicationMessageRef::Borrow(message), envelope);
  }

private:
  auto EnsureSendable() const -> base::Status
  {
    if (!valid()) {
      return base::Status::InvalidArgument("session handle is invalid");
    }
    if (command_sink_ == nullptr) {
      return base::Status::InvalidArgument("session handle is not bound to a runtime command sink");
    }
    return base::Status::Ok();
  }

  auto SubmitOwnedMessage(message::MessageRef message, SessionSendEnvelopeRef envelope) const -> base::Status
  {
    auto status = EnsureSendable();
    if (!status.ok()) {
      return status;
    }
    return command_sink_->EnqueueOwnedMessageWithEnvelope(session_id_, std::move(message), std::move(envelope));
  }

  auto SubmitInlineBorrowedMessage(const message::MessageRef& message, SessionSendEnvelopeView envelope) const
    -> base::Status
  {
    auto status = EnsureSendable();
    if (!status.ok()) {
      return status;
    }
    if (message.owns_storage()) {
      return command_sink_->EnqueueOwnedMessageWithEnvelope(
        session_id_, message, SessionSendEnvelopeRef::Own(envelope));
    }
    auto queued = command_sink_->TrySendInlineBorrowedMessageWithEnvelope(session_id_, message, envelope);
    if (!queued.ok()) {
      return queued.status();
    }
    if (queued.value()) {
      return base::Status::Ok();
    }
    return base::Status::InvalidArgument("inline borrowed send requires runtime inline callback context");
  }

  auto SubmitOwnedEncodedMessage(EncodedApplicationMessageRef message, SessionSendEnvelopeRef envelope) const
    -> base::Status
  {
    auto status = EnsureSendable();
    if (!status.ok()) {
      return status;
    }
    return command_sink_->EnqueueOwnedEncodedMessageWithEnvelope(session_id_, std::move(message), std::move(envelope));
  }

  auto SubmitInlineBorrowedEncodedMessage(const EncodedApplicationMessageRef& message,
                                          SessionSendEnvelopeView envelope) const -> base::Status
  {
    auto status = EnsureSendable();
    if (!status.ok()) {
      return status;
    }
    if (message.owns_storage()) {
      return command_sink_->EnqueueOwnedEncodedMessageWithEnvelope(
        session_id_, message, SessionSendEnvelopeRef::Own(envelope));
    }
    auto queued = command_sink_->TrySendInlineBorrowedEncodedMessageWithEnvelope(session_id_, message, envelope);
    if (!queued.ok()) {
      return queued.status();
    }
    if (queued.value()) {
      return base::Status::Ok();
    }
    return base::Status::InvalidArgument("inline borrowed encoded send requires runtime inline callback context");
  }

  std::uint64_t session_id_{ 0 };
  std::uint32_t worker_id_{ 0 };
  std::shared_ptr<SessionCommandSink> command_sink_;
};

} // namespace nimble::session
