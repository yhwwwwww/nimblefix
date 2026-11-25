#include "io_backend.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(NIMBLEFIX_ENABLE_IO_URING)
#include <liburing.h>
#endif

namespace nimblefix {

bool io_uring_available() noexcept {
#if defined(NIMBLEFIX_ENABLE_IO_URING)
    return true;
#else
    return false;
#endif
}

Status pin_current_thread(int cpu_core) noexcept {
    if (cpu_core < 0) return Status::success();
    if (cpu_core >= CPU_SETSIZE) return Status{ErrorCode::invalid_config};
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu_core, &set);
    const int rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
    if (rc != 0) {
        return Status{rc == EINVAL ? ErrorCode::invalid_config : ErrorCode::io_error, 0,
                      static_cast<std::size_t>(rc)};
    }
    return Status::success();
}

} // namespace nimblefix

namespace nimblefix::detail {
namespace {

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

[[nodiscard]] std::uint32_t to_epoll_events(std::uint32_t mask) noexcept {
    std::uint32_t ev = 0;
    if (mask & io_event_read) ev |= EPOLLIN;
    if (mask & io_event_write) ev |= EPOLLOUT;
    return ev;
}

[[nodiscard]] std::uint32_t from_epoll_events(std::uint32_t ev) noexcept {
    std::uint32_t mask = 0;
    if (ev & EPOLLIN) mask |= io_event_read;
    if (ev & EPOLLOUT) mask |= io_event_write;
    if (ev & EPOLLERR) mask |= io_event_error;
    if (ev & EPOLLHUP) mask |= io_event_hup;
    return mask;
}

class EpollBackend final : public IoBackend {
public:
    explicit EpollBackend(int fd, bool busy_poll, IdleStrategy idle_strategy,
                          std::uint32_t spin_iters, std::uint32_t yield_iters) noexcept
        : epoll_fd_(fd),
          busy_poll_(busy_poll),
          idle_strategy_(busy_poll ? IdleStrategy::busy_spin : idle_strategy),
          spin_iters_(spin_iters),
          yield_iters_(yield_iters) {}

    ~EpollBackend() override {
        if (epoll_fd_ >= 0) {
            ::close(epoll_fd_);
            epoll_fd_ = -1;
        }
    }

    [[nodiscard]] IoBackendKind kind() const noexcept override {
        return busy_poll_ ? IoBackendKind::busy_poll : IoBackendKind::epoll;
    }

    [[nodiscard]] bool supports_proactor() const noexcept override { return false; }

    void remove(IoTag& tag) noexcept override {
        if (epoll_fd_ >= 0 && tag.fd >= 0) {
            (void)::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, tag.fd, nullptr);
        }
        tag.fd = -1;
        tag.events = 0;
        tag.poll_armed = false;
    }

    void sync_readiness(IoTag& tag, int fd, std::uint32_t desired_events) noexcept override {
        if (epoll_fd_ < 0) return;
        if (fd < 0 || desired_events == 0) {
            remove(tag);
            return;
        }
        if (tag.fd == fd && tag.events == desired_events) return;
        epoll_event ev{};
        ev.events = to_epoll_events(desired_events);
        ev.data.ptr = &tag;
        if (tag.fd == fd) {
            if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0) {
                tag.events = desired_events;
                tag.poll_armed = true;
                return;
            }
        } else if (tag.fd >= 0) {
            (void)::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, tag.fd, nullptr);
            tag.fd = -1;
            tag.events = 0;
            tag.poll_armed = false;
        }
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == 0) {
            tag.fd = fd;
            tag.events = desired_events;
            tag.poll_armed = true;
        }
    }

    [[nodiscard]] int wait(std::span<IoCompletion> out, std::uint32_t wait_ms) noexcept override {
        if (epoll_fd_ < 0 || out.empty()) return 0;
        if (events_.size() < out.size()) {
            try {
                events_.resize(out.size());
            } catch (...) {
                errno = ENOMEM;
                return -1;
            }
        }
        int timeout = static_cast<int>(wait_ms);
        bool spinning = false;
        bool yielding = false;
        if (idle_strategy_ == IdleStrategy::busy_spin) {
            timeout = 0;
            spinning = true;
        } else if (idle_strategy_ == IdleStrategy::hybrid_backoff) {
            if (idle_count_ < spin_iters_) {
                timeout = 0;
                spinning = true;
            } else if (idle_count_ < spin_iters_ + yield_iters_) {
                timeout = 0;
                yielding = true;
            }
        }
        const int ready = ::epoll_wait(epoll_fd_, events_.data(), static_cast<int>(out.size()), timeout);
        if (ready <= 0) {
            if (ready == 0 && wait_ms > 0) {
                if (spinning) {
                    cpu_relax();
                    if (idle_strategy_ == IdleStrategy::hybrid_backoff) ++idle_count_;
                } else if (yielding) {
                    ::sched_yield();
                    ++idle_count_;
                }
            }
            return ready;
        }
        idle_count_ = 0;
        for (int i = 0; i < ready; ++i) {
            out[static_cast<std::size_t>(i)] = IoCompletion{
                static_cast<IoTag*>(events_[static_cast<std::size_t>(i)].data.ptr),
                IoCompletionKind::readiness,
                from_epoll_events(events_[static_cast<std::size_t>(i)].events),
                0,
            };
        }
        return ready;
    }

private:
    int epoll_fd_{-1};
    bool busy_poll_{false};
    IdleStrategy idle_strategy_{IdleStrategy::sleep};
    std::uint32_t spin_iters_{10'000};
    std::uint32_t yield_iters_{1'000};
    std::uint64_t idle_count_{0};
    std::vector<epoll_event> events_;
};

#if defined(NIMBLEFIX_ENABLE_IO_URING)

class IoUringBackend final : public IoBackend {
public:
    static constexpr std::uintptr_t op_mask = 0x3U;
    static constexpr std::uintptr_t op_poll = 0x0U;
    static constexpr std::uintptr_t op_recv = 0x1U;
    static constexpr std::uintptr_t op_send = 0x2U;
    static constexpr std::uintptr_t op_cancel = 0x3U;

    IoUringBackend() noexcept = default;
    ~IoUringBackend() override {
        if (initialized_) {
            io_uring_queue_exit(&ring_);
            initialized_ = false;
        }
    }

    [[nodiscard]] Status init(const IoBackendConfig& config) noexcept {
        const std::uint32_t depth = std::clamp<std::uint32_t>(config.uring_queue_depth, 32U, 4096U);
        io_uring_params params{};
        if (config.uring_sqpoll) {
            params.flags = IORING_SETUP_SQPOLL;
            params.sq_thread_idle = config.uring_sqpoll_idle_ms;
            const int rc = io_uring_queue_init_params(depth, &ring_, &params);
            if (rc < 0) return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(-rc)};
            initialized_ = true;
            return Status::success();
        }
        // Prefer low-latency single-issuer + deferred taskrun on Linux 6.1+.
        // Use IORING_SETUP_R_DISABLED so Engine::create() can run on one thread
        // and Engine::run() can bind the ring to its dedicated owner thread.
        params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_R_DISABLED;
        int rc = io_uring_queue_init_params(depth, &ring_, &params);
        if (rc == 0) {
            needs_enable_ = true;
            defer_taskrun_ = true;
            initialized_ = true;
            return Status::success();
        }
        std::memset(&params, 0, sizeof(params));
        params.flags = IORING_SETUP_COOP_TASKRUN;
        rc = io_uring_queue_init_params(depth, &ring_, &params);
        if (rc < 0) {
            rc = io_uring_queue_init(depth, &ring_, 0);
        }
        if (rc < 0) return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(-rc)};
        initialized_ = true;
        return Status::success();
    }

    [[nodiscard]] IoBackendKind kind() const noexcept override { return IoBackendKind::io_uring; }
    [[nodiscard]] bool supports_proactor() const noexcept override { return true; }

    void remove(IoTag& tag) noexcept override {
        if (initialized_ && tag.poll_armed) {
            if (auto* sqe = acquire_sqe()) {
                io_uring_prep_poll_remove(sqe, encode_data(&tag, op_poll));
                io_uring_sqe_set_data64(sqe, op_cancel);
                ++pending_sqes_;
            }
        }
        tag.fd = -1;
        tag.events = 0;
        tag.poll_armed = false;
    }

    void sync_readiness(IoTag& tag, int fd, std::uint32_t desired_events) noexcept override {
        if (!initialized_) return;
        if (fd < 0 || desired_events == 0) {
            remove(tag);
            return;
        }
        if (tag.poll_armed && tag.fd == fd && tag.events == desired_events) return;
        if (tag.poll_armed) {
            remove(tag);
        }
        auto* sqe = acquire_sqe();
        if (sqe == nullptr) return;
        unsigned poll_mask = 0;
        if (desired_events & io_event_read) poll_mask |= POLLIN;
        if (desired_events & io_event_write) poll_mask |= POLLOUT;
        io_uring_prep_poll_add(sqe, fd, poll_mask);
        io_uring_sqe_set_data64(sqe, encode_data(&tag, op_poll));
        ++pending_sqes_;
        tag.fd = fd;
        tag.events = desired_events;
        tag.poll_armed = true;
        ++tag.in_flight_polls;
    }

    [[nodiscard]] bool submit_recv(IoTag& tag, int fd, std::span<char> buffer) noexcept override {
        if (!initialized_ || fd < 0 || buffer.empty()) return false;
        auto* sqe = acquire_sqe();
        if (sqe == nullptr) return false;
        io_uring_prep_recv(sqe, fd, buffer.data(), buffer.size(), 0);
        io_uring_sqe_set_data64(sqe, encode_data(&tag, op_recv));
        ++pending_sqes_;
        return true;
    }

    [[nodiscard]] bool submit_send(IoTag& tag, int fd, std::span<const char> buffer) noexcept override {
        if (!initialized_ || fd < 0 || buffer.empty()) return false;
        auto* sqe = acquire_sqe();
        if (sqe == nullptr) return false;
        io_uring_prep_send(sqe, fd, buffer.data(), buffer.size(), MSG_NOSIGNAL);
        io_uring_sqe_set_data64(sqe, encode_data(&tag, op_send));
        ++pending_sqes_;
        return true;
    }

    void cancel_fd(int fd) noexcept override {
        if (!initialized_ || fd < 0) return;
        if (auto* sqe = acquire_sqe()) {
            io_uring_prep_cancel_fd(sqe, fd, IORING_ASYNC_CANCEL_ALL);
            io_uring_sqe_set_data64(sqe, op_cancel);
            ++pending_sqes_;
        }
    }

    void flush_submissions() noexcept override {
        if (!initialized_ || pending_sqes_ == 0) return;
        if (ensure_enabled() < 0) return;
        (void)io_uring_submit(&ring_);
        pending_sqes_ = 0;
    }

    [[nodiscard]] int wait(std::span<IoCompletion> out, std::uint32_t wait_ms) noexcept override {
        if (!initialized_ || out.empty()) return 0;
        if (const int en = ensure_enabled(); en < 0) {
            errno = -en;
            return -1;
        }

        if (wait_ms == 0) {
            if (pending_sqes_ != 0) {
                const int sub = defer_taskrun_
                    ? io_uring_submit_and_get_events(&ring_)
                    : io_uring_submit(&ring_);
                pending_sqes_ = 0;
                if (sub < 0 && sub != -EINTR) {
                    errno = -sub;
                    return -1;
                }
            } else if (defer_taskrun_ && io_uring_cq_ready(&ring_) == 0 &&
                       (IO_URING_READ_ONCE(*ring_.sq.kflags) & IORING_SQ_TASKRUN) != 0) {
                const int rc = io_uring_get_events(&ring_);
                if (rc < 0 && rc != -EINTR) {
                    errno = -rc;
                    return -1;
                }
            }
        } else {
            io_uring_cqe* first = nullptr;
            __kernel_timespec ts{};
            ts.tv_sec = static_cast<long long>(wait_ms / 1000U);
            ts.tv_nsec = static_cast<long long>((wait_ms % 1000U) * 1'000'000ULL);
            const int rc = io_uring_submit_and_wait_timeout(&ring_, &first, 1U, &ts, nullptr);
            pending_sqes_ = 0;
            if (rc < 0 && rc != -ETIME && rc != -EINTR) {
                errno = -rc;
                return -1;
            }
        }

        std::size_t count = 0;
        unsigned head = 0;
        unsigned consumed = 0;
        io_uring_cqe* cqe = nullptr;
        io_uring_for_each_cqe(&ring_, head, cqe) {
            if (count >= out.size()) break;
            ++consumed;
            const std::uint64_t raw = cqe->user_data;
            const auto op = static_cast<std::uintptr_t>(raw) & op_mask;
            if (op == op_cancel) continue;
            auto* tag = reinterpret_cast<IoTag*>(static_cast<std::uintptr_t>(raw) & ~op_mask);
            if (tag == nullptr) continue;

            if (op == op_poll) {
                if (tag->in_flight_polls != 0) --tag->in_flight_polls;
                if (cqe->res == -ECANCELED || !tag->poll_armed) {
                    continue;
                }
                tag->poll_armed = false;
                if (cqe->res < 0) {
                    out[count++] = IoCompletion{tag, IoCompletionKind::readiness, io_event_error, cqe->res};
                } else {
                    const auto revents = static_cast<unsigned>(cqe->res);
                    std::uint32_t ev = 0;
                    if (revents & POLLIN) ev |= io_event_read;
                    if (revents & POLLOUT) ev |= io_event_write;
                    if (revents & POLLERR) ev |= io_event_error;
                    if (revents & (POLLHUP | POLLRDHUP)) ev |= io_event_hup;
                    out[count++] = IoCompletion{tag, IoCompletionKind::readiness, ev, 0};
                }
            } else if (op == op_recv) {
                out[count++] = IoCompletion{tag, IoCompletionKind::recv_done, 0, cqe->res};
            } else if (op == op_send) {
                out[count++] = IoCompletion{tag, IoCompletionKind::send_done, 0, cqe->res};
            }
        }
        if (consumed != 0) io_uring_cq_advance(&ring_, consumed);
        return static_cast<int>(count);
    }

private:
    static std::uint64_t encode_data(IoTag* tag, std::uintptr_t op) noexcept {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(tag) | op);
    }

    [[nodiscard]] int ensure_enabled() noexcept {
        if (!needs_enable_) return 0;
        const int rc = io_uring_enable_rings(&ring_);
        if (rc == 0) needs_enable_ = false;
        return rc;
    }

    io_uring_sqe* acquire_sqe() noexcept {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (sqe != nullptr) return sqe;
        if (ensure_enabled() < 0) return nullptr;
        (void)io_uring_submit(&ring_);
        pending_sqes_ = 0;
        return io_uring_get_sqe(&ring_);
    }

    io_uring ring_{};
    bool initialized_{false};
    bool needs_enable_{false};
    bool defer_taskrun_{false};
    std::uint32_t pending_sqes_{0};
};

#endif // NIMBLEFIX_ENABLE_IO_URING

} // namespace

Result<std::unique_ptr<IoBackend>> create_io_backend(const IoBackendConfig& config) noexcept {
    try {
        switch (config.kind) {
        case IoBackendKind::epoll:
        case IoBackendKind::busy_poll: {
            const int fd = ::epoll_create1(EPOLL_CLOEXEC);
            if (fd < 0) return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(errno)};
            return std::unique_ptr<IoBackend>(
                new EpollBackend(fd, config.kind == IoBackendKind::busy_poll,
                                 config.idle_strategy,
                                 config.idle_spin_iterations,
                                 config.idle_yield_iterations));
        }
        case IoBackendKind::io_uring: {
#if defined(NIMBLEFIX_ENABLE_IO_URING)
            auto backend = std::make_unique<IoUringBackend>();
            if (const auto st = backend->init(config); !st.ok()) return st;
            return std::unique_ptr<IoBackend>(std::move(backend));
#else
            return Status{ErrorCode::unsupported};
#endif
        }
        }
        return Status{ErrorCode::invalid_config};
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded};
    }
}

} // namespace nimblefix::detail
