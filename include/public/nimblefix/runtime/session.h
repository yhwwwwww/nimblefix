#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "nimblefix/advanced/encoded_application_message.h"
#include "nimblefix/advanced/session_handle.h"
#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/generated/detail/api_support.h"
#include "nimblefix/message/message_view.h"

namespace nimble::runtime {

/// Append-only hybrid extras for outbound messages.
///
/// These fragments are appended verbatim to the final FIX message without
/// runtime reordering.  The resulting wire-level byte order is:
///
///   1. Managed header  — BeginString, BodyLength, MsgType, MsgSeqNum,
///                        SenderCompID, TargetCompID, SendingTime, …
///   2. header_fragment — pre-encoded header-level tags (appended after
///                        session-managed header fields)
///   3. Static body     — schema-known fields in canonical dictionary order
///                        (emitted by the generated Builder::EncodeBody)
///   4. Raw-static extras — compile-time set_tag<Tag>() values in source order
///   5. body_fragment   — pre-encoded body-level tags (appended after the
///                        static body and raw-static extras)
///   6. Trailer         — CheckSum
///
/// Hybrid fragments are strictly append-only: mid-insertion and reordering
/// are intentionally unsupported.  The final message does not guarantee
/// global canonical tag order; only the header / body / trailer structure
/// is preserved.
struct SendExtras
{
  std::string_view header_fragment;
  std::string_view body_fragment;
};

namespace detail {

template<class Msg, class Populate>
auto EncodeAndSend(session::SessionHandle& handle, Populate&& populate, SendExtras extras) -> base::Status
{
  typename Msg::Builder builder;
  std::forward<Populate>(populate)(builder);
  generated::detail::BodyEncodeBuffer body_buffer;
  auto status = builder.EncodeBody(body_buffer);
  if (!status.ok()) {
    return status;
  }
  session::EncodedApplicationMessage encoded(Msg::kMsgType, body_buffer.bytes());
  if (!extras.header_fragment.empty() || !extras.body_fragment.empty()) {
    encoded.extras = codec::EncodedOutboundExtras{
      std::string(extras.header_fragment),
      std::string(extras.body_fragment),
    };
  }
  return handle.SendEncoded(session::EncodedApplicationMessageRef::Take(std::move(encoded)));
}

template<class Msg, class Populate>
auto EncodeAndSendInline(session::SessionHandle& handle, Populate&& populate, SendExtras extras) -> base::Status
{
  typename Msg::Builder builder;
  std::forward<Populate>(populate)(builder);
  generated::detail::BodyEncodeBuffer body_buffer;
  auto status = builder.EncodeBody(body_buffer);
  if (!status.ok()) {
    return status;
  }
  codec::EncodedOutboundExtrasView extras_view;
  if (!extras.header_fragment.empty() || !extras.body_fragment.empty()) {
    extras_view = codec::EncodedOutboundExtrasView{
      extras.header_fragment,
      extras.body_fragment,
    };
  }
  session::EncodedApplicationMessageView view{
    .msg_type = Msg::kMsgType,
    .body = body_buffer.bytes(),
    .extras = extras_view,
  };
  return handle.SendEncoded(session::EncodedApplicationMessageRef::Borrow(view));
}

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

  template<class Msg, class Populate>
  auto send(Populate&& populate, SendExtras extras = {}) -> base::Status
  {
    return detail::EncodeAndSend<Msg>(handle_, std::forward<Populate>(populate), extras);
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

  template<class Msg, class Populate>
  auto send(Populate&& populate, SendExtras extras = {}) -> base::Status
  {
    return detail::EncodeAndSendInline<Msg>(handle_, std::forward<Populate>(populate), extras);
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
