#include "nimblefix/runtime/poll_wakeup.h"

#include <cerrno>
#include <cstdint>
#include <cstring>

#include <sys/eventfd.h>
#include <unistd.h>

namespace nimble::runtime {

PollWakeup::PollWakeup(PollWakeup&& other) noexcept
  : efd_(other.efd_)
{
  other.efd_ = -1;
}

auto
PollWakeup::operator=(PollWakeup&& other) noexcept -> PollWakeup&
{
  if (this == &other) {
    return *this;
  }

  Close();
  efd_ = other.efd_;
  other.efd_ = -1;
  return *this;
}

PollWakeup::~PollWakeup()
{
  Close();
}

auto
PollWakeup::Open() -> base::Status
{
  Close();

  const int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (fd < 0) {
    return base::Status::IoError(std::string("eventfd failed: ") + std::strerror(errno));
  }

  efd_ = fd;
  return base::Status::Ok();
}

auto
PollWakeup::Close() -> void
{
  if (efd_ >= 0) {
    close(efd_);
    efd_ = -1;
  }
}

auto
PollWakeup::Signal() const -> void
{
  if (!valid()) {
    return;
  }

  std::uint64_t val = 1;
  while (true) {
    const auto rc = write(efd_, &val, sizeof(val));
    if (rc == static_cast<ssize_t>(sizeof(val))) {
      return;
    }
    if (rc < 0 && errno == EINTR) {
      continue;
    }
    return;
  }
}

auto
PollWakeup::Drain() -> void
{
  if (!valid()) {
    return;
  }

  std::uint64_t val{};
  while (true) {
    const auto rc = read(efd_, &val, sizeof(val));
    if (rc == static_cast<ssize_t>(sizeof(val))) {
      return;
    }
    if (rc < 0 && errno == EINTR) {
      continue;
    }
    return;
  }
}

} // namespace nimble::runtime
