#pragma once

#include <cstdint>
#include <utility>

#include "nimblefix/advanced/encoded_application_message.h"
#include "nimblefix/advanced/session_handle.h"
#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/generated/detail/api_support.h"
#include "nimblefix/message/message_view.h"

namespace nimble::runtime {

namespace detail {

template<class Profile, class ApplicationType>
class TypedRuntimeApplication;

} // namespace detail

template<class Profile>
class Session
{
public:
  Session() = default;

  explicit Session(session::SessionHandle handle)
    : handle_(std::move(handle))
  {
  }

  [[nodiscard]] auto valid() const -> bool { return handle_.valid(); }
  [[nodiscard]] auto session_id() const -> std::uint64_t { return handle_.session_id(); }
  [[nodiscard]] auto worker_id() const -> std::uint32_t { return handle_.worker_id(); }
  auto snapshot() const -> base::Result<session::SessionSnapshot> { return handle_.Snapshot(); }
  [[nodiscard]] auto is_warmup() const -> bool
  {
    auto snap = snapshot();
    return snap.ok() && snap.value().is_warmup;
  }
  [[nodiscard]] auto raw_handle() const -> const session::SessionHandle& { return handle_; }

  auto send_message(message::Message message) -> base::Status
  {
    return handle_.Send(message::MessageRef::Take(std::move(message)));
  }

  template<class OutboundMessage>
  auto send(OutboundMessage&& outbound) -> base::Status
  {
    generated::detail::BodyEncodeBuffer body_buffer;
    auto status = outbound.EncodeBody(body_buffer);
    if (!status.ok()) {
      return status;
    }
    session::EncodedApplicationMessage encoded(OutboundMessage::kMsgType, body_buffer.bytes());
    return handle_.SendEncoded(session::EncodedApplicationMessageRef::Take(std::move(encoded)));
  }

private:
  session::SessionHandle handle_{};
};

template<class Profile>
class InlineSession
{
public:
  InlineSession() = default;

  explicit InlineSession(session::SessionHandle handle)
    : handle_(std::move(handle))
  {
  }

  [[nodiscard]] auto valid() const -> bool { return handle_.valid(); }
  [[nodiscard]] auto session_id() const -> std::uint64_t { return handle_.session_id(); }
  [[nodiscard]] auto worker_id() const -> std::uint32_t { return handle_.worker_id(); }
  auto snapshot() const -> base::Result<session::SessionSnapshot> { return handle_.Snapshot(); }
  [[nodiscard]] auto is_warmup() const -> bool
  {
    if (has_callback_warmup_) {
      return callback_warmup_;
    }
    auto snap = snapshot();
    return snap.ok() && snap.value().is_warmup;
  }
  [[nodiscard]] auto raw_handle() const -> const session::SessionHandle& { return handle_; }

  auto send_message(message::Message message) -> base::Status
  {
    return handle_.Send(message::MessageRef::Take(std::move(message)));
  }

  template<class OutboundMessage>
  auto send(OutboundMessage&& outbound) -> base::Status
  {
    generated::detail::BodyEncodeBuffer body_buffer;
    auto status = outbound.EncodeBody(body_buffer);
    if (!status.ok()) {
      return status;
    }
    session::EncodedApplicationMessage encoded(OutboundMessage::kMsgType, body_buffer.bytes());
    return handle_.SendEncoded(session::EncodedApplicationMessageRef::Take(std::move(encoded)));
  }

private:
  template<class BoundProfile, class ApplicationType>
  friend class detail::TypedRuntimeApplication;

  InlineSession(session::SessionHandle handle, bool is_warmup)
    : handle_(std::move(handle))
    , callback_warmup_(is_warmup)
    , has_callback_warmup_(true)
  {
  }

  session::SessionHandle handle_{};
  bool callback_warmup_{ false };
  bool has_callback_warmup_{ false };
};

} // namespace nimble::runtime
