#pragma once

#include <cstdint>

#include <nimblefix/status.hpp>

namespace nimblefix {

enum class IoBackendKind : std::uint8_t {
    epoll,      // Default Linux O(1) epoll reactor (zero external dependencies).
    busy_poll,  // Zero-sleep nonblocking poll loop (lowest single/low-session latency).
    io_uring,   // Optional Linux io_uring Proactor (async RECV/SEND + optional SQPOLL; requires liburing).
};

enum class IdleStrategy : std::uint8_t {
    sleep,          // Kernel sleep in epoll_wait / io_uring_wait_cqe when idle.
    busy_spin,      // Nonblocking zero-sleep spin with cpu_relax() pause.
    hybrid_backoff, // Spin for idle_spin_iterations, yield for idle_yield_iterations, then sleep.
};

struct IoBackendConfig {
    IoBackendKind kind{IoBackendKind::epoll};
    std::uint32_t uring_queue_depth{256};    // SQ/CQ ring depth when kind == io_uring.
    bool uring_sqpoll{false};                // Optional kernel SQPOLL thread (requires privilege or recent kernel).
    std::uint32_t uring_sqpoll_idle_ms{500}; // SQPOLL idle sleep threshold in milliseconds.
    IdleStrategy idle_strategy{IdleStrategy::sleep};
    std::uint32_t idle_spin_iterations{10'000};
    std::uint32_t idle_yield_iterations{1'000};

    [[nodiscard]] static constexpr IoBackendConfig epoll(
        IdleStrategy idle = IdleStrategy::sleep,
        std::uint32_t spin_iters = 10'000,
        std::uint32_t yield_iters = 1'000) noexcept {
        return {IoBackendKind::epoll, 256, false, 500, idle, spin_iters, yield_iters};
    }
    [[nodiscard]] static constexpr IoBackendConfig busy_poll() noexcept {
        return {IoBackendKind::busy_poll, 256, false, 500, IdleStrategy::busy_spin, 10'000, 1'000};
    }
    [[nodiscard]] static constexpr IoBackendConfig hybrid(
        std::uint32_t spin_iters = 10'000,
        std::uint32_t yield_iters = 1'000) noexcept {
        return {IoBackendKind::epoll, 256, false, 500, IdleStrategy::hybrid_backoff, spin_iters, yield_iters};
    }
    [[nodiscard]] static constexpr IoBackendConfig io_uring(
        std::uint32_t queue_depth = 256,
        bool sqpoll = false,
        std::uint32_t sqpoll_idle_ms = 500,
        IdleStrategy idle = IdleStrategy::sleep,
        std::uint32_t spin_iters = 10'000,
        std::uint32_t yield_iters = 1'000) noexcept {
        return {IoBackendKind::io_uring, queue_depth, sqpoll, sqpoll_idle_ms, idle, spin_iters, yield_iters};
    }
};

[[nodiscard]] bool io_uring_available() noexcept;

// Pin the calling thread to a specific logical CPU core (Linux pthread_setaffinity_np).
// A negative cpu_core is a no-op returning Status::success().
[[nodiscard]] Status pin_current_thread(int cpu_core) noexcept;

} // namespace nimblefix
