#pragma once

#include "nimblefix/base/status.h"

namespace nimble::runtime {

class PollWakeup
{
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

  [[nodiscard]] auto read_fd() const -> int { return efd_; }

  [[nodiscard]] auto valid() const -> bool { return efd_ >= 0; }

private:
  int efd_{ -1 };
};

} // namespace nimble::runtime
