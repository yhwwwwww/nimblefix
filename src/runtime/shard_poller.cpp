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

auto ShardPoller::InitBackend(IoBackend backend) -> base::Status {
    if (backend == IoBackend::kPoll) {
        return base::Status::Ok();
    }

    io_poller_ = CreateIoPoller(backend);
    if (io_poller_ == nullptr) {
        return base::Status::InvalidArgument("unsupported io backend");
    }
    auto status = io_poller_->Init();
    if (!status.ok()) return status;

    // Register wakeup fd with sentinel tag.
    if (wakeup_.valid()) {
        status = io_poller_->AddFd(wakeup_.read_fd(), kWakeupTag);
        if (!status.ok()) return status;
    }
    return base::Status::Ok();
}

}  // namespace fastfix::runtime