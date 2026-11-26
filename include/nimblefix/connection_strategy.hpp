#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nimblefix/status.hpp>

namespace nimblefix {

struct TcpEndpoint {
    // Numeric IPv4/IPv6 addresses. An empty listener host binds all interfaces;
    // initiator hosts must be nonempty numeric addresses.
    std::string host;
    std::uint16_t port{};
};

// Optional low-latency TCP socket tuning and local interface/source binding.
// All fields default to OS defaults (zero overhead when unused).
struct SocketOptions {
    // Optional numeric IPv4/IPv6 local source address to bind before connect()
    // (for multi-NIC hosts routing exchange traffic over dedicated lines).
    std::string local_bind_ip{};
    // Optional local source port to bind before connect() (0 = OS ephemeral port).
    std::uint16_t local_bind_port{};
    // Optional Linux SO_BUSY_POLL timeout in microseconds (0 = disabled).
    // Best-effort: ignored gracefully if EPERM/ENOPROTOOPT in unprivileged containers.
    std::uint32_t so_busy_poll_us{};
    // Enable TCP_QUICKACK on connected/accepted sockets (best-effort on Linux).
    bool tcp_quickack{false};
    // Optional SO_SNDBUF / SO_RCVBUF sizes in bytes (0 = OS default).
    std::size_t send_buffer_bytes{};
    std::size_t recv_buffer_bytes{};
};

enum class FailoverPolicy : std::uint8_t {
    ordered,      // Primary-first: try endpoints[0], [1], ... in order; optionally reset to primary after an active session.
    round_robin,  // Advance cyclically across endpoints on every connect failure or disconnect.
};

struct ConnectionStrategy {
    FailoverPolicy policy{FailoverPolicy::ordered};
    std::uint32_t initial_backoff_ms{0};       // 0 = use engine reconnect_delay_ms (or 0 in DirectInitiator).
    std::uint32_t max_backoff_ms{30'000};      // Backoff cap in ms (0 = uncapped beyond initial_backoff_ms).
    std::uint32_t backoff_multiplier{1};       // >= 1 (1 = constant backoff, 2 = exponential doubling).
    std::uint32_t max_retries{0};              // 0 = unlimited; >0 = stop after N consecutive failed attempts.
    bool reset_to_primary_on_success{true};    // For ordered policy: return to endpoints[0] after an active session disconnects.
    bool immediate_failover{true};             // Fail over to the next standby endpoint with 0 ms delay before wrapping around.

    [[nodiscard]] constexpr Status validate() const noexcept {
        if (backoff_multiplier == 0) return Status{ErrorCode::invalid_config};
        if (max_backoff_ms != 0 && initial_backoff_ms > max_backoff_ms)
            return Status{ErrorCode::invalid_config};
        return Status::success();
    }

    [[nodiscard]] static constexpr ConnectionStrategy ordered(
        std::uint32_t initial_ms = 250,
        std::uint32_t max_ms = 30'000,
        std::uint32_t multiplier = 2,
        std::uint32_t retries = 0,
        bool reset_primary = true,
        bool immediate = true) noexcept {
        return ConnectionStrategy{
            FailoverPolicy::ordered, initial_ms, max_ms, multiplier, retries, reset_primary, immediate};
    }

    [[nodiscard]] static constexpr ConnectionStrategy round_robin(
        std::uint32_t initial_ms = 250,
        std::uint32_t max_ms = 30'000,
        std::uint32_t multiplier = 2,
        std::uint32_t retries = 0,
        bool immediate = true) noexcept {
        return ConnectionStrategy{
            FailoverPolicy::round_robin, initial_ms, max_ms, multiplier, retries, false, immediate};
    }
};

namespace detail {

// Deterministic, zero-hot-path-allocation multi-endpoint failover & backoff controller
// shared between Layer 0 DirectInitiator and Layer 1/2 Engine initiators.
class FailoverController {
public:
    void configure(std::vector<TcpEndpoint> endpoints,
                   ConnectionStrategy strategy,
                   std::uint32_t default_delay_ms) noexcept {
        endpoints_ = std::move(endpoints);
        strategy_ = strategy;
        default_delay_ms_ = default_delay_ms;
        current_index_ = 0;
        consecutive_failures_ = 0;
        cycle_failures_ = 0;
        reached_active_ = false;
        exhausted_ = false;
        next_connect_at_ms_ = 0;
    }

    [[nodiscard]] bool empty() const noexcept { return endpoints_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return endpoints_.size(); }
    [[nodiscard]] std::size_t current_index() const noexcept { return current_index_; }
    [[nodiscard]] const TcpEndpoint& current_endpoint() const noexcept { return endpoints_[current_index_]; }
    [[nodiscard]] const std::vector<TcpEndpoint>& endpoints() const noexcept { return endpoints_; }
    [[nodiscard]] std::uint32_t consecutive_failures() const noexcept { return consecutive_failures_; }
    [[nodiscard]] bool exhausted() const noexcept { return exhausted_; }
    [[nodiscard]] std::uint64_t next_connect_at_ms() const noexcept { return next_connect_at_ms_; }
    [[nodiscard]] const ConnectionStrategy& strategy() const noexcept { return strategy_; }

    void on_session_active() noexcept {
        reached_active_ = true;
        consecutive_failures_ = 0;
        cycle_failures_ = 0;
        exhausted_ = false;
    }

    // Record a connect failure, timeout, or session disconnect; advances endpoint and computes
    // the next reconnect timestamp. Returns scheduled delay in milliseconds.
    std::uint32_t on_failure_or_disconnect(std::uint64_t now_ms, bool was_active_session) noexcept {
        if (endpoints_.empty()) {
            next_connect_at_ms_ = now_ms + default_delay_ms_;
            return default_delay_ms_;
        }
        const bool had_active = was_active_session || reached_active_;
        reached_active_ = false;

        if (had_active) {
            consecutive_failures_ = 0;
            cycle_failures_ = 0;
        } else {
            ++consecutive_failures_;
            ++cycle_failures_;
            if (strategy_.max_retries != 0 && consecutive_failures_ >= strategy_.max_retries) {
                exhausted_ = true;
                next_connect_at_ms_ = now_ms;
                return 0;
            }
        }

        bool wrapped = false;
        if (endpoints_.size() > 1) {
            if (strategy_.policy == FailoverPolicy::ordered) {
                if (had_active && strategy_.reset_to_primary_on_success) {
                    current_index_ = 0;
                    wrapped = true;
                } else {
                    current_index_ = (current_index_ + 1) % endpoints_.size();
                    wrapped = (current_index_ == 0);
                }
            } else {
                current_index_ = (current_index_ + 1) % endpoints_.size();
                wrapped = (cycle_failures_ >= endpoints_.size());
                if (wrapped) cycle_failures_ = 0;
            }
        } else {
            wrapped = true;
        }

        if (!had_active && endpoints_.size() > 1 && strategy_.immediate_failover && !wrapped) {
            next_connect_at_ms_ = now_ms;
            return 0;
        }

        const std::uint32_t base = strategy_.initial_backoff_ms != 0
            ? strategy_.initial_backoff_ms : default_delay_ms_;
        std::uint64_t delay = base;
        const std::uint32_t completed_cycles = had_active
            ? 0U
            : (consecutive_failures_ > 0
                ? (consecutive_failures_ - 1U) / static_cast<std::uint32_t>(endpoints_.size())
                : 0U);
        if (strategy_.backoff_multiplier > 1 && delay > 0) {
            const std::uint64_t cap = strategy_.max_backoff_ms != 0
                ? strategy_.max_backoff_ms : delay;
            for (std::uint32_t i = 0; i < completed_cycles && delay < cap; ++i) {
                if (delay > cap / strategy_.backoff_multiplier) {
                    delay = cap;
                    break;
                }
                delay *= strategy_.backoff_multiplier;
            }
        }
        if (strategy_.max_backoff_ms != 0 && delay > strategy_.max_backoff_ms) {
            delay = strategy_.max_backoff_ms;
        }
        next_connect_at_ms_ = now_ms + delay;
        return static_cast<std::uint32_t>(delay);
    }

    void reset_failures() noexcept {
        consecutive_failures_ = 0;
        cycle_failures_ = 0;
        exhausted_ = false;
        next_connect_at_ms_ = 0;
    }

private:
    std::vector<TcpEndpoint> endpoints_{};
    ConnectionStrategy strategy_{};
    std::uint32_t default_delay_ms_{250};
    std::size_t current_index_{};
    std::uint32_t consecutive_failures_{};
    std::uint32_t cycle_failures_{};
    bool reached_active_{};
    bool exhausted_{};
    std::uint64_t next_connect_at_ms_{};
};

} // namespace detail

} // namespace nimblefix
