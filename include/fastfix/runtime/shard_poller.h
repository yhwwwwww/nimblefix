#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <poll.h>

#include "fastfix/base/status.h"
#include "fastfix/runtime/io_poller.h"
#include "fastfix/runtime/poll_wakeup.h"
#include "fastfix/runtime/timer_wheel.h"

namespace fastfix::runtime {

class ShardPoller {
  public:
    struct PollState {
        std::size_t connection_poll_offset{0U};
        std::vector<bool> connection_ready;
    };

    struct IoReadyState {
        bool wakeup_signaled{false};
        std::vector<std::size_t> ready_indices;
    };

    auto OpenWakeup() -> base::Status;
    auto CloseWakeup() -> void;
    auto SignalWakeup() const -> void;
    auto DrainWakeup() -> void;

    /// Initialize an IoPoller backend. If backend is kPoll, no IoPoller is created.
    auto InitBackend(IoBackend backend) -> base::Status;

    /// Returns true if using IoPoller backend (epoll or io_uring).
    [[nodiscard]] auto has_io_poller() const -> bool { return io_poller_ != nullptr; }

    auto ClearTimers() -> void {
        timer_wheel_.Clear();
    }

    [[nodiscard]] auto NextDeadline() const -> std::optional<std::uint64_t> {
        return timer_wheel_.NextDeadline();
    }

    [[nodiscard]] auto timer_wheel() -> TimerWheel& {
        return timer_wheel_;
    }

    [[nodiscard]] auto timer_wheel() const -> const TimerWheel& {
        return timer_wheel_;
    }

    template <typename ConnectionFdProvider>
    auto PreparePoll(
        std::size_t connection_count,
        ConnectionFdProvider&& connection_fd_provider,
        std::vector<pollfd>& pollfds,
        PollState& state) const -> void {
        state.connection_poll_offset = pollfds.size();
        if (wakeup_.valid()) {
            pollfds.push_back(pollfd{.fd = wakeup_.read_fd(), .events = POLLIN, .revents = 0});
            ++state.connection_poll_offset;
        }

        state.connection_ready.assign(connection_count, false);
        for (std::size_t index = 0; index < connection_count; ++index) {
            pollfds.push_back(
                pollfd{.fd = connection_fd_provider(index), .events = POLLIN, .revents = 0});
        }
    }

    auto CaptureReady(std::span<const pollfd> pollfds, PollState& state) const -> void;

    template <typename ConnectionProcessor>
    auto ProcessReadyConnections(
        const PollState& state,
        std::size_t current_connection_count,
        ConnectionProcessor&& process_connection) const -> base::Status {
        const auto existing_connection_count = state.connection_ready.size();
        for (std::size_t index = existing_connection_count; index > 0; --index) {
            const auto connection_index = index - 1U;
            if (connection_index >= current_connection_count) {
                continue;
            }

            auto status = process_connection(connection_index, state.connection_ready[connection_index]);
            if (!status.ok()) {
                return status;
            }
        }

        return base::Status::Ok();
    }

    template <typename ConnectionFdProvider>
    auto SyncAndWait(
        std::size_t connection_count,
        ConnectionFdProvider&& connection_fd_provider,
        std::chrono::milliseconds timeout,
        IoReadyState& out) -> base::Status {
        out.wakeup_signaled = false;
        out.ready_indices.clear();

        // Build current fd set and fd→index map.
        std::unordered_map<int, std::size_t> fd_to_index;
        fd_to_index.reserve(connection_count);
        std::unordered_set<int> current_fds;
        current_fds.reserve(connection_count);
        for (std::size_t i = 0; i < connection_count; ++i) {
            const int fd = connection_fd_provider(i);
            if (fd >= 0) {
                current_fds.insert(fd);
                fd_to_index[fd] = i;
            }
        }

        // Remove stale fds.
        for (auto it = registered_fds_.begin(); it != registered_fds_.end(); ) {
            if (!current_fds.contains(*it)) {
                io_poller_->RemoveFd(*it);
                it = registered_fds_.erase(it);
            } else {
                ++it;
            }
        }

        // Add new fds (use fd value as tag).
        for (const int fd : current_fds) {
            if (!registered_fds_.contains(fd)) {
                auto status = io_poller_->AddFd(fd, static_cast<std::size_t>(fd));
                if (!status.ok()) return status;
                registered_fds_.insert(fd);
            }
        }

        // Wait for events.
        auto result = io_poller_->Wait(timeout);
        if (!result.ok()) return result.status();
        const int ready_count = result.value();

        // Drain wakeup if signaled and map ready tags to connection indices.
        for (int i = 0; i < ready_count; ++i) {
            const auto tag = io_poller_->ReadyTag(i);
            if (tag == kWakeupTag) {
                out.wakeup_signaled = true;
                continue;
            }
            const auto fd = static_cast<int>(tag);
            auto it = fd_to_index.find(fd);
            if (it != fd_to_index.end()) {
                out.ready_indices.push_back(it->second);
            }
        }

        if (out.wakeup_signaled) {
            DrainWakeup();
        }

        return base::Status::Ok();
    }

  private:
    static constexpr std::size_t kWakeupTag = ~std::size_t{0};

    TimerWheel timer_wheel_{};
    PollWakeup wakeup_{};
    std::unique_ptr<IoPoller> io_poller_;
    std::unordered_set<int> registered_fds_;
};

}  // namespace fastfix::runtime