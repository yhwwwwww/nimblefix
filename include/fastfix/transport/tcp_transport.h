#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fastfix/base/result.h"
#include "fastfix/base/status.h"

namespace fastfix::transport {

inline constexpr int kDefaultTcpListenBacklog = 64;
inline constexpr std::size_t kMaxReadBufferSize = 1048576U; // 1 MB

class TcpConnection
{
public:
  TcpConnection();
  explicit TcpConnection(int fd);
  ~TcpConnection();

  TcpConnection(const TcpConnection&) = delete;
  auto operator=(const TcpConnection&) -> TcpConnection& = delete;

  TcpConnection(TcpConnection&& other) noexcept;
  auto operator=(TcpConnection&& other) noexcept -> TcpConnection&;

  [[nodiscard]] static auto Connect(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout)
    -> base::Result<TcpConnection>;

  [[nodiscard]] bool valid() const { return fd_ >= 0; }

  [[nodiscard]] int fd() const { return fd_; }

  auto Send(std::span<const std::byte> bytes, std::chrono::milliseconds timeout) -> base::Status;
  auto Send(const std::vector<std::byte>& bytes, std::chrono::milliseconds timeout) -> base::Status;
  auto SendGather(std::span<const std::span<const std::byte>> segments, std::chrono::milliseconds timeout)
    -> base::Status;
  auto SendZeroCopyGather(std::span<const std::span<const std::byte>> segments, std::chrono::milliseconds timeout)
    -> base::Status;
  auto BusySend(std::span<const std::byte> bytes, std::chrono::milliseconds timeout) -> base::Status;
  auto TryReceiveFrameView() -> base::Result<std::optional<std::span<const std::byte>>>;
  auto ReceiveFrameView(std::chrono::milliseconds timeout) -> base::Result<std::span<const std::byte>>;
  auto BusyReceiveFrameView(std::chrono::milliseconds timeout) -> base::Result<std::span<const std::byte>>;
  auto TryReceiveFrame() -> base::Result<std::optional<std::vector<std::byte>>>;
  auto ReceiveFrame(std::chrono::milliseconds timeout) -> base::Result<std::vector<std::byte>>;
  auto Close() -> void;

private:
  auto Swap(TcpConnection& other) noexcept -> void;

  // Returns: 1=ready, 0=timeout, -1=error. Monitors fd for given events using
  // epoll.
  auto EpollWaitFd(int fd, uint32_t events, int timeout_ms) -> int;

  int fd_{ -1 };
  int epoll_fd_ = -1;
  std::vector<std::byte> read_buffer_;
  std::vector<std::byte> frame_buffer_;
  std::size_t read_cursor_{ 0 };
};

class TcpAcceptor
{
public:
  TcpAcceptor() = default;
  explicit TcpAcceptor(int fd, std::uint16_t port);
  ~TcpAcceptor();

  TcpAcceptor(const TcpAcceptor&) = delete;
  auto operator=(const TcpAcceptor&) -> TcpAcceptor& = delete;

  TcpAcceptor(TcpAcceptor&& other) noexcept;
  auto operator=(TcpAcceptor&& other) noexcept -> TcpAcceptor&;

  [[nodiscard]] static auto Listen(const std::string& host, std::uint16_t port, int backlog = kDefaultTcpListenBacklog)
    -> base::Result<TcpAcceptor>;

  [[nodiscard]] bool valid() const { return fd_ >= 0; }

  [[nodiscard]] std::uint16_t port() const { return port_; }

  [[nodiscard]] int fd() const { return fd_; }

  auto TryAccept() -> base::Result<std::optional<TcpConnection>>;
  auto Accept(std::chrono::milliseconds timeout) -> base::Result<TcpConnection>;
  auto Close() -> void;

private:
  auto Swap(TcpAcceptor& other) noexcept -> void;

  int fd_{ -1 };
  int epoll_fd_ = -1;
  std::uint16_t port_{ 0 };
};

} // namespace fastfix::transport