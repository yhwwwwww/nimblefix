#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <nimblefix/status.hpp>
#include <nimblefix/tcp_transport.hpp>
#include <nimblefix/tls.hpp>

namespace nimblefix {

// Optional Layer 0 nonblocking OpenSSL 3 TLS transport for DirectInitiator.
// Links against NimbleFIX::nimblefix_core_tls (keeping nimblefix_core OpenSSL-free).
// Preserves the configured TlsContext across close()/connect() so DirectInitiator
// can reconnect over TLS without reconstructing the transport.
class DirectTlsTransport {
public:
    DirectTlsTransport() noexcept = default;
    DirectTlsTransport(const DirectTlsTransport&) = delete;
    DirectTlsTransport& operator=(const DirectTlsTransport&) = delete;
    DirectTlsTransport(DirectTlsTransport&&) noexcept;
    DirectTlsTransport& operator=(DirectTlsTransport&&) noexcept;
    ~DirectTlsTransport();

    [[nodiscard]] static Result<DirectTlsTransport> create(TlsConfig config) noexcept;

    [[nodiscard]] Status connect(std::string_view host, std::uint16_t port) noexcept;
    [[nodiscard]] Status finish_connect() noexcept;
    [[nodiscard]] TransportIoResult read(std::span<char> output) noexcept;
    [[nodiscard]] TransportIoResult write(std::span<const char> input) noexcept;
    void close() noexcept;

    [[nodiscard]] int native_fd() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool is_connecting() const noexcept;
    [[nodiscard]] bool is_handshaking() const noexcept;
    [[nodiscard]] bool handshake_complete() const noexcept;
    [[nodiscard]] std::size_t pending_plaintext() const noexcept;
    [[nodiscard]] bool wants_read() const noexcept;
    [[nodiscard]] bool wants_write() const noexcept;

private:
    struct Impl;
    explicit DirectTlsTransport(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

static_assert(DirectTransport<DirectTlsTransport>);

} // namespace nimblefix
