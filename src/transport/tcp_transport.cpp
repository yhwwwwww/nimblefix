#include "fastfix/transport/tcp_transport.h"

#include "fastfix/codec/simd_scan.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <charconv>
#include <cstring>
#include <limits>

namespace fastfix::transport {

namespace {

constexpr std::size_t kDefaultReadBufferCapacity = 4096U;
constexpr std::size_t kDefaultFrameBufferCapacity = 1024U;
constexpr std::size_t kMinimumFrameProbeBytes = 12U;
constexpr std::size_t kBodyLengthFieldPrefixSize = 3U;
constexpr std::size_t kChecksumFieldWireSize = 7U;
constexpr char kFixBeginStringTag = '8';
constexpr char kFixBodyLengthTag = '9';
constexpr char kFixTagValueSeparator = '=';
constexpr char kFixFieldDelimiter = '\x01';
constexpr int kSocketOptionEnabled = 1;

auto SetNonBlocking(int fd) -> base::Status {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return base::Status::IoError(std::string("fcntl(F_GETFL) failed: ") + std::strerror(errno));
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return base::Status::IoError(std::string("fcntl(F_SETFL) failed: ") + std::strerror(errno));
    }
    return base::Status::Ok();
}

auto SetNoDelay(int fd) -> base::Status {
    int enabled = kSocketOptionEnabled;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0) {
        return base::Status::IoError(std::string("setsockopt(TCP_NODELAY) failed: ") + std::strerror(errno));
    }
    return base::Status::Ok();
}

auto TrySetBusyPoll(int fd) -> void {
    int busy_poll_us = 50;
    // EPERM-tolerant: unprivileged processes can't set SO_BUSY_POLL
    setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_us, sizeof(busy_poll_us));
}

auto TrySetQuickAck(int fd) -> void {
    int enabled = 1;
    // TCP_QUICKACK disables delayed ACKs; the kernel may reset it after
    // each recv, so this is only an initial hint.
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &enabled, sizeof(enabled));
}

auto SetReuseAddr(int fd) -> base::Status {
    int enabled = kSocketOptionEnabled;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0) {
        return base::Status::IoError(std::string("setsockopt(SO_REUSEADDR) failed: ") + std::strerror(errno));
    }
    return base::Status::Ok();
}

auto PollFd(int fd, short events, std::chrono::milliseconds timeout) -> base::Status {
    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = events;
    const auto timeout_ms = timeout.count() > std::numeric_limits<int>::max()
                                ? std::numeric_limits<int>::max()
                                : static_cast<int>(timeout.count());
    const int rc = poll(&descriptor, 1, timeout_ms);
    if (rc == 0) {
        return base::Status::IoError("socket operation timed out");
    }
    if (rc < 0) {
        return base::Status::IoError(std::string("poll failed: ") + std::strerror(errno));
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return base::Status::IoError("socket closed or errored while polling");
    }
    if ((descriptor.revents & events) == 0) {
        return base::Status::IoError("socket did not become ready for the requested event");
    }
    return base::Status::Ok();
}

auto ResolveAddress(const std::string& host, std::uint16_t port, int flags) -> base::Result<addrinfo*> {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = flags;

    addrinfo* results = nullptr;
    const auto port_string = std::to_string(port);
    const int rc = getaddrinfo(host.empty() ? nullptr : host.c_str(), port_string.c_str(), &hints, &results);
    if (rc != 0) {
        return base::Status::IoError(std::string("getaddrinfo failed: ") + gai_strerror(rc));
    }
    return results;
}

auto ReleaseAddress(addrinfo* info) -> void {
    if (info != nullptr) {
        freeaddrinfo(info);
    }
}

auto ExtractAssignedPort(int fd) -> base::Result<std::uint16_t> {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return base::Status::IoError(std::string("getsockname failed: ") + std::strerror(errno));
    }
    return ntohs(address.sin_port);
}

template <typename Sink>
auto TryExtractFrame(std::span<const std::byte> buffer, Sink&& sink) -> base::Result<std::size_t> {
    if (buffer.size() < kMinimumFrameProbeBytes) {
        return std::size_t{0};
    }
    if (static_cast<char>(buffer[0]) != kFixBeginStringTag ||
        static_cast<char>(buffer[1]) != kFixTagValueSeparator) {
        return base::Status::FormatError("FIX frame must begin with tag 8");
    }

    constexpr auto kDelim = std::byte{'\x01'};
    const auto* first_ptr = codec::FindByte(buffer.data(), buffer.size(), kDelim);
    const auto first_delimiter = static_cast<std::size_t>(first_ptr - buffer.data());
    if (first_delimiter >= buffer.size()) {
        return std::size_t{0};
    }
    if (first_delimiter + kBodyLengthFieldPrefixSize >= buffer.size()) {
        return std::size_t{0};
    }
    if (static_cast<char>(buffer[first_delimiter + 1U]) != kFixBodyLengthTag ||
        static_cast<char>(buffer[first_delimiter + 2U]) != kFixTagValueSeparator) {
        return base::Status::FormatError("FIX frame must place BodyLength immediately after BeginString");
    }

    const auto* scan_start = buffer.data() + first_delimiter + kBodyLengthFieldPrefixSize;
    const auto scan_len = buffer.size() - first_delimiter - kBodyLengthFieldPrefixSize;
    const auto* second_ptr = codec::FindByte(scan_start, scan_len, kDelim);
    const auto second_delimiter = static_cast<std::size_t>(second_ptr - buffer.data());
    if (second_delimiter >= buffer.size()) {
        return std::size_t{0};
    }

    std::uint32_t body_length = 0;
    const auto* begin = reinterpret_cast<const char*>(buffer.data() + first_delimiter + kBodyLengthFieldPrefixSize);
    const auto* end = reinterpret_cast<const char*>(buffer.data() + second_delimiter);
    const auto [ptr, ec] = std::from_chars(begin, end, body_length);
    if (ec != std::errc() || ptr != end) {
        return base::Status::FormatError("invalid BodyLength field in frame header");
    }

    const std::size_t total_size = (second_delimiter + 1U) + body_length + kChecksumFieldWireSize;
    if (buffer.size() < total_size) {
        return std::size_t{0};
    }

    sink(buffer.subspan(0, total_size));
    return total_size;
}

}  // namespace

TcpConnection::TcpConnection() {
    read_buffer_.reserve(kDefaultReadBufferCapacity);
    frame_buffer_.reserve(kDefaultFrameBufferCapacity);
}

TcpConnection::TcpConnection(int fd)
    : fd_(fd) {
    read_buffer_.reserve(kDefaultReadBufferCapacity);
    frame_buffer_.reserve(kDefaultFrameBufferCapacity);
}

TcpConnection::~TcpConnection() {
    Close();
}

TcpConnection::TcpConnection(TcpConnection&& other) noexcept {
    Swap(other);
}

auto TcpConnection::operator=(TcpConnection&& other) noexcept -> TcpConnection& {
    if (this != &other) {
        Close();
        Swap(other);
    }
    return *this;
}

auto TcpConnection::Swap(TcpConnection& other) noexcept -> void {
    std::swap(fd_, other.fd_);
    std::swap(read_buffer_, other.read_buffer_);
    std::swap(frame_buffer_, other.frame_buffer_);
    std::swap(read_cursor_, other.read_cursor_);
}

auto TcpConnection::Connect(
    const std::string& host,
    std::uint16_t port,
    std::chrono::milliseconds timeout) -> base::Result<TcpConnection> {
    auto addresses = ResolveAddress(host, port, 0);
    if (!addresses.ok()) {
        return addresses.status();
    }

    addrinfo* results = addresses.value();
    for (auto* address = results; address != nullptr; address = address->ai_next) {
        const int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            continue;
        }

        auto status = SetNonBlocking(fd);
        if (!status.ok()) {
            close(fd);
            ReleaseAddress(results);
            return status;
        }

        const int rc = connect(fd, address->ai_addr, address->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS) {
            close(fd);
            continue;
        }

        status = PollFd(fd, POLLOUT, timeout);
        if (!status.ok()) {
            close(fd);
            continue;
        }

        int socket_error = 0;
        socklen_t socket_error_length = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_length) != 0 || socket_error != 0) {
            close(fd);
            continue;
        }

        status = SetNoDelay(fd);
        if (!status.ok()) {
            close(fd);
            ReleaseAddress(results);
            return status;
        }
        TrySetBusyPoll(fd);
        TrySetQuickAck(fd);

        ReleaseAddress(results);
        return TcpConnection(fd);
    }

    ReleaseAddress(results);
    return base::Status::IoError("could not connect to TCP endpoint");
}

auto TcpConnection::Send(std::span<const std::byte> bytes, std::chrono::milliseconds timeout) -> base::Status {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        auto status = PollFd(fd_, POLLOUT, timeout);
        if (!status.ok()) {
            return status;
        }

        const auto rc = send(fd_, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return base::Status::IoError(std::string("send failed: ") + std::strerror(errno));
        }
        if (rc == 0) {
            return base::Status::IoError("peer closed the socket during send");
        }

        sent += static_cast<std::size_t>(rc);
    }
    return base::Status::Ok();
}

auto TcpConnection::Send(const std::vector<std::byte>& bytes, std::chrono::milliseconds timeout) -> base::Status {
    return Send(std::span<const std::byte>(bytes.data(), bytes.size()), timeout);
}

auto TcpConnection::BusySend(std::span<const std::byte> bytes, std::chrono::milliseconds timeout) -> base::Status {
    std::size_t sent = 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (sent < bytes.size()) {
        const auto rc = send(fd_, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    return base::Status::IoError("timed out while busy-sending");
                }
                continue;
            }
            return base::Status::IoError(std::string("send failed: ") + std::strerror(errno));
        }
        if (rc == 0) {
            return base::Status::IoError("peer closed the socket during send");
        }
        sent += static_cast<std::size_t>(rc);
    }
    return base::Status::Ok();
}

auto TcpConnection::TryReceiveFrameView() -> base::Result<std::optional<std::span<const std::byte>>> {
    while (true) {
        auto unconsumed = std::span<const std::byte>(
            read_buffer_.data() + read_cursor_, read_buffer_.size() - read_cursor_);
        auto consumed = TryExtractFrame(unconsumed, [&](std::span<const std::byte> frame) {
            frame_buffer_.assign(frame.begin(), frame.end());
        });
        if (consumed.ok() && consumed.value() > 0) {
            read_cursor_ += consumed.value();
            if (read_cursor_ == read_buffer_.size()) {
                read_buffer_.clear();
                read_cursor_ = 0;
            } else if (read_cursor_ > read_buffer_.capacity() / 2) {
                read_buffer_.erase(read_buffer_.begin(),
                    read_buffer_.begin() + static_cast<std::ptrdiff_t>(read_cursor_));
                read_cursor_ = 0;
            }
            return std::optional<std::span<const std::byte>>(
                std::span<const std::byte>(frame_buffer_.data(), frame_buffer_.size()));
        }
        if (consumed.status().code() == base::ErrorCode::kFormatError) {
            return consumed.status();
        }

        std::byte buffer[kDefaultReadBufferCapacity];
        const auto rc = recv(fd_, buffer, sizeof(buffer), 0);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return std::optional<std::span<const std::byte>>{};
            }
            return base::Status::IoError(std::string("recv failed: ") + std::strerror(errno));
        }
        if (rc == 0) {
            return base::Status::IoError("peer closed the socket");
        }

        read_buffer_.insert(read_buffer_.end(), buffer, buffer + rc);

        if (read_buffer_.size() > kMaxReadBufferSize) {
            Close();
            return base::Status::IoError("TCP read buffer exceeded maximum size limit");
        }
    }
}

auto TcpConnection::ReceiveFrameView(std::chrono::milliseconds timeout) -> base::Result<std::span<const std::byte>> {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        auto unconsumed = std::span<const std::byte>(
            read_buffer_.data() + read_cursor_, read_buffer_.size() - read_cursor_);
        auto consumed = TryExtractFrame(unconsumed, [&](std::span<const std::byte> frame) {
            frame_buffer_.assign(frame.begin(), frame.end());
        });
        if (consumed.ok() && consumed.value() > 0) {
            read_cursor_ += consumed.value();
            if (read_cursor_ == read_buffer_.size()) {
                read_buffer_.clear();
                read_cursor_ = 0;
            } else if (read_cursor_ > read_buffer_.capacity() / 2) {
                read_buffer_.erase(read_buffer_.begin(),
                    read_buffer_.begin() + static_cast<std::ptrdiff_t>(read_cursor_));
                read_cursor_ = 0;
            }
            return std::span<const std::byte>(frame_buffer_.data(), frame_buffer_.size());
        }
        if (consumed.status().code() == base::ErrorCode::kFormatError) {
            return consumed.status();
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return base::Status::IoError("timed out while waiting for a FIX frame");
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);

        auto status = PollFd(fd_, POLLIN, remaining);
        if (!status.ok()) {
            return status;
        }

        std::byte buffer[kDefaultReadBufferCapacity];
        const auto rc = recv(fd_, buffer, sizeof(buffer), 0);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            return base::Status::IoError(std::string("recv failed: ") + std::strerror(errno));
        }
        if (rc == 0) {
            return base::Status::IoError("peer closed the socket");
        }

        read_buffer_.insert(read_buffer_.end(), buffer, buffer + rc);

        if (read_buffer_.size() > kMaxReadBufferSize) {
            Close();
            return base::Status::IoError("TCP read buffer exceeded maximum size limit");
        }
    }
}

auto TcpConnection::BusyReceiveFrameView(std::chrono::milliseconds timeout) -> base::Result<std::span<const std::byte>> {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        auto unconsumed = std::span<const std::byte>(
            read_buffer_.data() + read_cursor_, read_buffer_.size() - read_cursor_);
        auto consumed = TryExtractFrame(unconsumed, [&](std::span<const std::byte> frame) {
            frame_buffer_.assign(frame.begin(), frame.end());
        });
        if (consumed.ok() && consumed.value() > 0) {
            read_cursor_ += consumed.value();
            if (read_cursor_ == read_buffer_.size()) {
                read_buffer_.clear();
                read_cursor_ = 0;
            } else if (read_cursor_ > read_buffer_.capacity() / 2) {
                read_buffer_.erase(read_buffer_.begin(),
                    read_buffer_.begin() + static_cast<std::ptrdiff_t>(read_cursor_));
                read_cursor_ = 0;
            }
            return std::span<const std::byte>(frame_buffer_.data(), frame_buffer_.size());
        }
        if (consumed.status().code() == base::ErrorCode::kFormatError) {
            return consumed.status();
        }

        std::byte buffer[kDefaultReadBufferCapacity];
        const auto rc = recv(fd_, buffer, sizeof(buffer), 0);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    return base::Status::IoError("timed out while busy-waiting for a FIX frame");
                }
                continue;
            }
            return base::Status::IoError(std::string("recv failed: ") + std::strerror(errno));
        }
        if (rc == 0) {
            return base::Status::IoError("peer closed the socket");
        }

        read_buffer_.insert(read_buffer_.end(), buffer, buffer + rc);

        if (read_buffer_.size() > kMaxReadBufferSize) {
            Close();
            return base::Status::IoError("TCP read buffer exceeded maximum size limit");
        }
    }
}

auto TcpConnection::TryReceiveFrame() -> base::Result<std::optional<std::vector<std::byte>>> {
    auto frame = TryReceiveFrameView();
    if (!frame.ok()) {
        return frame.status();
    }
    if (!frame.value().has_value()) {
        return std::optional<std::vector<std::byte>>{};
    }
    return std::optional<std::vector<std::byte>>(
        std::vector<std::byte>(frame.value().value().begin(), frame.value().value().end()));
}

auto TcpConnection::ReceiveFrame(std::chrono::milliseconds timeout) -> base::Result<std::vector<std::byte>> {
    auto frame = ReceiveFrameView(timeout);
    if (!frame.ok()) {
        return frame.status();
    }
    return std::vector<std::byte>(frame.value().begin(), frame.value().end());
}

auto TcpConnection::Close() -> void {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    read_buffer_.clear();
    read_cursor_ = 0;
}

TcpAcceptor::TcpAcceptor(int fd, std::uint16_t port)
    : fd_(fd),
      port_(port) {
}

TcpAcceptor::~TcpAcceptor() {
    Close();
}

TcpAcceptor::TcpAcceptor(TcpAcceptor&& other) noexcept {
    Swap(other);
}

auto TcpAcceptor::operator=(TcpAcceptor&& other) noexcept -> TcpAcceptor& {
    if (this != &other) {
        Close();
        Swap(other);
    }
    return *this;
}

auto TcpAcceptor::Swap(TcpAcceptor& other) noexcept -> void {
    std::swap(fd_, other.fd_);
    std::swap(port_, other.port_);
}

auto TcpAcceptor::Listen(const std::string& host, std::uint16_t port, int backlog) -> base::Result<TcpAcceptor> {
    auto addresses = ResolveAddress(host, port, AI_PASSIVE);
    if (!addresses.ok()) {
        return addresses.status();
    }

    addrinfo* results = addresses.value();
    for (auto* address = results; address != nullptr; address = address->ai_next) {
        const int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            continue;
        }

        auto status = SetReuseAddr(fd);
        if (!status.ok()) {
            close(fd);
            ReleaseAddress(results);
            return status;
        }
        status = SetNonBlocking(fd);
        if (!status.ok()) {
            close(fd);
            ReleaseAddress(results);
            return status;
        }

        if (bind(fd, address->ai_addr, address->ai_addrlen) != 0) {
            close(fd);
            continue;
        }
        if (listen(fd, backlog) != 0) {
            close(fd);
            continue;
        }

        auto assigned_port = ExtractAssignedPort(fd);
        if (!assigned_port.ok()) {
            close(fd);
            ReleaseAddress(results);
            return assigned_port.status();
        }

        ReleaseAddress(results);
        return TcpAcceptor(fd, assigned_port.value());
    }

    ReleaseAddress(results);
    return base::Status::IoError("could not bind TCP acceptor");
}

auto TcpAcceptor::Accept(std::chrono::milliseconds timeout) -> base::Result<TcpConnection> {
    auto status = PollFd(fd_, POLLIN, timeout);
    if (!status.ok()) {
        return status;
    }

    auto connection = TryAccept();
    if (!connection.ok()) {
        return connection.status();
    }
    if (!connection.value().has_value()) {
        return base::Status::IoError("acceptor became readable without a pending TCP connection");
    }
    return std::move(*connection.value());
}

auto TcpAcceptor::TryAccept() -> base::Result<std::optional<TcpConnection>> {
    const int connection_fd = accept(fd_, nullptr, nullptr);
    if (connection_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::optional<TcpConnection>{};
        }
        return base::Status::IoError(std::string("accept failed: ") + std::strerror(errno));
    }

    auto status = SetNonBlocking(connection_fd);
    if (!status.ok()) {
        close(connection_fd);
        return status;
    }
    status = SetNoDelay(connection_fd);
    if (!status.ok()) {
        close(connection_fd);
        return status;
    }
    TrySetBusyPoll(connection_fd);
    TrySetQuickAck(connection_fd);

    return std::optional<TcpConnection>(TcpConnection(connection_fd));
}

auto TcpAcceptor::Close() -> void {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    port_ = 0;
}

}  // namespace fastfix::transport