#include "tcp.hpp"

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
                                    bool& in_progress) noexcept {
    in_progress = false;
    if (!is_numeric_host(host) || port == 0) return Status{ErrorCode::invalid_value};
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

TcpListener::TcpListener(TcpListener&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this == &other) return *this;
    close();
    fd_ = std::exchange(other.fd_, -1);
    return *this;
}

TcpListener::~TcpListener() {
    close();
}

Result<TcpListener> TcpListener::bind(std::string_view host, std::uint16_t port,
                                      int backlog) noexcept {
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
            if (::bind(fd, address->ai_addr, address->ai_addrlen) == 0 &&
                ::listen(fd, backlog) == 0) {
                ::freeaddrinfo(addresses);
                return TcpListener{fd};
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
