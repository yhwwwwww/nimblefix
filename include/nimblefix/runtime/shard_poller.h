#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "nimblefix/base/status.h"
#include "nimblefix/runtime/io_poller.h"
#include "nimblefix/runtime/poll_wakeup.h"
#include "nimblefix/runtime/timer_wheel.h"

namespace nimble::runtime {

class ShardPoller
{
public:
  struct IoReadyState
  {
    bool wakeup_signaled{ false };
    std::vector<std::size_t> ready_indices;
  };

  auto OpenWakeup() -> base::Status;
  auto CloseWakeup() -> void;
  auto SignalWakeup() const -> void;
  auto DrainWakeup() -> void;

  /// Initialize an IoPoller backend (epoll or io_uring).
  auto InitBackend(IoBackend backend) -> base::Status;

  /// Returns true if using IoPoller backend (epoll or io_uring).
  [[nodiscard]] auto has_io_poller() const -> bool { return io_poller_ != nullptr; }
  [[nodiscard]] auto io_poller() const -> IoPoller* { return io_poller_.get(); }

  auto ClearTimers() -> void { timer_wheel_.Clear(); }

  [[nodiscard]] auto NextDeadline() const -> std::optional<std::uint64_t> { return timer_wheel_.NextDeadline(); }

  [[nodiscard]] auto timer_wheel() -> TimerWheel& { return timer_wheel_; }

  [[nodiscard]] auto timer_wheel() const -> const TimerWheel& { return timer_wheel_; }

  template<typename ConnectionFdProvider>
  auto SyncAndWait(std::size_t connection_count,
                   ConnectionFdProvider&& connection_fd_provider,
                   std::chrono::milliseconds timeout,
                   IoReadyState& out) -> base::Status
  {
    out.wakeup_signaled = false;
    out.ready_indices.clear();

    // Build current fd list and fd->index mapping using a flat vector.
    current_fds_.clear();
    for (std::size_t i = 0; i < connection_count; ++i) {
      const int fd = connection_fd_provider(i);
      if (fd >= 0) {
        current_fds_.push_back(fd);
        const auto ufd = static_cast<std::size_t>(fd);
        if (ufd >= fd_to_index_.size()) {
          fd_to_index_.resize(ufd + 1U, kNoIndex);
        }
        fd_to_index_[ufd] = i;
      }
    }

    // Sort current_fds_ for efficient set-difference with registered_fds_.
    std::sort(current_fds_.begin(), current_fds_.end());

    // Remove stale fds.
    auto write_pos = std::size_t{ 0 };
    for (std::size_t r = 0; r < registered_fds_.size(); ++r) {
      const int fd = registered_fds_[r];
      if (std::binary_search(current_fds_.begin(), current_fds_.end(), fd)) {
        registered_fds_[write_pos++] = fd;
      } else {
        io_poller_->RemoveFd(fd);
      }
    }
    registered_fds_.resize(write_pos);

    // Add new fds. registered_fds_ is kept sorted.
    std::sort(registered_fds_.begin(), registered_fds_.end());
    for (const int fd : current_fds_) {
      if (!std::binary_search(registered_fds_.begin(), registered_fds_.end(), fd)) {
        auto status = io_poller_->AddFd(fd, static_cast<std::size_t>(fd));
        if (!status.ok())
          return status;
        registered_fds_.push_back(fd);
      }
    }
    std::sort(registered_fds_.begin(), registered_fds_.end());

    // Wait for events.
    auto result = io_poller_->Wait(timeout);
    if (!result.ok())
      return result.status();
    const int ready_count = result.value();

    // Drain wakeup if signaled and map ready tags to connection indices.
    for (int i = 0; i < ready_count; ++i) {
      const auto tag = io_poller_->ReadyTag(i);
      if (tag == kWakeupTag) {
        out.wakeup_signaled = true;
        continue;
      }
      const auto fd = static_cast<std::size_t>(tag);
      if (fd < fd_to_index_.size() && fd_to_index_[fd] != kNoIndex) {
        out.ready_indices.push_back(fd_to_index_[fd]);
      }
    }

    // Clear used fd_to_index_ entries (O(N) where N = current fds, not
    // capacity).
    for (const int fd : current_fds_) {
      fd_to_index_[static_cast<std::size_t>(fd)] = kNoIndex;
    }

    if (out.wakeup_signaled) {
      DrainWakeup();
    }

    return base::Status::Ok();
  }

private:
  static constexpr std::size_t kWakeupTag = ~std::size_t{ 0 };
  static constexpr std::size_t kNoIndex = ~std::size_t{ 0 };

  TimerWheel timer_wheel_{};
  PollWakeup wakeup_{};
  std::unique_ptr<IoPoller> io_poller_;
  std::vector<int> registered_fds_;      // sorted vector of registered fds
  std::vector<std::size_t> fd_to_index_; // flat array: fd -> connection index (sparse)
  std::vector<int> current_fds_;         // scratch: sorted vector of current fds
};

} // namespace nimble::runtime
