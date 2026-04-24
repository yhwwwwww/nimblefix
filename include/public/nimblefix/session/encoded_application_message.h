#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nimblefix/session/encoded_frame.h"

namespace nimble::session {

inline constexpr std::size_t kEncodedApplicationInlineCapacity = kEncodedFrameInlineCapacity;

/// Small-buffer storage for a pre-encoded application body.
///
/// This does not hold a complete FIX frame. Session-managed header/trailer
/// fields are added later by `AdminProtocol` or `SessionHandle` send paths.
struct EncodedApplicationBytes
{
  std::array<std::byte, kEncodedApplicationInlineCapacity> inline_storage{};
  std::size_t inline_size{ 0U };
  std::vector<std::byte> overflow_storage;

  /// Copy an application body into local storage.
  ///
  /// \param bytes Encoded application-message body bytes only.
  auto assign(std::span<const std::byte> bytes) -> void
  {
    if (bytes.size() <= kEncodedApplicationInlineCapacity) {
      inline_size = bytes.size();
      overflow_storage.clear();
      std::copy(bytes.begin(), bytes.end(), inline_storage.begin());
      return;
    }

    inline_size = 0U;
    overflow_storage.assign(bytes.begin(), bytes.end());
  }

  /// \return Borrowed span over inline or overflow storage.
  [[nodiscard]] auto view() const -> std::span<const std::byte>
  {
    if (!overflow_storage.empty()) {
      return std::span<const std::byte>(overflow_storage.data(), overflow_storage.size());
    }
    return std::span<const std::byte>(inline_storage.data(), inline_size);
  }

  [[nodiscard]] auto size() const -> std::size_t
  {
    return overflow_storage.empty() ? inline_size : overflow_storage.size();
  }

  [[nodiscard]] auto empty() const -> bool { return size() == 0U; }

  operator std::span<const std::byte>() const { return view(); }
};

/// Borrowed pre-encoded application message.
///
/// Boundary condition: `body` is the application payload only. Callers must not
/// pass a complete FIX frame through this API.
struct EncodedApplicationMessageView
{
  std::string_view msg_type;
  std::span<const std::byte> body;

  [[nodiscard]] auto valid() const -> bool { return !msg_type.empty(); }
};

/// Owned pre-encoded application message.
///
/// Design intent: let callers pre-encode application bodies while keeping
/// session-managed FIX framing inside the runtime.
struct EncodedApplicationMessage
{
  std::string msg_type;
  EncodedApplicationBytes body;

  EncodedApplicationMessage() = default;

  /// \param type Application `MsgType(35)`.
  /// \param encoded_body Encoded application body bytes only.
  EncodedApplicationMessage(std::string_view type, std::span<const std::byte> encoded_body)
    : msg_type(type)
  {
    body.assign(encoded_body);
  }

  /// \return Borrowed view over this owned message.
  [[nodiscard]] auto view() const -> EncodedApplicationMessageView
  {
    return EncodedApplicationMessageView{
      .msg_type = msg_type,
      .body = body.view(),
    };
  }

  [[nodiscard]] auto valid() const -> bool { return !msg_type.empty(); }
};

/// Owned-or-borrowed reference wrapper for pre-encoded application bodies.
///
/// Performance: use `Borrow()` only when the body outlives the immediate send
/// call. `Copy()` is the safe convenience path; `Take()` avoids the extra copy
/// when the caller can transfer ownership.
class EncodedApplicationMessageRef
{
public:
  EncodedApplicationMessageRef() = default;

  /// Transfer ownership of an encoded application message.
  ///
  /// \param message Owned application message.
  /// \return Reference wrapper owning the message storage.
  static auto Take(EncodedApplicationMessage&& message) -> EncodedApplicationMessageRef
  {
    return EncodedApplicationMessageRef(std::make_shared<EncodedApplicationMessage>(std::move(message)));
  }

  /// Borrow an encoded application body.
  ///
  /// \param view Borrowed message view.
  /// \return Non-owning reference wrapper.
  static auto Borrow(EncodedApplicationMessageView view) -> EncodedApplicationMessageRef
  {
    return EncodedApplicationMessageRef(view);
  }

  /// Copy an encoded application body into owned storage when needed.
  ///
  /// \param view Source message view.
  /// \return Owning wrapper for valid messages, otherwise a borrowed invalid wrapper.
  static auto Copy(EncodedApplicationMessageView view) -> EncodedApplicationMessageRef
  {
    if (!view.valid()) {
      return Borrow(view);
    }
    return Take(EncodedApplicationMessage(view.msg_type, view.body));
  }

  [[nodiscard]] auto valid() const -> bool
  {
    if (owned_ != nullptr) {
      return owned_->valid();
    }
    return view_.valid();
  }

  [[nodiscard]] auto owns_storage() const -> bool { return owned_ != nullptr; }

  [[nodiscard]] auto borrows_view() const -> bool { return owned_ == nullptr && view_.valid(); }

  [[nodiscard]] auto view() const -> EncodedApplicationMessageView
  {
    if (owned_ != nullptr) {
      return owned_->view();
    }
    return view_;
  }

private:
  explicit EncodedApplicationMessageRef(std::shared_ptr<const EncodedApplicationMessage> owned)
    : owned_(std::move(owned))
  {
  }

  explicit EncodedApplicationMessageRef(EncodedApplicationMessageView view)
    : view_(view)
  {
  }

  std::shared_ptr<const EncodedApplicationMessage> owned_{};
  EncodedApplicationMessageView view_{};
};

} // namespace nimble::session