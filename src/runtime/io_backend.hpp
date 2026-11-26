#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <nimblefix/io_backend.hpp>
#include <nimblefix/status.hpp>

namespace nimblefix::detail {

inline constexpr std::uint32_t io_event_read  = 1U << 0U;
inline constexpr std::uint32_t io_event_write = 1U << 1U;
inline constexpr std::uint32_t io_event_error = 1U << 2U;
inline constexpr std::uint32_t io_event_hup   = 1U << 3U;

enum class IoCompletionKind : std::uint8_t {
    readiness, // Reactor readiness notification (epoll / busy_poll / io_uring POLL_ADD).
    recv_done, // Proactor async RECV completion: result >= 0 bytes (0 == EOF), < 0 negated errno.
    send_done, // Proactor async SEND completion: result >= 0 bytes written, < 0 negated errno.
};

struct alignas(8) IoTag {
    std::uint8_t kind{};
    void* pointer{};
    int fd{-1};
    std::uint32_t events{};
    std::uint32_t in_flight_polls{};
    bool poll_armed{};
};

struct IoCompletion {
    IoTag* tag{};
    IoCompletionKind kind{IoCompletionKind::readiness};
    std::uint32_t events{};
    std::int32_t result{};
};

// Pluggable I/O backend boundary separating FIX protocol/session logic from OS I/O multiplexing.
// Supports both Reactor backends (epoll, busy_poll) and Proactor backends (io_uring async RECV/SEND).
class IoBackend {
public:
    virtual ~IoBackend() = default;

    [[nodiscard]] virtual IoBackendKind kind() const noexcept = 0;
    [[nodiscard]] virtual bool supports_proactor() const noexcept = 0;

    virtual void sync_readiness(IoTag& tag, int fd, std::uint32_t desired_events) noexcept = 0;
    virtual void remove(IoTag& tag) noexcept = 0;

    [[nodiscard]] virtual bool submit_recv(IoTag& tag, int fd, std::span<char> buffer) noexcept {
        (void)tag; (void)fd; (void)buffer;
        return false;
    }
    [[nodiscard]] virtual bool submit_send(IoTag& tag, int fd, std::span<const char> buffer) noexcept {
        (void)tag; (void)fd; (void)buffer;
        return false;
    }
    virtual void cancel_fd(int fd) noexcept { (void)fd; }
    virtual void flush_submissions() noexcept {}

    // Submits queued SQEs (if any) and waits up to wait_ms for completions.
    // Returns completion count (>= 0), or -1 with errno set on error.
    [[nodiscard]] virtual int wait(std::span<IoCompletion> out, std::uint32_t wait_ms) noexcept = 0;
};

[[nodiscard]] Result<std::unique_ptr<IoBackend>> create_io_backend(
    const IoBackendConfig& config) noexcept;

using IoBackendTestFactory = Result<std::unique_ptr<IoBackend>> (*)(
    const IoBackendConfig& config) noexcept;
void set_io_backend_test_factory(IoBackendTestFactory factory) noexcept;

} // namespace nimblefix::detail
