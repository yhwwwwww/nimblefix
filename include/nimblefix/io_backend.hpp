#pragma once

#include <cstdint>

namespace nimblefix {

enum class IoBackendKind : std::uint8_t {
    epoll,      // Default Linux O(1) epoll reactor (zero external dependencies).
    busy_poll,  // Zero-sleep nonblocking poll loop (lowest single/low-session latency).
    io_uring,   // Optional Linux io_uring Proactor (async RECV/SEND + optional SQPOLL; requires liburing).
};

struct IoBackendConfig {
    IoBackendKind kind{IoBackendKind::epoll};
    std::uint32_t uring_queue_depth{256};    // SQ/CQ ring depth when kind == io_uring.
    bool uring_sqpoll{false};                // Optional kernel SQPOLL thread (requires privilege or recent kernel).
    std::uint32_t uring_sqpoll_idle_ms{500}; // SQPOLL idle sleep threshold in milliseconds.

    [[nodiscard]] static constexpr IoBackendConfig epoll() noexcept {
        return {IoBackendKind::epoll};
    }
    [[nodiscard]] static constexpr IoBackendConfig busy_poll() noexcept {
        return {IoBackendKind::busy_poll};
    }
    [[nodiscard]] static constexpr IoBackendConfig io_uring(
        std::uint32_t queue_depth = 256,
        bool sqpoll = false,
        std::uint32_t sqpoll_idle_ms = 500) noexcept {
        return {IoBackendKind::io_uring, queue_depth, sqpoll, sqpoll_idle_ms};
    }
};

[[nodiscard]] bool io_uring_available() noexcept;

} // namespace nimblefix
