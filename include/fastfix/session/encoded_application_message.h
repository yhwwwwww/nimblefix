#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fastfix/session/encoded_frame.h"

namespace fastfix::session {

inline constexpr std::size_t kEncodedApplicationInlineCapacity = kEncodedFrameInlineCapacity;

struct EncodedApplicationBytes
{
  std::array<std::byte, kEncodedApplicationInlineCapacity> inline_storage{};
  std::size_t inline_size{ 0U };
  std::vector<std::byte> overflow_storage;

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

struct EncodedApplicationMessageView
{
  std::string_view msg_type;
  std::span<const std::byte> body;

  [[nodiscard]] auto valid() const -> bool { return !msg_type.empty(); }
};

struct EncodedApplicationMessage
{
  std::string msg_type;
  EncodedApplicationBytes body;

  EncodedApplicationMessage() = default;

  EncodedApplicationMessage(std::string_view type, std::span<const std::byte> encoded_body)
    : msg_type(type)
  {
    body.assign(encoded_body);
  }

  [[nodiscard]] auto view() const -> EncodedApplicationMessageView
  {
    return EncodedApplicationMessageView{
      .msg_type = msg_type,
      .body = body.view(),
    };
  }

  [[nodiscard]] auto valid() const -> bool { return !msg_type.empty(); }
};

class EncodedApplicationMessageRef
{
public:
  EncodedApplicationMessageRef() = default;

  explicit EncodedApplicationMessageRef(EncodedApplicationMessage message)
    : owned_(std::make_shared<EncodedApplicationMessage>(std::move(message)))
  {
  }

  explicit EncodedApplicationMessageRef(EncodedApplicationMessageView view)
    : view_(view)
  {
  }

  static auto Own(EncodedApplicationMessageView view) -> EncodedApplicationMessageRef
  {
    if (!view.valid()) {
      return EncodedApplicationMessageRef(view);
    }
    return EncodedApplicationMessageRef(EncodedApplicationMessage(view.msg_type, view.body));
  }

  [[nodiscard]] auto valid() const -> bool
  {
    if (owned_ != nullptr) {
      return owned_->valid();
    }
    return view_.valid();
  }

  [[nodiscard]] auto owns_message() const -> bool { return owned_ != nullptr; }

  [[nodiscard]] auto view() const -> EncodedApplicationMessageView
  {
    if (owned_ != nullptr) {
      return owned_->view();
    }
    return view_;
  }

private:
  std::shared_ptr<const EncodedApplicationMessage> owned_{};
  EncodedApplicationMessageView view_{};
};

} // namespace fastfix::session