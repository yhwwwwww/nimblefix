#pragma once

#include "fastfix/base/status.h"

namespace fastfix::runtime {

class PollWakeup {
  public:
    PollWakeup() = default;
    PollWakeup(const PollWakeup&) = delete;
    auto operator=(const PollWakeup&) -> PollWakeup& = delete;
    PollWakeup(PollWakeup&& other) noexcept;
    auto operator=(PollWakeup&& other) noexcept -> PollWakeup&;
    ~PollWakeup();

    auto Open() -> base::Status;
    auto Close() -> void;
    auto Signal() const -> void;
    auto Drain() -> void;

    [[nodiscard]] auto read_fd() const -> int {
        return read_fd_;
    }

    [[nodiscard]] auto valid() const -> bool {
        return read_fd_ >= 0 && write_fd_ >= 0;
    }

  private:
    int read_fd_{-1};
    int write_fd_{-1};
};

}  // namespace fastfix::runtime