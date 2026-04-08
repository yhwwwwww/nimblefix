#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "fastfix/base/result.h"
#include "fastfix/base/status.h"

namespace fastfix::runtime {

enum class IoBackend : std::uint32_t {
    kPoll = 0,
    kEpoll = 1,
    kIoUring = 2,
};

/// Returns the best available backend for the running kernel.
auto DetectBestIoBackend() -> IoBackend;

/// Returns true if the given backend is supported on this system.
auto IsIoBackendAvailable(IoBackend backend) -> bool;

class IoPoller {
  public:
    virtual ~IoPoller() = default;

    /// Initialize the poller (create epoll fd, io_uring ring, etc.)
    virtual auto Init() -> base::Status = 0;

    /// Add a file descriptor to the interest set. `tag` is an opaque index
    /// returned in ready events so the caller can identify which fd fired.
    virtual auto AddFd(int fd, std::size_t tag) -> base::Status = 0;

    /// Remove a file descriptor from the interest set. No-op if not present.
    virtual auto RemoveFd(int fd) -> void = 0;

    /// Wait for readable events up to `timeout`. Returns number of ready fds.
    /// After return, call `ReadyTag(i)` for i in [0, return_value) to get tags.
    virtual auto Wait(std::chrono::milliseconds timeout) -> base::Result<int> = 0;

    /// After Wait() returns N>0, retrieve the tag for the i-th ready event.
    virtual auto ReadyTag(int index) const -> std::size_t = 0;

    /// Close and release resources.
    virtual auto Close() -> void = 0;
};

auto CreateIoPoller(IoBackend backend) -> std::unique_ptr<IoPoller>;

}  // namespace fastfix::runtime
