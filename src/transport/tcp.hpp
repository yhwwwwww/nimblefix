#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <nimblefix/connection_strategy.hpp>
#include <nimblefix/status.hpp>

namespace nimblefix::detail {

struct BoundEndpoint {
    std::string host;
    std::uint16_t port{};
};

struct SocketIoResult {
    std::size_t bytes{};
    bool would_block{};
    bool peer_closed{};
    Status status{};
};

// Stage 3 accepts numeric IPv4/IPv6 hosts only, keeping address resolution out
// of the owner loop. An empty listener host means bind on all local interfaces.
[[nodiscard]] bool is_numeric_host(std::string_view host,
                                   bool allow_empty = false) noexcept;

class TcpStream {
public:
    TcpStream() noexcept = default;
    explicit TcpStream(int fd, bool quickack = false) noexcept
        : fd_(fd), quickack_(quickack) {}
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&& other) noexcept;
    ~TcpStream();

    [[nodiscard]] static Result<TcpStream> connect(std::string_view host,
                                                    std::uint16_t port,
                                                    bool& in_progress,
                                                    const SocketOptions& options = {}) noexcept;

    [[nodiscard]] SocketIoResult read(std::span<char> output) noexcept;
    [[nodiscard]] SocketIoResult write(std::span<const char> input) noexcept;
    [[nodiscard]] SocketIoResult write_batch(std::span<const std::span<const char>> chunks) noexcept;
    [[nodiscard]] Status finish_connect() noexcept;
    void rearm_quickack() const noexcept;
    [[nodiscard]] int native_handle() const noexcept { return fd_; }
    [[nodiscard]] int release_handle() noexcept {
        quickack_ = false;
        return std::exchange(fd_, -1);
    }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    void close() noexcept;

private:
    int fd_{-1};
    bool quickack_{false};
};

class TcpListener {
public:
    TcpListener() noexcept = default;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;
    ~TcpListener();

    [[nodiscard]] static Result<TcpListener> bind(std::string_view host,
                                                   std::uint16_t port,
                                                   int backlog = 128,
                                                   const SocketOptions& options = {}) noexcept;
    [[nodiscard]] Result<TcpStream> accept() noexcept;
    [[nodiscard]] Result<BoundEndpoint> local_endpoint() const noexcept;
    [[nodiscard]] int native_handle() const noexcept { return fd_; }
    [[nodiscard]] int release_handle() noexcept { return std::exchange(fd_, -1); }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    void close() noexcept;

private:
    explicit TcpListener(int fd, SocketOptions options = {}) noexcept
        : fd_(fd), options_(std::move(options)) {}
    int fd_{-1};
    SocketOptions options_{};
};

} // namespace nimblefix::detail
