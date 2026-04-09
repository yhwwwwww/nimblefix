#include "fastfix/runtime/io_poller.h"

#include <cerrno>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <poll.h>
#include <sys/epoll.h>
#include <unistd.h>

#if !defined(FASTFIX_DISABLE_LIBURING) && __has_include(<liburing.h>)
#define FASTFIX_HAS_LIBURING 1
#include <liburing.h>
#endif

namespace fastfix::runtime {

// ---------------------------------------------------------------------------
// PollPoller
// ---------------------------------------------------------------------------

class PollPoller final : public IoPoller {
  public:
    ~PollPoller() override { Close(); }

    auto Init() -> base::Status override { return base::Status::Ok(); }

    auto AddFd(int fd, std::size_t tag) -> base::Status override {
        pollfds_.push_back(pollfd{.fd = fd, .events = POLLIN, .revents = 0});
        tags_.push_back(tag);
        return base::Status::Ok();
    }

    auto RemoveFd(int fd) -> void override {
        for (std::size_t i = 0; i < pollfds_.size(); ++i) {
            if (pollfds_[i].fd == fd) {
                pollfds_.erase(pollfds_.begin() + static_cast<std::ptrdiff_t>(i));
                tags_.erase(tags_.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

    auto Wait(std::chrono::milliseconds timeout) -> base::Result<int> override {
        for (auto& pfd : pollfds_) { pfd.revents = 0; }
        const int timeout_ms = timeout.count() < 0 ? -1 : static_cast<int>(timeout.count());
        const int rc = ::poll(pollfds_.data(), pollfds_.size(), timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) return 0;
            return base::Status::IoError(std::string("poll failed: ") + std::strerror(errno));
        }
        ready_tags_.clear();
        for (std::size_t i = 0; i < pollfds_.size(); ++i) {
            if (pollfds_[i].revents & (POLLIN | POLLERR | POLLHUP)) {
                ready_tags_.push_back(tags_[i]);
            }
        }
        return static_cast<int>(ready_tags_.size());
    }

    auto ReadyTag(int index) const -> std::size_t override {
        return ready_tags_[static_cast<std::size_t>(index)];
    }

    auto Close() -> void override {
        pollfds_.clear();
        tags_.clear();
        ready_tags_.clear();
    }

  private:
    std::vector<pollfd> pollfds_;
    std::vector<std::size_t> tags_;
    std::vector<std::size_t> ready_tags_;
};

// ---------------------------------------------------------------------------
// EpollPoller
// ---------------------------------------------------------------------------

class EpollPoller final : public IoPoller {
  public:
    ~EpollPoller() override { Close(); }

    auto Init() -> base::Status override {
        epfd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epfd_ < 0) {
            return base::Status::IoError(
                std::string("epoll_create1 failed: ") + std::strerror(errno));
        }
        events_.resize(64);
        return base::Status::Ok();
    }

    auto AddFd(int fd, std::size_t tag) -> base::Status override {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.u64 = static_cast<std::uint64_t>(tag);
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
            return base::Status::IoError(
                std::string("epoll_ctl ADD failed: ") + std::strerror(errno));
        }
        ++fd_count_;
        if (fd_count_ > events_.size()) {
            events_.resize(fd_count_ * 2);
        }
        return base::Status::Ok();
    }

    auto RemoveFd(int fd) -> void override {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
        if (fd_count_ > 0) --fd_count_;
    }

    auto Wait(std::chrono::milliseconds timeout) -> base::Result<int> override {
        const int timeout_ms = timeout.count() < 0 ? -1 : static_cast<int>(timeout.count());
        const int n = epoll_wait(
            epfd_, events_.data(), static_cast<int>(events_.size()), timeout_ms);
        if (n < 0) {
            if (errno == EINTR) return 0;
            return base::Status::IoError(
                std::string("epoll_wait failed: ") + std::strerror(errno));
        }
        ready_count_ = n;
        return n;
    }

    auto ReadyTag(int index) const -> std::size_t override {
        return static_cast<std::size_t>(events_[static_cast<std::size_t>(index)].data.u64);
    }

    auto Close() -> void override {
        if (epfd_ >= 0) {
            ::close(epfd_);
            epfd_ = -1;
        }
        fd_count_ = 0;
        ready_count_ = 0;
    }

  private:
    int epfd_{-1};
    std::size_t fd_count_{0};
    int ready_count_{0};
    std::vector<epoll_event> events_;
};

// ---------------------------------------------------------------------------
// IoUringPoller
// ---------------------------------------------------------------------------

#ifdef FASTFIX_HAS_LIBURING
class IoUringPoller final : public IoPoller {
  public:
    ~IoUringPoller() override { Close(); }

    auto Init() -> base::Status override {
        int ret = io_uring_queue_init(kRingSize, &ring_, 0);
        if (ret < 0) {
            return base::Status::IoError(
                std::string("io_uring_queue_init failed: ") + std::strerror(-ret));
        }
        initialized_ = true;
        return base::Status::Ok();
    }

    auto AddFd(int fd, std::size_t tag) -> base::Status override {
        auto* sqe = io_uring_get_sqe(&ring_);
        if (sqe == nullptr) {
            return base::Status::IoError("io_uring SQE ring full");
        }
        io_uring_prep_poll_multishot(sqe, fd, POLLIN);
        io_uring_sqe_set_data64(sqe, static_cast<__u64>(tag));
        fd_tags_[fd] = tag;
        ++pending_submits_;
        return base::Status::Ok();
    }

    auto RemoveFd(int fd) -> void override {
        auto it = fd_tags_.find(fd);
        if (it == fd_tags_.end()) return;

        auto* sqe = io_uring_get_sqe(&ring_);
        if (sqe != nullptr) {
            io_uring_prep_poll_remove(sqe, static_cast<__u64>(it->second));
            io_uring_sqe_set_data64(sqe, kRemoveTag);
            ++pending_submits_;
        }
        fd_tags_.erase(it);
    }

    auto Wait(std::chrono::milliseconds timeout) -> base::Result<int> override {
        // Submit any pending SQEs (AddFd / RemoveFd)
        if (pending_submits_ > 0) {
            io_uring_submit(&ring_);
            pending_submits_ = 0;
        }

        // Wait for at least one CQE
        struct __kernel_timespec ts {};
        ts.tv_sec = timeout.count() / 1000;
        ts.tv_nsec = (timeout.count() % 1000) * 1000000;

        struct io_uring_cqe* cqe = nullptr;
        int ret;
        if (timeout.count() < 0) {
            ret = io_uring_wait_cqe(&ring_, &cqe);
        } else {
            ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
        }

        if (ret == -ETIME || ret == -EINTR) {
            return 0;
        }
        if (ret < 0) {
            return base::Status::IoError(
                std::string("io_uring_wait_cqe failed: ") + std::strerror(-ret));
        }

        // Harvest all available CQEs
        ready_tags_.clear();
        unsigned head;
        unsigned count = 0;
        io_uring_for_each_cqe(&ring_, head, cqe) {
            ++count;
            const auto tag = static_cast<std::size_t>(io_uring_cqe_get_data64(cqe));
            if (tag == kRemoveTag) continue;  // removal completion
            if (cqe->res < 0) continue;       // error
            if (cqe->res & POLLIN) {
                ready_tags_.push_back(tag);
            }
            // If multishot was cancelled, resubmit
            if (!(cqe->flags & IORING_CQE_F_MORE)) {
                for (const auto& [fd, t] : fd_tags_) {
                    if (t == tag) {
                        auto* sqe = io_uring_get_sqe(&ring_);
                        if (sqe) {
                            io_uring_prep_poll_multishot(sqe, fd, POLLIN);
                            io_uring_sqe_set_data64(sqe, static_cast<__u64>(tag));
                            ++pending_submits_;
                        }
                        break;
                    }
                }
            }
        }
        io_uring_cq_advance(&ring_, count);

        return static_cast<int>(ready_tags_.size());
    }

    auto ReadyTag(int index) const -> std::size_t override {
        return ready_tags_[static_cast<std::size_t>(index)];
    }

    auto Close() -> void override {
        if (initialized_) {
            io_uring_queue_exit(&ring_);
            initialized_ = false;
        }
        fd_tags_.clear();
        ready_tags_.clear();
        pending_submits_ = 0;
    }

  private:
    static constexpr unsigned kRingSize = 256;
    static constexpr std::size_t kRemoveTag = ~std::size_t{0};

    io_uring ring_{};
    bool initialized_{false};
    int pending_submits_{0};
    std::unordered_map<int, std::size_t> fd_tags_;
    std::vector<std::size_t> ready_tags_;
};
#endif  // FASTFIX_HAS_LIBURING

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

auto DetectBestIoBackend() -> IoBackend {
#ifdef FASTFIX_HAS_LIBURING
    if (IsIoBackendAvailable(IoBackend::kIoUring)) {
        return IoBackend::kIoUring;
    }
#endif
    return IoBackend::kEpoll;
}

auto IsIoBackendAvailable(IoBackend backend) -> bool {
    switch (backend) {
        case IoBackend::kPoll:  return true;
        case IoBackend::kEpoll: return true;
        case IoBackend::kIoUring: {
#ifdef FASTFIX_HAS_LIBURING
            io_uring ring{};
            int ret = io_uring_queue_init(1, &ring, 0);
            if (ret < 0) return false;
            io_uring_queue_exit(&ring);
            return true;
#else
            return false;
#endif
        }
    }
    return false;
}

auto CreateIoPoller(IoBackend backend) -> std::unique_ptr<IoPoller> {
    switch (backend) {
#ifdef FASTFIX_HAS_LIBURING
        case IoBackend::kIoUring:
            return std::make_unique<IoUringPoller>();
#else
        case IoBackend::kIoUring:
            return std::make_unique<EpollPoller>();  // fallback: no liburing
#endif
        case IoBackend::kEpoll:
            return std::make_unique<EpollPoller>();
        case IoBackend::kPoll:
        default:
            return std::make_unique<PollPoller>();
    }
}

}  // namespace fastfix::runtime
