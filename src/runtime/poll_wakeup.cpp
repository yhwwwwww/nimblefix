#include "fastfix/runtime/poll_wakeup.h"

#include <array>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

namespace fastfix::runtime {

namespace {

constexpr std::size_t kWakeDrainBufferBytes = 64U;

auto ConfigureFd(int fd) -> base::Status {
    const auto flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return base::Status::IoError(std::string("fcntl(F_GETFL) failed: ") + std::strerror(errno));
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return base::Status::IoError(std::string("fcntl(F_SETFL) failed: ") + std::strerror(errno));
    }

    const auto fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags < 0) {
        return base::Status::IoError(std::string("fcntl(F_GETFD) failed: ") + std::strerror(errno));
    }
    if (fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0) {
        return base::Status::IoError(std::string("fcntl(F_SETFD) failed: ") + std::strerror(errno));
    }

    return base::Status::Ok();
}

}  // namespace

PollWakeup::PollWakeup(PollWakeup&& other) noexcept
    : read_fd_(other.read_fd_),
      write_fd_(other.write_fd_) {
    other.read_fd_ = -1;
    other.write_fd_ = -1;
}

auto PollWakeup::operator=(PollWakeup&& other) noexcept -> PollWakeup& {
    if (this == &other) {
        return *this;
    }

    Close();
    read_fd_ = other.read_fd_;
    write_fd_ = other.write_fd_;
    other.read_fd_ = -1;
    other.write_fd_ = -1;
    return *this;
}

PollWakeup::~PollWakeup() {
    Close();
}

auto PollWakeup::Open() -> base::Status {
    Close();

    int fds[2]{-1, -1};
    if (pipe(fds) != 0) {
        return base::Status::IoError(std::string("pipe failed: ") + std::strerror(errno));
    }

    auto status = ConfigureFd(fds[0]);
    if (!status.ok()) {
        close(fds[0]);
        close(fds[1]);
        return status;
    }
    status = ConfigureFd(fds[1]);
    if (!status.ok()) {
        close(fds[0]);
        close(fds[1]);
        return status;
    }

    read_fd_ = fds[0];
    write_fd_ = fds[1];
    return base::Status::Ok();
}

auto PollWakeup::Close() -> void {
    if (read_fd_ >= 0) {
        close(read_fd_);
        read_fd_ = -1;
    }
    if (write_fd_ >= 0) {
        close(write_fd_);
        write_fd_ = -1;
    }
}

auto PollWakeup::Signal() const -> void {
    if (!valid()) {
        return;
    }

    static constexpr unsigned char kWakeByte = 1U;
    while (true) {
        const auto rc = write(write_fd_, &kWakeByte, sizeof(kWakeByte));
        if (rc == static_cast<ssize_t>(sizeof(kWakeByte))) {
            return;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

auto PollWakeup::Drain() -> void {
    if (!valid()) {
        return;
    }

    std::array<unsigned char, kWakeDrainBufferBytes> buffer{};
    while (true) {
        const auto rc = read(read_fd_, buffer.data(), buffer.size());
        if (rc > 0) {
            continue;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

}  // namespace fastfix::runtime