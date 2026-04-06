#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "fastfix/base/result.h"
#include "fastfix/base/status.h"
#include "fastfix/message/message.h"
#include "fastfix/session/session_key.h"
#include "fastfix/session/session_snapshot.h"

namespace fastfix::session {

enum class SessionNotificationKind : std::uint32_t {
    kSessionBound = 0,
    kSessionActive,
    kSessionClosed,
    kAdminMessage,
    kApplicationMessage,
};

struct SessionNotification {
    SessionNotificationKind kind{SessionNotificationKind::kSessionBound};
    SessionSnapshot snapshot;
    SessionKey session_key;
    message::MessageRef message;
    std::string text;
    std::uint64_t timestamp_ns{0};

    [[nodiscard]] auto message_view() const -> message::MessageView {
        return message.view();
    }
};

class SessionSubscriptionStream {
  public:
    virtual ~SessionSubscriptionStream() = default;

    virtual auto TryPop() -> base::Result<std::optional<SessionNotification>> = 0;
};

class SessionSubscription {
  public:
    SessionSubscription() = default;

    explicit SessionSubscription(std::shared_ptr<SessionSubscriptionStream> stream)
        : stream_(std::move(stream)) {
    }

    [[nodiscard]] auto valid() const -> bool {
        return stream_ != nullptr;
    }

    auto TryPop() -> base::Result<std::optional<SessionNotification>> {
        if (stream_ == nullptr) {
            return base::Status::InvalidArgument("session subscription is invalid");
        }
        return stream_->TryPop();
    }

  private:
    std::shared_ptr<SessionSubscriptionStream> stream_;
};

class SessionCommandSink {
  public:
    virtual ~SessionCommandSink() = default;

        virtual auto EnqueueSend(std::uint64_t session_id, message::MessageRef message) -> base::Status = 0;
        virtual auto EnqueueSendBorrowed(std::uint64_t session_id, const message::MessageRef& message)
                -> base::Result<bool> {
                (void)session_id;
                (void)message;
                return false;
        }
    virtual auto LoadSnapshot(std::uint64_t session_id) const -> base::Result<SessionSnapshot> = 0;
    virtual auto Subscribe(std::uint64_t session_id, std::size_t queue_capacity) -> base::Result<SessionSubscription> = 0;
};

class SessionHandle {
  public:
    SessionHandle() = default;

    SessionHandle(std::uint64_t session_id, std::uint32_t worker_id)
        : session_id_(session_id), worker_id_(worker_id) {
    }

    SessionHandle(
        std::uint64_t session_id,
        std::uint32_t worker_id,
        std::shared_ptr<SessionCommandSink> command_sink)
        : session_id_(session_id),
          worker_id_(worker_id),
          command_sink_(std::move(command_sink)) {
    }

    [[nodiscard]] bool valid() const {
        return session_id_ != 0;
    }

    [[nodiscard]] bool sendable() const {
        return valid() && command_sink_ != nullptr;
    }

    [[nodiscard]] std::uint64_t session_id() const {
        return session_id_;
    }

    [[nodiscard]] std::uint32_t worker_id() const {
        return worker_id_;
    }

    auto Snapshot() const -> base::Result<SessionSnapshot> {
        if (!valid()) {
            return base::Status::InvalidArgument("session handle is invalid");
        }
        if (command_sink_ == nullptr) {
            return base::Status::InvalidArgument("session handle is not bound to a runtime command sink");
        }
        return command_sink_->LoadSnapshot(session_id_);
    }

    auto Subscribe(std::size_t queue_capacity = 256U) const -> base::Result<SessionSubscription> {
        if (!valid()) {
            return base::Status::InvalidArgument("session handle is invalid");
        }
        if (command_sink_ == nullptr) {
            return base::Status::InvalidArgument("session handle is not bound to a runtime command sink");
        }
        return command_sink_->Subscribe(session_id_, queue_capacity);
    }

    auto Send(const message::Message& message) const -> base::Status {
        if (!valid()) {
            return base::Status::InvalidArgument("session handle is invalid");
        }
        if (command_sink_ == nullptr) {
            return base::Status::InvalidArgument("session handle is not bound to a runtime command sink");
        }
        return command_sink_->EnqueueSend(session_id_, message::MessageRef(message::Message(message.data())));
    }

    auto Send(message::MessageView view) const -> base::Status {
        if (!valid()) {
            return base::Status::InvalidArgument("session handle is invalid");
        }
        if (command_sink_ == nullptr) {
            return base::Status::InvalidArgument("session handle is not bound to a runtime command sink");
        }
        return command_sink_->EnqueueSend(session_id_, message::MessageRef::Own(view));
    }

    auto Send(const message::MessageRef& message) const -> base::Status {
        if (!valid()) {
            return base::Status::InvalidArgument("session handle is invalid");
        }
        if (command_sink_ == nullptr) {
            return base::Status::InvalidArgument("session handle is not bound to a runtime command sink");
        }
        if (message.owns_message()) {
            return command_sink_->EnqueueSend(session_id_, message);
        }
        return command_sink_->EnqueueSend(session_id_, message::MessageRef::Own(message.view()));
    }

    auto SendBorrowed(message::MessageView view) const -> base::Status {
        return SendBorrowed(message::MessageRef(view));
    }

    auto SendBorrowed(const message::MessageRef& message) const -> base::Status {
        if (!valid()) {
            return base::Status::InvalidArgument("session handle is invalid");
        }
        if (command_sink_ == nullptr) {
            return base::Status::InvalidArgument("session handle is not bound to a runtime command sink");
        }
        if (message.owns_message()) {
            return command_sink_->EnqueueSend(session_id_, message);
        }
        auto queued = command_sink_->EnqueueSendBorrowed(session_id_, message);
        if (!queued.ok()) {
            return queued.status();
        }
        if (queued.value()) {
            return base::Status::Ok();
        }
        return base::Status::InvalidArgument("borrowed send requires runtime inline callback context");
    }

    auto Send(message::Message&& message) const -> base::Status {
        if (!valid()) {
            return base::Status::InvalidArgument("session handle is invalid");
        }
        if (command_sink_ == nullptr) {
            return base::Status::InvalidArgument("session handle is not bound to a runtime command sink");
        }
        return command_sink_->EnqueueSend(session_id_, message::MessageRef(std::move(message)));
    }

  private:
    std::uint64_t session_id_{0};
    std::uint32_t worker_id_{0};
    std::shared_ptr<SessionCommandSink> command_sink_;
};

}  // namespace fastfix::session
