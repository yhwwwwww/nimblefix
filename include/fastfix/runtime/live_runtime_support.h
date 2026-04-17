#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string_view>
#include <thread>

#include "fastfix/base/status.h"

namespace fastfix::runtime {

inline constexpr std::string_view kSingleProducerSendViolation =
  "runtime session send path requires a single producer thread per "
  "SessionHandle command sink";

inline thread_local const void* g_inline_borrow_send_sink = nullptr;

[[nodiscard]] inline auto
CurrentProducerToken() -> std::size_t
{
  return std::hash<std::thread::id>{}(std::this_thread::get_id()) + 1U;
}

class BorrowedSendScope
{
public:
  explicit BorrowedSendScope(const void* sink)
    : previous_(g_inline_borrow_send_sink)
  {
    g_inline_borrow_send_sink = sink;
  }

  BorrowedSendScope(const BorrowedSendScope&) = delete;
  auto operator=(const BorrowedSendScope&) -> BorrowedSendScope& = delete;

  ~BorrowedSendScope() { g_inline_borrow_send_sink = previous_; }

private:
  const void* previous_{ nullptr };
};

class SingleProducerGuard
{
public:
  auto Validate(std::string_view violation = kSingleProducerSendViolation) -> base::Status
  {
    const auto token = CurrentProducerToken();
    std::size_t expected = 0U;
    if (producer_thread_token_.compare_exchange_strong(expected, token, std::memory_order_relaxed)) {
      return base::Status::Ok();
    }
    if (expected == token) {
      return base::Status::Ok();
    }
    return base::Status::InvalidArgument(std::string(violation));
  }

private:
  std::atomic<std::size_t> producer_thread_token_{ 0U };
};

} // namespace fastfix::runtime