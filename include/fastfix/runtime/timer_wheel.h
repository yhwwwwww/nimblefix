#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace fastfix::runtime {

inline constexpr std::uint64_t kDefaultTimerWheelTickNs = 1'000'000ULL;
inline constexpr std::size_t kDefaultTimerWheelSlotCount = 4096U;
inline constexpr std::uint64_t kMinimumTimerWheelTickNs = 1ULL;
inline constexpr std::size_t kMinimumTimerWheelSlotCount = 1U;

struct TimerWheelOptions
{
  std::uint64_t tick_ns{ kDefaultTimerWheelTickNs };
  std::size_t slot_count{ kDefaultTimerWheelSlotCount };
};

class TimerWheel
{
public:
  explicit TimerWheel(TimerWheelOptions options = {})
    : options_{
      .tick_ns = options.tick_ns == 0U ? kMinimumTimerWheelTickNs : options.tick_ns,
      .slot_count = options.slot_count == 0U ? kMinimumTimerWheelSlotCount : options.slot_count,
    }
  {
  }

  auto Clear() -> void
  {
    initialized_ = false;
    base_time_ns_ = 0U;
    current_time_ns_ = 0U;
    current_tick_ = 0U;
    heap_.clear();
    active_generation_.clear();
    next_generation_ = 0U;
    earliest_deadline_.reset();
  }

  auto Schedule(std::uint64_t timer_id, std::uint64_t deadline_ns, std::uint64_t now_ns) -> void
  {
    EnsureInitialized(now_ns);

    const auto gen = next_generation_++;
    active_generation_[timer_id] = gen;

    heap_.push_back(HeapEntry{ deadline_ns, timer_id, gen });
    std::push_heap(heap_.begin(), heap_.end(), HeapGreater{});

    if (!earliest_deadline_.has_value() || deadline_ns < *earliest_deadline_) {
      earliest_deadline_ = deadline_ns;
    }
  }

  auto Cancel(std::uint64_t timer_id) -> void
  {
    // Lazy deletion: remove from active map so heap entries are skipped on pop.
    active_generation_.erase(timer_id);
  }

  auto PopExpired(std::uint64_t now_ns, std::vector<std::uint64_t>* expired_timer_ids) -> void
  {
    if (expired_timer_ids == nullptr) {
      return;
    }

    EnsureInitialized(now_ns);

    while (!heap_.empty()) {
      const auto& top = heap_.front();
      if (top.deadline_ns > now_ns) {
        break;
      }

      auto entry = top;
      std::pop_heap(heap_.begin(), heap_.end(), HeapGreater{});
      heap_.pop_back();

      // Skip stale or cancelled entries.
      auto it = active_generation_.find(entry.timer_id);
      if (it == active_generation_.end() || it->second != entry.generation) {
        continue;
      }

      // This is the active entry; fire it and remove from active map.
      active_generation_.erase(it);
      expired_timer_ids->push_back(entry.timer_id);
    }

    // Purge any leading stale/cancelled entries from heap top.
    PurgeStaleTop();

    // Update earliest deadline.
    if (heap_.empty()) {
      earliest_deadline_.reset();
    } else {
      earliest_deadline_ = heap_.front().deadline_ns;
    }
  }

  [[nodiscard]] auto NextDeadline() const -> std::optional<std::uint64_t> { return earliest_deadline_; }

private:
  struct HeapEntry
  {
    std::uint64_t deadline_ns{ 0U };
    std::uint64_t timer_id{ 0U };
    std::uint64_t generation{ 0U };
  };

  struct HeapGreater
  {
    auto operator()(const HeapEntry& a, const HeapEntry& b) const -> bool { return a.deadline_ns > b.deadline_ns; }
  };

  auto EnsureInitialized(std::uint64_t now_ns) -> void
  {
    if (!initialized_) {
      initialized_ = true;
      base_time_ns_ = now_ns;
      current_tick_ = 0U;
    }
    current_time_ns_ = now_ns;
  }

  auto PurgeStaleTop() -> void
  {
    while (!heap_.empty()) {
      auto it = active_generation_.find(heap_.front().timer_id);
      if (it != active_generation_.end() && it->second == heap_.front().generation) {
        break;
      }
      std::pop_heap(heap_.begin(), heap_.end(), HeapGreater{});
      heap_.pop_back();
    }
  }

  TimerWheelOptions options_{};
  bool initialized_{ false };
  std::uint64_t base_time_ns_{ 0U };
  std::uint64_t current_time_ns_{ 0U };
  std::uint64_t current_tick_{ 0U };
  std::vector<HeapEntry> heap_;
  std::unordered_map<std::uint64_t, std::uint64_t> active_generation_;
  std::uint64_t next_generation_{ 0U };
  std::optional<std::uint64_t> earliest_deadline_;
};

} // namespace fastfix::runtime
