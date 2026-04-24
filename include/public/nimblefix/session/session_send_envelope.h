#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace nimble::session {

struct SessionSendEnvelopeView
{
  // SenderSubID(50) written into the session-managed FIX header when present.
  std::string_view sender_sub_id;
  // TargetSubID(57) written into the session-managed FIX header when present.
  std::string_view target_sub_id;

  [[nodiscard]] auto empty() const -> bool { return sender_sub_id.empty() && target_sub_id.empty(); }
};

struct SessionSendEnvelope
{
  // Use envelope fields for session-managed header tags 50/57. Do not place
  // them in the message body unless the body schema independently requires the
  // same tag numbers.
  std::string sender_sub_id;
  std::string target_sub_id;

  SessionSendEnvelope() = default;

  SessionSendEnvelope(std::string_view sender, std::string_view target)
    : sender_sub_id(sender)
    , target_sub_id(target)
  {
  }

  [[nodiscard]] auto view() const -> SessionSendEnvelopeView
  {
    return SessionSendEnvelopeView{
      .sender_sub_id = sender_sub_id,
      .target_sub_id = target_sub_id,
    };
  }

  [[nodiscard]] auto empty() const -> bool { return sender_sub_id.empty() && target_sub_id.empty(); }
};

class SessionSendEnvelopeRef
{
public:
  SessionSendEnvelopeRef() = default;

  explicit SessionSendEnvelopeRef(SessionSendEnvelope envelope)
    : owned_(std::make_shared<SessionSendEnvelope>(std::move(envelope)))
  {
  }

  explicit SessionSendEnvelopeRef(SessionSendEnvelopeView view)
    : view_(view)
  {
  }

  static auto Own(SessionSendEnvelopeView view) -> SessionSendEnvelopeRef
  {
    if (view.empty()) {
      return SessionSendEnvelopeRef(view);
    }
    return SessionSendEnvelopeRef(SessionSendEnvelope(view.sender_sub_id, view.target_sub_id));
  }

  [[nodiscard]] auto empty() const -> bool { return view().empty(); }

  [[nodiscard]] auto owns_envelope() const -> bool { return owned_ != nullptr; }

  [[nodiscard]] auto view() const -> SessionSendEnvelopeView
  {
    if (owned_ != nullptr) {
      return owned_->view();
    }
    return view_;
  }

private:
  std::shared_ptr<const SessionSendEnvelope> owned_{};
  SessionSendEnvelopeView view_{};
};

} // namespace nimble::session