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

enum class SessionNotificationKind : std::uint32_t
{
  kSessionBound = 0,
  kSessionActive,
  kSessionClosed,
  kAdminMessage,
  kApplicationMessage,
};

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

class SessionSubscriptionStream
{
public:
  virtual ~SessionSubscriptionStream() = default;

  virtual auto TryPop() -> base::Result<std::optional<SessionNotification>> = 0;
};

class SessionSubscription
{
public:
  SessionSubscription() = default;

  explicit SessionSubscription(std::shared_ptr<SessionSubscriptionStream> stream)
    : stream_(std::move(stream))
  {
  }

  [[nodiscard]] auto valid() const -> bool { return stream_ != nullptr; }

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

class SessionCommandSink
{
public:
  virtual ~SessionCommandSink() = default;

  virtual auto EnqueueOwnedMessage(std::uint64_t session_id, message::MessageRef message) -> base::Status = 0;
  virtual auto EnqueueOwnedMessageWithEnvelope(std::uint64_t session_id,
                                               message::MessageRef message,
                                               SessionSendEnvelopeRef envelope) -> base::Status
  {
    if (!envelope.empty()) {
      return base::Status::InvalidArgument("session envelope send is unsupported by this runtime command sink");
    }
    return EnqueueOwnedMessage(session_id, std::move(message));
  }
  virtual auto TrySendInlineBorrowedMessage(std::uint64_t session_id, const message::MessageRef& message)
    -> base::Result<bool>
  {
    (void)session_id;
    (void)message;
    return false;
  }
  virtual auto TrySendInlineBorrowedMessageWithEnvelope(std::uint64_t session_id,
                                                        const message::MessageRef& message,
                                                        SessionSendEnvelopeView envelope) -> base::Result<bool>
  {
    if (!envelope.empty()) {
      return base::Status::InvalidArgument("session envelope send is unsupported by this runtime command sink");
    }
    return TrySendInlineBorrowedMessage(session_id, message);
  }
  virtual auto EnqueueOwnedEncodedMessage(std::uint64_t session_id, EncodedApplicationMessageRef message)
    -> base::Status
  {
    (void)session_id;
    (void)message;
    return base::Status::InvalidArgument("encoded send is unsupported by this runtime command sink");
  }
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
  virtual auto TrySendInlineBorrowedEncodedMessage(std::uint64_t session_id,
                                                   const EncodedApplicationMessageRef& message) -> base::Result<bool>
  {
    (void)session_id;
    (void)message;
    return false;
  }
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
  virtual auto LoadSnapshot(std::uint64_t session_id) const -> base::Result<SessionSnapshot> = 0;
  virtual auto Subscribe(std::uint64_t session_id, std::size_t queue_capacity) -> base::Result<SessionSubscription> = 0;
};

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
  auto SendCopy(message::MessageView message, SessionSendEnvelopeView envelope = {}) const -> base::Status
  {
    return SubmitOwnedMessage(message::MessageRef::Copy(message), SessionSendEnvelopeRef::Own(envelope));
  }

  auto SendTake(message::Message&& message, SessionSendEnvelopeView envelope = {}) const -> base::Status
  {
    return SubmitOwnedMessage(message::MessageRef::Take(std::move(message)), SessionSendEnvelopeRef::Own(envelope));
  }

  auto SendInlineBorrowed(message::MessageView message, SessionSendEnvelopeView envelope = {}) const -> base::Status
  {
    return SubmitInlineBorrowedMessage(message::MessageRef::Borrow(message), envelope);
  }

  auto SendEncodedCopy(EncodedApplicationMessageView message, SessionSendEnvelopeView envelope = {}) const
    -> base::Status
  {
    return SubmitOwnedEncodedMessage(EncodedApplicationMessageRef::Copy(message),
                                     SessionSendEnvelopeRef::Own(envelope));
  }

  auto SendEncodedTake(EncodedApplicationMessage&& message, SessionSendEnvelopeView envelope = {}) const -> base::Status
  {
    return SubmitOwnedEncodedMessage(EncodedApplicationMessageRef::Take(std::move(message)),
                                     SessionSendEnvelopeRef::Own(envelope));
  }

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
