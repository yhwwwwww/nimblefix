#include "fastfix/runtime/shard_poller.h"

namespace fastfix::runtime {

auto ShardPoller::OpenWakeup() -> base::Status {
    return wakeup_.Open();
}

auto ShardPoller::CloseWakeup() -> void {
    wakeup_.Close();
}

auto ShardPoller::SignalWakeup() const -> void {
    wakeup_.Signal();
}

auto ShardPoller::DrainWakeup() -> void {
    wakeup_.Drain();
}

auto ShardPoller::CaptureReady(std::span<const pollfd> pollfds, PollState& state) const -> void {
    for (std::size_t index = 0; index < state.connection_ready.size(); ++index) {
        state.connection_ready[index] =
            (pollfds[state.connection_poll_offset + index].revents & POLLIN) != 0;
    }
}

}  // namespace fastfix::runtime