#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <nimblefix/connection_strategy.hpp>
#include <nimblefix/status.hpp>

namespace nimblefix {

namespace detail {
[[nodiscard]] bool is_valid_bind_ip(std::string_view ip) noexcept;
} // namespace detail

struct TransportIoResult {
    std::size_t bytes{};
    bool would_block{};
    bool peer_closed{};
    Status status{};
};

template <class T>
concept DirectTransport = requires(T& t, const T& ct, std::string_view host, std::uint16_t port,
                                   std::span<char> rx, std::span<const char> tx) {
    { t.connect(host, port) } noexcept -> std::same_as<Status>;
    { t.finish_connect() } noexcept -> std::same_as<Status>;
    { t.read(rx) } noexcept -> std::same_as<TransportIoResult>;
    { t.write(tx) } noexcept -> std::same_as<TransportIoResult>;
    { t.close() } noexcept -> std::same_as<void>;
    { ct.native_fd() } noexcept -> std::same_as<int>;
    { ct.valid() } noexcept -> std::same_as<bool>;
    { ct.is_connecting() } noexcept -> std::same_as<bool>;
};

// Lightweight nonblocking TCP transport for Layer 0 DirectInitiator.
// Supports numeric IPv4/IPv6 hosts with TCP_NODELAY and MSG_NOSIGNAL.
class TcpTransport {
public:
    TcpTransport() noexcept = default;
    explicit TcpTransport(int fd, bool connecting = false) noexcept
        : fd_(fd), connecting_(connecting) {}
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;
    TcpTransport(TcpTransport&& other) noexcept;
    TcpTransport& operator=(TcpTransport&& other) noexcept;
    ~TcpTransport();

    void set_socket_options(SocketOptions options) noexcept { options_ = std::move(options); }
    [[nodiscard]] const SocketOptions& socket_options() const noexcept { return options_; }

    [[nodiscard]] Status connect(std::string_view host, std::uint16_t port) noexcept;
    [[nodiscard]] Status connect(std::string_view host, std::uint16_t port,
                                 const SocketOptions& options) noexcept;
    [[nodiscard]] Status finish_connect() noexcept;
    [[nodiscard]] TransportIoResult read(std::span<char> output) noexcept;
    [[nodiscard]] TransportIoResult write(std::span<const char> input) noexcept;
    [[nodiscard]] TransportIoResult write_batch(std::span<const std::span<const char>> chunks) noexcept;
    void close() noexcept;

    [[nodiscard]] int native_fd() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] bool is_connecting() const noexcept { return connecting_; }
    void rearm_quickack() const noexcept;

private:
    int fd_{-1};
    bool connecting_{};
    SocketOptions options_{};
};

static_assert(DirectTransport<TcpTransport>);

} // namespace nimblefix
