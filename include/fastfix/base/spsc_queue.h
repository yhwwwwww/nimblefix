#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace fastfix::base {

template<typename T>
class SpscQueue
{
public:
  explicit SpscQueue(std::size_t capacity)
    : slots_((capacity == 0U ? 1U : capacity) + 1U)
    , capacity_(slots_.size())
  {
  }

  auto TryPush(const T& value) -> bool { return TryPushImpl(value); }

  auto TryPush(T&& value) -> bool { return TryPushImpl(std::move(value)); }

  template<typename... Args>
  auto Emplace(Args&&... args) -> bool
  {
    return TryPushImpl(T(std::forward<Args>(args)...));
  }

  auto TryPop() -> std::optional<T>
  {
    const auto tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) {
      return std::nullopt;
    }

    T value = std::move(slots_[tail]);
    tail_.store(Next(tail), std::memory_order_release);
    return value;
  }

private:
  template<typename U>
  auto TryPushImpl(U&& value) -> bool
  {
    const auto head = head_.load(std::memory_order_relaxed);
    const auto next = Next(head);
    if (next == tail_.load(std::memory_order_acquire)) {
      return false;
    }

    slots_[head] = std::forward<U>(value);
    head_.store(next, std::memory_order_release);
    return true;
  }

  [[nodiscard]] auto Next(std::size_t index) const -> std::size_t { return index + 1U == capacity_ ? 0U : index + 1U; }

  // slots_ and capacity_ are read by both producer and consumer but never
  // written after construction, so they do not need cache-line isolation.
  std::vector<T> slots_;
  std::size_t capacity_{ 0 };
  // head_ is written by the producer; put it on its own cache line to avoid
  // false sharing with tail_ (written by the consumer).
  alignas(64) std::atomic<std::size_t> head_{ 0 };
  alignas(64) std::atomic<std::size_t> tail_{ 0 };
};

} // namespace fastfix::base