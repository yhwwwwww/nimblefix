#include "tcp.hpp"
#include <nimblefix/tcp_transport.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <limits>
#include <string>
#include <utility>

namespace nimblefix::detail {
namespace {

[[nodiscard]] Status socket_error() noexcept {
    return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(errno)};
}

bool set_nodelay(int fd) noexcept {
    const int enabled = 1;
    return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0;
}

void apply_socket_options(int fd, const SocketOptions& options) noexcept {
    if (options.send_buffer_bytes != 0) {
        const int sz = static_cast<int>(std::min<std::size_t>(
            options.send_buffer_bytes, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
    }
    if (options.recv_buffer_bytes != 0) {
        const int sz = static_cast<int>(std::min<std::size_t>(
            options.recv_buffer_bytes, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
    }
#ifdef TCP_QUICKACK
    if (options.tcp_quickack) {
        const int enabled = 1;
        (void)::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &enabled, sizeof(enabled));
    }
#endif
#ifdef SO_BUSY_POLL
    if (options.so_busy_poll_us != 0) {
        const int usec = static_cast<int>(std::min<std::uint32_t>(
            options.so_busy_poll_us, static_cast<std::uint32_t>(std::numeric_limits<int>::max())));
        (void)::setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &usec, sizeof(usec));
    }
#endif
}

[[nodiscard]] bool bind_local_source(int fd, int family, const SocketOptions& options,
                                     int& out_errno) noexcept {
    if (options.local_bind_ip.empty() && options.local_bind_port == 0) return true;
    if (options.local_bind_port != 0) {
        const int one = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
    if (family == AF_INET) {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(options.local_bind_port);
        if (options.local_bind_ip.empty()) {
            local.sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (::inet_pton(AF_INET, options.local_bind_ip.c_str(), &local.sin_addr) != 1) {
            out_errno = EINVAL;
            return false;
        }
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            out_errno = errno;
            return false;
        }
        return true;
    }
    if (family == AF_INET6) {
        sockaddr_in6 local{};
        local.sin6_family = AF_INET6;
        local.sin6_port = htons(options.local_bind_port);
        if (options.local_bind_ip.empty()) {
            local.sin6_addr = in6addr_any;
        } else if (::inet_pton(AF_INET6, options.local_bind_ip.c_str(), &local.sin6_addr) != 1) {
            out_errno = EINVAL;
            return false;
        }
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            out_errno = errno;
            return false;
        }
        return true;
    }
    out_errno = EAFNOSUPPORT;
    return false;
}

} // namespace

bool is_numeric_host(std::string_view host, bool allow_empty) noexcept {
    if (host.empty()) return allow_empty;
    if (host.size() >= NI_MAXHOST || host.find('\0') != std::string_view::npos) return false;
    std::array<char, NI_MAXHOST> text{};
    std::memcpy(text.data(), host.data(), host.size());
    in_addr ipv4{};
    in6_addr ipv6{};
    return ::inet_pton(AF_INET, text.data(), &ipv4) == 1 ||
        ::inet_pton(AF_INET6, text.data(), &ipv6) == 1;
}

TcpStream::TcpStream(TcpStream&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

TcpStream& TcpStream::operator=(TcpStream&& other) noexcept {
    if (this == &other) return *this;
    close();
    fd_ = std::exchange(other.fd_, -1);
    return *this;
}

TcpStream::~TcpStream() {
    close();
}

Result<TcpStream> TcpStream::connect(std::string_view host, std::uint16_t port,
                                    bool& in_progress,
                                    const SocketOptions& options) noexcept {
    in_progress = false;
    if (!is_numeric_host(host) || port == 0) return Status{ErrorCode::invalid_value};
    if (!options.local_bind_ip.empty() && !is_numeric_host(options.local_bind_ip))
        return Status{ErrorCode::invalid_value};
    try {
        const std::string host_string{host};
        const auto service = std::to_string(port);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_NUMERICHOST;
        addrinfo* addresses = nullptr;
        const int resolved = ::getaddrinfo(host_string.c_str(), service.c_str(), &hints, &addresses);
        if (resolved != 0) return Status{ErrorCode::invalid_value};

        int last_error = EHOSTUNREACH;
        for (auto* address = addresses; address != nullptr; address = address->ai_next) {
            const int fd = ::socket(address->ai_family,
                address->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, address->ai_protocol);
            if (fd < 0) {
                last_error = errno;
                continue;
            }
            if (!set_nodelay(fd)) {
                last_error = errno;
                (void)::close(fd);
                continue;
            }
            apply_socket_options(fd, options);
            if (!bind_local_source(fd, address->ai_family, options, last_error)) {
                (void)::close(fd);
                continue;
            }
            const int connected = ::connect(fd, address->ai_addr, address->ai_addrlen);
            if (connected == 0) {
                ::freeaddrinfo(addresses);
                return TcpStream{fd};
            }
            if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
                in_progress = true;
                ::freeaddrinfo(addresses);
                return TcpStream{fd};
            }
            last_error = errno;
            (void)::close(fd);
        }
        ::freeaddrinfo(addresses);
        errno = last_error;
        return socket_error();
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded};
    }
}

SocketIoResult TcpStream::read(std::span<char> output) noexcept {
    if (fd_ < 0) return {0, false, true, Status{ErrorCode::invalid_value}};
    if (output.empty()) return {};
    ssize_t count;
    do {
        count = ::recv(fd_, output.data(), output.size(), 0);
    } while (count < 0 && errno == EINTR);
    if (count > 0) return {static_cast<std::size_t>(count), false, false, Status::success()};
    if (count == 0) return {0, false, true, Status::success()};
    if (errno == EAGAIN || errno == EWOULDBLOCK) return {0, true, false, Status::success()};
    return {0, false, false, socket_error()};
}

SocketIoResult TcpStream::write(std::span<const char> input) noexcept {
    if (fd_ < 0) return {0, false, true, Status{ErrorCode::invalid_value}};
    if (input.empty()) return {};
    ssize_t count;
    do {
        count = ::send(fd_, input.data(), input.size(), MSG_NOSIGNAL);
    } while (count < 0 && errno == EINTR);
    if (count > 0) return {static_cast<std::size_t>(count), false, false, Status::success()};
    if (count == 0) return {0, false, true, Status::success()};
    if (errno == EAGAIN || errno == EWOULDBLOCK) return {0, true, false, Status::success()};
    return {0, false, false, socket_error()};
}

SocketIoResult TcpStream::write_batch(std::span<const std::span<const char>> chunks) noexcept {
    if (fd_ < 0) return {0, false, true, Status{ErrorCode::invalid_value}};
    if (chunks.empty()) return {};
    if (chunks.size() == 1) return write(chunks[0]);
    std::array<iovec, 64> iov{};
    std::size_t count_iov = 0;
    for (const auto& chunk : chunks) {
        if (chunk.empty()) continue;
        if (count_iov >= iov.size()) break;
        iov[count_iov].iov_base = const_cast<char*>(chunk.data());
        iov[count_iov].iov_len = chunk.size();
        ++count_iov;
    }
    if (count_iov == 0) return {};
    msghdr hdr{};
    hdr.msg_iov = iov.data();
    hdr.msg_iovlen = count_iov;
    ssize_t count;
    do {
        count = ::sendmsg(fd_, &hdr, MSG_NOSIGNAL);
    } while (count < 0 && errno == EINTR);
    if (count > 0) return {static_cast<std::size_t>(count), false, false, Status::success()};
    if (count == 0) return {0, false, true, Status::success()};
    if (errno == EAGAIN || errno == EWOULDBLOCK) return {0, true, false, Status::success()};
    return {0, false, false, socket_error()};
}

Status TcpStream::finish_connect() noexcept {
    if (fd_ < 0) return Status{ErrorCode::invalid_value};
    int error = 0;
    socklen_t size = sizeof(error);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &size) != 0) return socket_error();
    if (error == 0) return Status::success();
    errno = error;
    return socket_error();
}

void TcpStream::close() noexcept {
    if (fd_ < 0) return;
    (void)::close(fd_);
    fd_ = -1;
}

TcpListener::TcpListener(TcpListener&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), options_(std::move(other.options_)) {}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this == &other) return *this;
    close();
    fd_ = std::exchange(other.fd_, -1);
    options_ = std::move(other.options_);
    return *this;
}

TcpListener::~TcpListener() {
    close();
}

Result<TcpListener> TcpListener::bind(std::string_view host, std::uint16_t port,
                                      int backlog,
                                      const SocketOptions& options) noexcept {
    if (backlog <= 0 || !is_numeric_host(host, true)) return Status{ErrorCode::invalid_value};
    try {
        const std::string host_string{host};
        const auto service = std::to_string(port);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_PASSIVE;
        if (!host.empty()) hints.ai_flags |= AI_NUMERICHOST;
        addrinfo* addresses = nullptr;
        const char* host_ptr = host_string.empty() ? nullptr : host_string.c_str();
        const int resolved = ::getaddrinfo(host_ptr, service.c_str(), &hints, &addresses);
        if (resolved != 0) return Status{ErrorCode::invalid_value};

        int last_error = EADDRNOTAVAIL;
        for (auto* address = addresses; address != nullptr; address = address->ai_next) {
            const int fd = ::socket(address->ai_family,
                address->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, address->ai_protocol);
            if (fd < 0) {
                last_error = errno;
                continue;
            }
            const int enabled = 1;
            (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
            apply_socket_options(fd, options);
            if (::bind(fd, address->ai_addr, address->ai_addrlen) == 0 &&
                ::listen(fd, backlog) == 0) {
                ::freeaddrinfo(addresses);
                return TcpListener{fd, options};
            }
            last_error = errno;
            (void)::close(fd);
        }
        ::freeaddrinfo(addresses);
        errno = last_error;
        return socket_error();
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded};
    }
}

Result<TcpStream> TcpListener::accept() noexcept {
    if (fd_ < 0) return Status{ErrorCode::invalid_value};
    sockaddr_storage address{};
    socklen_t size = sizeof(address);
    int fd;
    do {
        fd = ::accept4(fd_, reinterpret_cast<sockaddr*>(&address), &size,
            SOCK_NONBLOCK | SOCK_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd >= 0) {
        if (!set_nodelay(fd)) {
            const int saved_errno = errno;
            (void)::close(fd);
            errno = saved_errno;
            return socket_error();
        }
        apply_socket_options(fd, options_);
        return TcpStream{fd};
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return Status{ErrorCode::need_more};
    return socket_error();
}

Result<BoundEndpoint> TcpListener::local_endpoint() const noexcept {
    if (fd_ < 0) return Status{ErrorCode::invalid_value};
    sockaddr_storage address{};
    socklen_t size = sizeof(address);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &size) != 0) return socket_error();
    char host[NI_MAXHOST]{};
    char service[NI_MAXSERV]{};
    const int result = ::getnameinfo(reinterpret_cast<sockaddr*>(&address), size,
        host, sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV);
    if (result != 0) return Status{ErrorCode::invalid_value};
    try {
        const auto parsed_port = std::stoul(service);
        if (parsed_port > UINT16_MAX) return Status{ErrorCode::invalid_value};
        return BoundEndpoint{host, static_cast<std::uint16_t>(parsed_port)};
    } catch (...) {
        return Status{ErrorCode::invalid_value};
    }
}

void TcpListener::close() noexcept {
    if (fd_ < 0) return;
    (void)::close(fd_);
    fd_ = -1;
}

} // namespace nimblefix::detail

namespace nimblefix {

TcpTransport::TcpTransport(TcpTransport&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      connecting_(std::exchange(other.connecting_, false)),
      options_(std::move(other.options_)) {}

TcpTransport& TcpTransport::operator=(TcpTransport&& other) noexcept {
    if (this == &other) return *this;
    close();
    fd_ = std::exchange(other.fd_, -1);
    connecting_ = std::exchange(other.connecting_, false);
    options_ = std::move(other.options_);
    return *this;
}

TcpTransport::~TcpTransport() {
    close();
}

Status TcpTransport::connect(std::string_view host, std::uint16_t port) noexcept {
    return connect(host, port, options_);
}

Status TcpTransport::connect(std::string_view host, std::uint16_t port,
                             const SocketOptions& options) noexcept {
    close();
    bool in_progress = false;
    auto stream = detail::TcpStream::connect(host, port, in_progress, options);
    if (!stream.ok()) return stream.status();
    fd_ = stream.value().release_handle();
    connecting_ = in_progress;
    return Status::success();
}

Status TcpTransport::finish_connect() noexcept {
    if (fd_ < 0) return Status{ErrorCode::invalid_value};
    if (!connecting_) return Status::success();
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) != 0)
        return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(errno)};
    if (err != 0) return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(err)};
    sockaddr_storage peer{};
    socklen_t peer_len = sizeof(peer);
    if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len) != 0) {
        if (errno == ENOTCONN) return Status{ErrorCode::need_more};
        return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(errno)};
    }
    connecting_ = false;
    return Status::success();
}

TransportIoResult TcpTransport::read(std::span<char> output) noexcept {
    if (fd_ < 0) return {0, false, true, Status{ErrorCode::invalid_value}};
    if (output.empty()) return {};
    ssize_t count;
    do {
        count = ::recv(fd_, output.data(), output.size(), 0);
    } while (count < 0 && errno == EINTR);
    if (count > 0) return {static_cast<std::size_t>(count), false, false, Status::success()};
    if (count == 0) return {0, false, true, Status::success()};
    if (errno == EAGAIN || errno == EWOULDBLOCK) return {0, true, false, Status::success()};
    return {0, false, false, Status{ErrorCode::io_error, 0, static_cast<std::size_t>(errno)}};
}

TransportIoResult TcpTransport::write(std::span<const char> input) noexcept {
    if (fd_ < 0) return {0, false, true, Status{ErrorCode::invalid_value}};
    if (input.empty()) return {};
    ssize_t count;
    do {
        count = ::send(fd_, input.data(), input.size(), MSG_NOSIGNAL);
    } while (count < 0 && errno == EINTR);
    if (count > 0) return {static_cast<std::size_t>(count), false, false, Status::success()};
    if (count == 0) return {0, false, true, Status::success()};
    if (errno == EAGAIN || errno == EWOULDBLOCK) return {0, true, false, Status::success()};
    return {0, false, false, Status{ErrorCode::io_error, 0, static_cast<std::size_t>(errno)}};
}

TransportIoResult TcpTransport::write_batch(std::span<const std::span<const char>> chunks) noexcept {
    if (fd_ < 0) return {0, false, true, Status{ErrorCode::invalid_value}};
    if (chunks.empty()) return {};
    if (chunks.size() == 1) return write(chunks[0]);
    std::array<iovec, 64> iov{};
    std::size_t count_iov = 0;
    for (const auto& chunk : chunks) {
        if (chunk.empty()) continue;
        if (count_iov >= iov.size()) break;
        iov[count_iov].iov_base = const_cast<char*>(chunk.data());
        iov[count_iov].iov_len = chunk.size();
        ++count_iov;
    }
    if (count_iov == 0) return {};
    msghdr hdr{};
    hdr.msg_iov = iov.data();
    hdr.msg_iovlen = count_iov;
    ssize_t count;
    do {
        count = ::sendmsg(fd_, &hdr, MSG_NOSIGNAL);
    } while (count < 0 && errno == EINTR);
    if (count > 0) return {static_cast<std::size_t>(count), false, false, Status::success()};
    if (count == 0) return {0, false, true, Status::success()};
    if (errno == EAGAIN || errno == EWOULDBLOCK) return {0, true, false, Status::success()};
    return {0, false, false, Status{ErrorCode::io_error, 0, static_cast<std::size_t>(errno)}};
}

void TcpTransport::close() noexcept {
    if (fd_ >= 0) {
        (void)::close(fd_);
        fd_ = -1;
    }
    connecting_ = false;
}

} // namespace nimblefix
