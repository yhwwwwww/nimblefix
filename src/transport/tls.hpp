#pragma once

#include <memory>
#include <span>
#include <string_view>

#include "tcp.hpp"
#include <nimblefix/tls.hpp>

namespace nimblefix::detail {

class TlsContext {
  public:
    TlsContext() noexcept = default;
    TlsContext(const TlsContext &) = delete;
    TlsContext &operator=(const TlsContext &) = delete;
    TlsContext(TlsContext &&) noexcept;
    TlsContext &operator=(TlsContext &&) noexcept;
    ~TlsContext();
    [[nodiscard]] static Result<TlsContext> create(const TlsConfig &config, bool server) noexcept;
    [[nodiscard]] bool valid() const noexcept;

  private:
    struct Impl;
    explicit TlsContext(std::shared_ptr<Impl> impl) noexcept;
    friend class TlsStream;
    std::shared_ptr<Impl> impl_;
};

// The socket remains owned by TcpStream. The TLS object owns bounded retry
// plaintext and OpenSSL record state, and never changes process signal policy.
class TlsStream {
  public:
    TlsStream() noexcept;
    TlsStream(const TlsStream &) = delete;
    TlsStream &operator=(const TlsStream &) = delete;
    TlsStream(TlsStream &&) noexcept;
    TlsStream &operator=(TlsStream &&) noexcept;
    ~TlsStream();
    [[nodiscard]] static Result<TlsStream> create(const TlsContext &context, int socket,
                                                  std::string_view peer_name = {}) noexcept;
    [[nodiscard]] SocketIoResult handshake() noexcept;
    [[nodiscard]] SocketIoResult read(std::span<char> output) noexcept;
    [[nodiscard]] SocketIoResult write(std::span<const char> input) noexcept;
    [[nodiscard]] bool handshake_complete() const noexcept;
    [[nodiscard]] bool wants_read() const noexcept;
    [[nodiscard]] bool wants_write() const noexcept;
    [[nodiscard]] bool read_wants_write() const noexcept;
    [[nodiscard]] bool write_wants_read() const noexcept;
    [[nodiscard]] std::size_t pending_plaintext() const noexcept;
    [[nodiscard]] std::size_t retry_plaintext_bytes() const noexcept;
    void close() noexcept;

  private:
    struct Impl;
    explicit TlsStream(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace nimblefix::detail
