#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"

namespace nimble::transport {

inline constexpr int kDefaultTcpListenBacklog = 64;
inline constexpr std::size_t kMaxReadBufferSize = 1048576U; // Hard safety cap for partially received frames.

/// Non-blocking TCP connection wrapper specialized for FIX frame I/O.
///
/// Design intent: keep the transport surface small and predictable for the
/// runtime. All send/receive methods operate on complete FIX frames, and frame
/// extraction enforces `8=...|9=...|...|10=...` structure before exposing a
/// payload.
///
/// Performance/lifecycle contract:
/// - `Send()`/`ReceiveFrameView()` wait with epoll and do not busy-spin
/// - `BusySend()`/`BusyReceiveFrameView()` spin until timeout and may burn CPU
/// - `SendGather()` uses `writev()` to avoid concatenating segments
/// - `SendZeroCopyGather()` may use `io_uring` for large multi-segment payloads
///   and falls back transparently when unavailable or not worthwhile
/// - frame views borrow an internal buffer and become invalid after the next
///   receive call, `Close()`, move-assignment, or destruction
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

  /// Open a non-blocking TCP connection and wait for connect completion.
  ///
  /// \param host Remote IPv4/hostname to connect to.
  /// \param port Remote TCP port.
  /// \param timeout Maximum connect wait time.
  /// \return Connected `TcpConnection` on success, otherwise an I/O status.
  [[nodiscard]] static auto Connect(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout)
    -> base::Result<TcpConnection>;

  [[nodiscard]] bool valid() const { return fd_ >= 0; }

  [[nodiscard]] int fd() const { return fd_; }

  /// Send one contiguous frame buffer.
  ///
  /// \param bytes Contiguous frame bytes to send.
  /// \param timeout Maximum wait while the socket is backpressured.
  /// \return `Ok()` when all bytes are sent, otherwise an I/O status.
  auto Send(std::span<const std::byte> bytes, std::chrono::milliseconds timeout) -> base::Status;

  /// Send one contiguous frame buffer from a vector.
  ///
  /// \param bytes Contiguous frame bytes to send.
  /// \param timeout Maximum wait while the socket is backpressured.
  /// \return `Ok()` when all bytes are sent, otherwise an I/O status.
  auto Send(const std::vector<std::byte>& bytes, std::chrono::milliseconds timeout) -> base::Status;

  /// Send one logical frame from multiple byte segments.
  ///
  /// \param segments Ordered payload segments written with `writev()`.
  /// \param timeout Maximum wait while the socket is backpressured.
  /// \return `Ok()` when all segments are sent, otherwise an I/O status.
  auto SendGather(std::span<const std::span<const std::byte>> segments, std::chrono::milliseconds timeout)
    -> base::Status;

  /// Send one logical frame from multiple segments, optionally using zero-copy.
  ///
  /// Boundary condition: this is an optimization hint, not a distinct contract.
  /// Small payloads or builds without `io_uring` fall back to `SendGather()`.
  ///
  /// \param segments Ordered payload segments.
  /// \param timeout Maximum wait while the socket is backpressured.
  /// \return `Ok()` when all segments are sent, otherwise an I/O status.
  auto SendZeroCopyGather(std::span<const std::span<const std::byte>> segments, std::chrono::milliseconds timeout)
    -> base::Status;

  /// Send one contiguous frame while busy-spinning on `EAGAIN`.
  ///
  /// Use only on isolated hot paths where trading CPU for latency is acceptable.
  ///
  /// \param bytes Contiguous frame bytes to send.
  /// \param timeout Maximum spin time.
  /// \return `Ok()` when all bytes are sent, otherwise an I/O status.
  auto BusySend(std::span<const std::byte> bytes, std::chrono::milliseconds timeout) -> base::Status;

  /// Poll for one complete FIX frame without blocking.
  ///
  /// The returned span borrows an internal scratch buffer containing exactly one
  /// frame. It becomes invalid after the next receive call or `Close()`.
  ///
  /// \return `std::nullopt` when no complete frame is available yet, or an I/O/format status.
  auto TryReceiveFrameView() -> base::Result<std::optional<std::span<const std::byte>>>;

  /// Wait for one complete FIX frame and return a borrowed view.
  ///
  /// \param timeout Maximum wait time.
  /// \return Borrowed frame view on success, otherwise an I/O/format status.
  auto ReceiveFrameView(std::chrono::milliseconds timeout) -> base::Result<std::span<const std::byte>>;

  /// Wait for one complete FIX frame while busy-spinning.
  ///
  /// \param timeout Maximum spin time.
  /// \return Borrowed frame view on success, otherwise an I/O/format status.
  auto BusyReceiveFrameView(std::chrono::milliseconds timeout) -> base::Result<std::span<const std::byte>>;

  /// Poll for one complete FIX frame and return an owned copy.
  ///
  /// \return `std::nullopt` when no complete frame is available yet, or an I/O/format status.
  auto TryReceiveFrame() -> base::Result<std::optional<std::vector<std::byte>>>;

  /// Wait for one complete FIX frame and return an owned copy.
  ///
  /// \param timeout Maximum wait time.
  /// \return Owned frame bytes on success, otherwise an I/O/format status.
  auto ReceiveFrame(std::chrono::milliseconds timeout) -> base::Result<std::vector<std::byte>>;

  /// Close the socket and clear buffered receive state.
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

/// Non-blocking TCP listener wrapper for FIX acceptor front doors.
///
/// Design intent: expose a tiny accept loop surface to the runtime while
/// keeping listen sockets non-blocking and retrying Linux's transient
/// `accept()`-time network errors.
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

  /// Poll for one pending connection without blocking.
  ///
  /// \return `std::nullopt` when the listen queue is empty, or an I/O status.
  auto TryAccept() -> base::Result<std::optional<TcpConnection>>;

  /// Wait for one pending connection.
  ///
  /// \param timeout Maximum wait time.
  /// \return Connected socket on success, otherwise an I/O status.
  auto Accept(std::chrono::milliseconds timeout) -> base::Result<TcpConnection>;

  /// Close the listen socket and clear the discovered port.
  auto Close() -> void;

private:
  auto Swap(TcpAcceptor& other) noexcept -> void;

  int fd_{ -1 };
  int epoll_fd_ = -1;
  std::uint16_t port_{ 0 };
};

} // namespace nimble::transport