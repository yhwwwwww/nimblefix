#include "fastfix/transport/tcp_transport.h"

#include "fastfix/codec/fix_tags.h"
#include "fastfix/codec/simd_scan.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#ifndef FASTFIX_DISABLE_LIBURING
#include <liburing.h>
#endif

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
constexpr int kSocketOptionEnabled = 1;

auto
IsRetryableAcceptError(int error) -> bool
{
  switch (error) {
    case EINTR:
    case ECONNABORTED:
    case ENETDOWN:
    case EPROTO:
    case ENOPROTOOPT:
    case EHOSTDOWN:
    case ENONET:
    case EHOSTUNREACH:
    case EOPNOTSUPP:
    case ENETUNREACH:
      return true;
    default:
      return false;
  }
}

auto
SetNonBlocking(int fd) -> base::Status
{
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return base::Status::IoError(std::string("fcntl(F_GETFL) failed: ") + std::strerror(errno));
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    return base::Status::IoError(std::string("fcntl(F_SETFL) failed: ") + std::strerror(errno));
  }
  return base::Status::Ok();
}

auto
SetNoDelay(int fd) -> base::Status
{
  int enabled = kSocketOptionEnabled;
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0) {
    return base::Status::IoError(std::string("setsockopt(TCP_NODELAY) failed: ") + std::strerror(errno));
  }
  return base::Status::Ok();
}

auto
TrySetBusyPoll(int fd) -> void
{
  int busy_poll_us = 50;
  // EPERM-tolerant: unprivileged processes can't set SO_BUSY_POLL
  setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_us, sizeof(busy_poll_us));
}

auto
TrySetQuickAck(int fd) -> void
{
  int enabled = 1;
  // TCP_QUICKACK disables delayed ACKs; the kernel may reset it after
  // each recv, so this is only an initial hint.
  setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &enabled, sizeof(enabled));
}

auto
SetReuseAddr(int fd) -> base::Status
{
  int enabled = kSocketOptionEnabled;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0) {
    return base::Status::IoError(std::string("setsockopt(SO_REUSEADDR) failed: ") + std::strerror(errno));
  }
  return base::Status::Ok();
}

auto
ResolveAddress(const std::string& host, std::uint16_t port, int flags) -> base::Result<addrinfo*>
{
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

auto
ReleaseAddress(addrinfo* info) -> void
{
  if (info != nullptr) {
    freeaddrinfo(info);
  }
}

auto
ExtractAssignedPort(int fd) -> base::Result<std::uint16_t>
{
  sockaddr_in address{};
  socklen_t length = sizeof(address);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return base::Status::IoError(std::string("getsockname failed: ") + std::strerror(errno));
  }
  return ntohs(address.sin_port);
}

template<typename Sink>
auto
TryExtractFrame(std::span<const std::byte> buffer, Sink&& sink) -> base::Result<std::size_t>
{
  if (buffer.size() < kMinimumFrameProbeBytes) {
    return std::size_t{ 0 };
  }
  if (static_cast<char>(buffer[0]) != codec::tags::kBeginStringPrefix.front() ||
      static_cast<char>(buffer[1]) != codec::tags::kBeginStringPrefix[1]) {
    return base::Status::FormatError("FIX frame must begin with tag 8");
  }

  constexpr auto kDelim = std::byte{ '\x01' };
  const auto* first_ptr = codec::FindByte(buffer.data(), buffer.size(), kDelim);
  const auto first_delimiter = static_cast<std::size_t>(first_ptr - buffer.data());
  if (first_delimiter >= buffer.size()) {
    return std::size_t{ 0 };
  }
  if (first_delimiter + kBodyLengthFieldPrefixSize >= buffer.size()) {
    return std::size_t{ 0 };
  }
  if (static_cast<char>(buffer[first_delimiter + 1U]) != codec::tags::kBodyLengthPrefix.front() ||
      static_cast<char>(buffer[first_delimiter + 2U]) != codec::tags::kBodyLengthPrefix[1]) {
    return base::Status::FormatError("FIX frame must place BodyLength immediately after BeginString");
  }

  const auto* scan_start = buffer.data() + first_delimiter + kBodyLengthFieldPrefixSize;
  const auto scan_len = buffer.size() - first_delimiter - kBodyLengthFieldPrefixSize;
  const auto* second_ptr = codec::FindByte(scan_start, scan_len, kDelim);
  const auto second_delimiter = static_cast<std::size_t>(second_ptr - buffer.data());
  if (second_delimiter >= buffer.size()) {
    return std::size_t{ 0 };
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
    return std::size_t{ 0 };
  }

  sink(buffer.subspan(0, total_size));
  return total_size;
}

} // namespace

TcpConnection::TcpConnection()
{
  read_buffer_.reserve(kDefaultReadBufferCapacity);
  frame_buffer_.reserve(kDefaultFrameBufferCapacity);
}

TcpConnection::TcpConnection(int fd)
  : fd_(fd)
{
  read_buffer_.reserve(kDefaultReadBufferCapacity);
  frame_buffer_.reserve(kDefaultFrameBufferCapacity);
}

TcpConnection::~TcpConnection()
{
  Close();
}

TcpConnection::TcpConnection(TcpConnection&& other) noexcept
{
  Swap(other);
}

auto
TcpConnection::operator=(TcpConnection&& other) noexcept -> TcpConnection&
{
  if (this != &other) {
    Close();
    Swap(other);
  }
  return *this;
}

auto
TcpConnection::Swap(TcpConnection& other) noexcept -> void
{
  std::swap(fd_, other.fd_);
  std::swap(epoll_fd_, other.epoll_fd_);
  std::swap(read_buffer_, other.read_buffer_);
  std::swap(frame_buffer_, other.frame_buffer_);
  std::swap(read_cursor_, other.read_cursor_);
}

auto
TcpConnection::Connect(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout)
  -> base::Result<TcpConnection>
{
  auto addresses = ResolveAddress(host, port, 0);
  if (!addresses.ok()) {
    return addresses.status();
  }

  const auto timeout_ms = timeout.count() > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
                                                                            : static_cast<int>(timeout.count());

  const int epfd = epoll_create1(EPOLL_CLOEXEC);
  if (epfd < 0) {
    ReleaseAddress(addresses.value());
    return base::Status::IoError("epoll_create1 failed");
  }

  addrinfo* results = addresses.value();
  for (auto* address = results; address != nullptr; address = address->ai_next) {
    const int fd = socket(address->ai_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
      continue;
    }

    const int rc = connect(fd, address->ai_addr, address->ai_addrlen);
    if (rc != 0) {
      if (errno != EINPROGRESS) {
        close(fd);
        continue;
      }
      // Wait for connection via epoll
      struct epoll_event ev{};
      ev.events = EPOLLOUT;
      ev.data.fd = fd;
      if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        close(fd);
        continue;
      }
      struct epoll_event out{};
      int eret;
      do {
        eret = epoll_wait(epfd, &out, 1, timeout_ms);
      } while (eret == -1 && errno == EINTR);
      epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
      if (eret <= 0 || (out.events & (EPOLLERR | EPOLLHUP))) {
        close(fd);
        continue;
      }
    }

    int socket_error = 0;
    socklen_t socket_error_length = sizeof(socket_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_length) != 0 || socket_error != 0) {
      close(fd);
      continue;
    }

    auto status = SetNoDelay(fd);
    if (!status.ok()) {
      close(fd);
      ::close(epfd);
      ReleaseAddress(results);
      return status;
    }
    TrySetBusyPoll(fd);
    TrySetQuickAck(fd);

    ::close(epfd);
    ReleaseAddress(results);
    return TcpConnection(fd);
  }

  ::close(epfd);
  ReleaseAddress(results);
  return base::Status::IoError("could not connect to TCP endpoint");
}

auto
TcpConnection::Send(std::span<const std::byte> bytes, std::chrono::milliseconds timeout) -> base::Status
{
  if (epoll_fd_ < 0) {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
      return base::Status::IoError("epoll_create1 failed");
  }
  const auto timeout_ms = timeout.count() > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
                                                                            : static_cast<int>(timeout.count());
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    int poll_ret = EpollWaitFd(fd_, EPOLLOUT, timeout_ms);
    if (poll_ret == 0) {
      return base::Status::IoError("socket operation timed out");
    }
    if (poll_ret < 0) {
      return base::Status::IoError("socket closed or errored while polling");
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

auto
TcpConnection::Send(const std::vector<std::byte>& bytes, std::chrono::milliseconds timeout) -> base::Status
{
  return Send(std::span<const std::byte>(bytes.data(), bytes.size()), timeout);
}

auto
TcpConnection::SendGather(std::span<const std::span<const std::byte>> segments, std::chrono::milliseconds timeout)
  -> base::Status
{
  if (segments.empty()) {
    return base::Status::Ok();
  }
  if (segments.size() == 1U) {
    return Send(segments[0], timeout);
  }

  if (epoll_fd_ < 0) {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
      return base::Status::IoError("epoll_create1 failed");
  }
  const auto timeout_ms = timeout.count() > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
                                                                            : static_cast<int>(timeout.count());

  // Build iovec array
  std::vector<struct iovec> iov(segments.size());
  std::size_t total_bytes = 0;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    iov[i].iov_base = const_cast<void*>(static_cast<const void*>(segments[i].data()));
    iov[i].iov_len = segments[i].size();
    total_bytes += segments[i].size();
  }

  std::size_t sent = 0;
  std::size_t current_iov = 0;
  while (sent < total_bytes) {
    int poll_ret = EpollWaitFd(fd_, EPOLLOUT, timeout_ms);
    if (poll_ret == 0) {
      return base::Status::IoError("socket operation timed out");
    }
    if (poll_ret < 0) {
      return base::Status::IoError("socket closed or errored while polling");
    }

    const auto remaining_iovs = static_cast<int>(iov.size() - current_iov);
    const auto rc = ::writev(fd_, &iov[current_iov], remaining_iovs);
    if (rc < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return base::Status::IoError(std::string("writev failed: ") + std::strerror(errno));
    }
    if (rc == 0) {
      return base::Status::IoError("peer closed the socket during send");
    }

    sent += static_cast<std::size_t>(rc);

    // Advance iov pointers past fully-sent segments
    auto remaining = static_cast<std::size_t>(rc);
    while (current_iov < iov.size() && remaining >= iov[current_iov].iov_len) {
      remaining -= iov[current_iov].iov_len;
      ++current_iov;
    }
    // Adjust partially-sent segment
    if (remaining > 0 && current_iov < iov.size()) {
      iov[current_iov].iov_base = static_cast<char*>(iov[current_iov].iov_base) + remaining;
      iov[current_iov].iov_len -= remaining;
    }
  }
  return base::Status::Ok();
}

#ifndef FASTFIX_DISABLE_LIBURING
auto
TcpConnection::SendZeroCopyGather(std::span<const std::span<const std::byte>> segments,
                                  std::chrono::milliseconds timeout) -> base::Status
{
  if (segments.empty()) {
    return base::Status::Ok();
  }
  // Fallback to writev for small payloads (io_uring setup overhead not worth
  // it).
  std::size_t total_bytes = 0;
  for (const auto& seg : segments) {
    total_bytes += seg.size();
  }
  if (total_bytes < 4096U || segments.size() < 2U) {
    return SendGather(segments, timeout);
  }

  struct io_uring ring{};
  if (io_uring_queue_init(static_cast<unsigned>(segments.size()), &ring, 0) < 0) {
    // io_uring not available — fall back to writev
    return SendGather(segments, timeout);
  }

  // Submit one SEND_ZC SQE per segment; use MSG_MORE on all but the last.
  for (std::size_t i = 0; i < segments.size(); ++i) {
    auto* sqe = io_uring_get_sqe(&ring);
    if (sqe == nullptr) {
      io_uring_queue_exit(&ring);
      return SendGather(segments, timeout);
    }
    io_uring_prep_send_zc(
      sqe, fd_, segments[i].data(), segments[i].size(), MSG_NOSIGNAL | (i + 1 < segments.size() ? MSG_MORE : 0), 0);
    sqe->user_data = i;
  }

  const auto submitted = io_uring_submit(&ring);
  if (submitted < 0) {
    io_uring_queue_exit(&ring);
    return SendGather(segments, timeout);
  }

  // Wait for all completions (send + notification CQEs).
  int remaining = static_cast<int>(segments.size());
  while (remaining > 0) {
    struct io_uring_cqe* cqe = nullptr;
    struct __kernel_timespec ts{};
    ts.tv_sec = timeout.count() / 1000;
    ts.tv_nsec = (timeout.count() % 1000) * 1000000;
    const auto ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
    if (ret < 0) {
      io_uring_queue_exit(&ring);
      if (ret == -ETIME) {
        return base::Status::IoError("io_uring send_zc timed out");
      }
      return base::Status::IoError("io_uring wait failed");
    }
    if (cqe->res < 0) {
      const auto err = -cqe->res;
      io_uring_cqe_seen(&ring, cqe);
      io_uring_queue_exit(&ring);
      return base::Status::IoError(std::string("io_uring send_zc failed: ") + std::strerror(err));
    }
    // SEND_ZC produces a notification CQE (IORING_CQE_F_NOTIF) after the data
    // CQE. Only count non-notification completions.
    if (!(cqe->flags & IORING_CQE_F_NOTIF)) {
      --remaining;
    }
    io_uring_cqe_seen(&ring, cqe);
  }

  io_uring_queue_exit(&ring);
  return base::Status::Ok();
}
#else
auto
TcpConnection::SendZeroCopyGather(std::span<const std::span<const std::byte>> segments,
                                  std::chrono::milliseconds timeout) -> base::Status
{
  return SendGather(segments, timeout);
}
#endif

auto
TcpConnection::BusySend(std::span<const std::byte> bytes, std::chrono::milliseconds timeout) -> base::Status
{
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

auto
TcpConnection::TryReceiveFrameView() -> base::Result<std::optional<std::span<const std::byte>>>
{
  while (true) {
    auto unconsumed =
      std::span<const std::byte>(read_buffer_.data() + read_cursor_, read_buffer_.size() - read_cursor_);
    auto consumed = TryExtractFrame(
      unconsumed, [&](std::span<const std::byte> frame) { frame_buffer_.assign(frame.begin(), frame.end()); });
    if (consumed.ok() && consumed.value() > 0) {
      read_cursor_ += consumed.value();
      if (read_cursor_ == read_buffer_.size()) {
        read_buffer_.clear();
        read_cursor_ = 0;
      } else if (read_cursor_ > read_buffer_.capacity() / 2) {
        read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + static_cast<std::ptrdiff_t>(read_cursor_));
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

auto
TcpConnection::ReceiveFrameView(std::chrono::milliseconds timeout) -> base::Result<std::span<const std::byte>>
{
  if (epoll_fd_ < 0) {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
      return base::Status::IoError("epoll_create1 failed");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    auto unconsumed =
      std::span<const std::byte>(read_buffer_.data() + read_cursor_, read_buffer_.size() - read_cursor_);
    auto consumed = TryExtractFrame(
      unconsumed, [&](std::span<const std::byte> frame) { frame_buffer_.assign(frame.begin(), frame.end()); });
    if (consumed.ok() && consumed.value() > 0) {
      read_cursor_ += consumed.value();
      if (read_cursor_ == read_buffer_.size()) {
        read_buffer_.clear();
        read_cursor_ = 0;
      } else if (read_cursor_ > read_buffer_.capacity() / 2) {
        read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + static_cast<std::ptrdiff_t>(read_cursor_));
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
    const auto remaining_ms = remaining.count() > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
                                                                                  : static_cast<int>(remaining.count());

    int poll_ret = EpollWaitFd(fd_, EPOLLIN, remaining_ms);
    if (poll_ret == 0) {
      return base::Status::IoError("socket operation timed out");
    }
    if (poll_ret < 0) {
      return base::Status::IoError("socket closed or errored while polling");
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

auto
TcpConnection::BusyReceiveFrameView(std::chrono::milliseconds timeout) -> base::Result<std::span<const std::byte>>
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    auto unconsumed =
      std::span<const std::byte>(read_buffer_.data() + read_cursor_, read_buffer_.size() - read_cursor_);
    auto consumed = TryExtractFrame(
      unconsumed, [&](std::span<const std::byte> frame) { frame_buffer_.assign(frame.begin(), frame.end()); });
    if (consumed.ok() && consumed.value() > 0) {
      read_cursor_ += consumed.value();
      if (read_cursor_ == read_buffer_.size()) {
        read_buffer_.clear();
        read_cursor_ = 0;
      } else if (read_cursor_ > read_buffer_.capacity() / 2) {
        read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + static_cast<std::ptrdiff_t>(read_cursor_));
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

auto
TcpConnection::TryReceiveFrame() -> base::Result<std::optional<std::vector<std::byte>>>
{
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

auto
TcpConnection::ReceiveFrame(std::chrono::milliseconds timeout) -> base::Result<std::vector<std::byte>>
{
  auto frame = ReceiveFrameView(timeout);
  if (!frame.ok()) {
    return frame.status();
  }
  return std::vector<std::byte>(frame.value().begin(), frame.value().end());
}

auto
TcpConnection::EpollWaitFd(int fd, uint32_t events, int timeout_ms) -> int
{
  struct epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
    if (errno == EEXIST) {
      epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
    } else {
      return -1;
    }
  }
  struct epoll_event out{};
  int ret;
  do {
    ret = epoll_wait(epoll_fd_, &out, 1, timeout_ms);
  } while (ret == -1 && errno == EINTR);
  epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  if (ret == 0)
    return 0; // timeout
  if (ret < 0)
    return -1; // error
  if (out.events & (EPOLLERR | EPOLLHUP))
    return -1;
  return 1; // ready
}

auto
TcpConnection::Close() -> void
{
  if (epoll_fd_ >= 0) {
    ::close(epoll_fd_);
    epoll_fd_ = -1;
  }
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  read_buffer_.clear();
  read_cursor_ = 0;
}

TcpAcceptor::TcpAcceptor(int fd, std::uint16_t port)
  : fd_(fd)
  , port_(port)
{
}

TcpAcceptor::~TcpAcceptor()
{
  Close();
}

TcpAcceptor::TcpAcceptor(TcpAcceptor&& other) noexcept
{
  Swap(other);
}

auto
TcpAcceptor::operator=(TcpAcceptor&& other) noexcept -> TcpAcceptor&
{
  if (this != &other) {
    Close();
    Swap(other);
  }
  return *this;
}

auto
TcpAcceptor::Swap(TcpAcceptor& other) noexcept -> void
{
  std::swap(fd_, other.fd_);
  std::swap(epoll_fd_, other.epoll_fd_);
  std::swap(port_, other.port_);
}

auto
TcpAcceptor::Listen(const std::string& host, std::uint16_t port, int backlog) -> base::Result<TcpAcceptor>
{
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

auto
TcpAcceptor::Accept(std::chrono::milliseconds timeout) -> base::Result<TcpConnection>
{
  if (epoll_fd_ < 0) {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
      return base::Status::IoError("epoll_create1 failed");
  }
  const auto timeout_ms = timeout.count() > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
                                                                            : static_cast<int>(timeout.count());
  struct epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = fd_;
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd_, &ev) != 0) {
    return base::Status::IoError("epoll_ctl failed in Accept");
  }
  struct epoll_event out{};
  int ret;
  do {
    ret = epoll_wait(epoll_fd_, &out, 1, timeout_ms);
  } while (ret == -1 && errno == EINTR);
  epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd_, nullptr);
  if (ret == 0) {
    return base::Status::IoError("socket operation timed out");
  }
  if (ret < 0 || (out.events & (EPOLLERR | EPOLLHUP))) {
    return base::Status::IoError("socket closed or errored while polling");
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

auto
TcpAcceptor::TryAccept() -> base::Result<std::optional<TcpConnection>>
{
  while (true) {
    const int connection_fd = accept(fd_, nullptr, nullptr);
    if (connection_fd < 0) {
      const int accept_error = errno;
      if (accept_error == EAGAIN || accept_error == EWOULDBLOCK) {
        return std::optional<TcpConnection>{};
      }
      // Linux may surface pending TCP/IP errors from accept(); keep
      // draining the listen queue instead of terminating the front door.
      if (IsRetryableAcceptError(accept_error)) {
        continue;
      }
      return base::Status::IoError(std::string("accept failed: ") + std::strerror(accept_error));
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
}

auto
TcpAcceptor::Close() -> void
{
  if (epoll_fd_ >= 0) {
    ::close(epoll_fd_);
    epoll_fd_ = -1;
  }
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  port_ = 0;
}

} // namespace fastfix::transport