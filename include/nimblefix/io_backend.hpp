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
};

[[nodiscard]] bool io_uring_available() noexcept;

} // namespace nimblefix
