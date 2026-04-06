#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <poll.h>

#include "fastfix/base/status.h"
#include "fastfix/runtime/poll_wakeup.h"
#include "fastfix/runtime/timer_wheel.h"

namespace fastfix::runtime {

class ShardPoller {
  public:
    struct PollState {
        std::size_t connection_poll_offset{0U};
        std::vector<bool> connection_ready;
    };

    auto OpenWakeup() -> base::Status;
    auto CloseWakeup() -> void;
    auto SignalWakeup() const -> void;
    auto DrainWakeup() -> void;

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

  private:
    TimerWheel timer_wheel_{};
    PollWakeup wakeup_{};
};

}  // namespace fastfix::runtime