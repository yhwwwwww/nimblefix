#include <nimblefix/direct_tls_transport.hpp>

#include <string>
#include <utility>

#include "tls.hpp"

namespace nimblefix {

struct DirectTlsTransport::Impl {
    TlsConfig config{};
    detail::TlsContext context{};
    TcpTransport tcp{};
    detail::TlsStream tls{};
    std::string peer_name{};
    bool tls_created{};
    bool tls_handshaking{};
};

DirectTlsTransport::DirectTlsTransport(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

DirectTlsTransport::DirectTlsTransport(DirectTlsTransport&&) noexcept = default;
DirectTlsTransport& DirectTlsTransport::operator=(DirectTlsTransport&&) noexcept = default;

DirectTlsTransport::~DirectTlsTransport() {
    close();
}

Result<DirectTlsTransport> DirectTlsTransport::create(TlsConfig config) noexcept {
    if (!config.enabled) return Status{ErrorCode::invalid_config};
    auto ctx = detail::TlsContext::create(config, false);
    if (!ctx.ok()) return ctx.status();
    try {
        auto impl = std::make_unique<Impl>();
        impl->config = std::move(config);
        impl->context = std::move(ctx).value();
        return DirectTlsTransport{std::move(impl)};
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded};
    }
}

Status DirectTlsTransport::connect(std::string_view host, std::uint16_t port) noexcept {
    if (!impl_ || !impl_->context.valid()) return Status{ErrorCode::invalid_config};
    close();
    try {
        impl_->peer_name = impl_->config.server_name.empty()
            ? std::string{host}
            : impl_->config.server_name;
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded};
    }
    if (const auto st = impl_->tcp.connect(host, port); !st.ok()) return st;
    impl_->tls_handshaking = true;
    if (!impl_->tcp.is_connecting()) {
        const auto hs = finish_connect();
        if (hs.code == ErrorCode::need_more) return Status::success();
        return hs;
    }
    return Status::success();
}

Status DirectTlsTransport::finish_connect() noexcept {
    if (!impl_ || !impl_->tcp.valid()) return Status{ErrorCode::not_ready};
    if (impl_->tcp.is_connecting()) {
        const auto st = impl_->tcp.finish_connect();
        if (st.code == ErrorCode::need_more) return st;
        if (!st.ok()) {
            close();
            return st;
        }
    }
    if (!impl_->tls_handshaking) return Status::success();

    if (!impl_->tls_created) {
        auto stream = detail::TlsStream::create(impl_->context, impl_->tcp.native_fd(), impl_->peer_name);
        if (!stream.ok()) {
            close();
            return stream.status();
        }
        impl_->tls = std::move(stream).value();
        impl_->tls_created = true;
    }

    const auto io = impl_->tls.handshake();
    if (!io.status.ok()) {
        close();
        return io.status;
    }
    if (io.peer_closed) {
        close();
        return Status{ErrorCode::connection_closed};
    }
    if (io.would_block || !impl_->tls.handshake_complete()) {
        return Status{ErrorCode::need_more};
    }
    impl_->tls_handshaking = false;
    return Status::success();
}

TransportIoResult DirectTlsTransport::read(std::span<char> output) noexcept {
    TransportIoResult res{};
    if (!impl_ || !impl_->tcp.valid() || impl_->tls_handshaking) {
        res.status = Status{ErrorCode::not_ready};
        return res;
    }
    const auto io = impl_->tls.read(output);
    res.bytes = io.bytes;
    res.would_block = io.would_block;
    res.peer_closed = io.peer_closed;
    res.status = io.status;
    return res;
}

TransportIoResult DirectTlsTransport::write(std::span<const char> input) noexcept {
    TransportIoResult res{};
    if (!impl_ || !impl_->tcp.valid() || impl_->tls_handshaking) {
        res.status = Status{ErrorCode::not_ready};
        return res;
    }
    const auto io = impl_->tls.write(input);
    res.bytes = io.bytes;
    res.would_block = io.would_block;
    res.peer_closed = io.peer_closed;
    res.status = io.status;
    return res;
}

void DirectTlsTransport::close() noexcept {
    if (!impl_) return;
    impl_->tls.close();
    impl_->tcp.close();
    impl_->tls_created = false;
    impl_->tls_handshaking = false;
}

int DirectTlsTransport::native_fd() const noexcept {
    return impl_ ? impl_->tcp.native_fd() : -1;
}

bool DirectTlsTransport::valid() const noexcept {
    return impl_ && impl_->tcp.valid();
}

bool DirectTlsTransport::is_connecting() const noexcept {
    return impl_ && impl_->tcp.valid() && (impl_->tcp.is_connecting() || impl_->tls_handshaking);
}

bool DirectTlsTransport::is_handshaking() const noexcept {
    return impl_ && impl_->tcp.valid() && !impl_->tcp.is_connecting() && impl_->tls_handshaking;
}

bool DirectTlsTransport::handshake_complete() const noexcept {
    return impl_ && impl_->tcp.valid() && !impl_->tls_handshaking && impl_->tls.handshake_complete();
}

std::size_t DirectTlsTransport::pending_plaintext() const noexcept {
    return impl_ ? impl_->tls.pending_plaintext() : 0;
}

bool DirectTlsTransport::wants_read() const noexcept {
    return impl_ && impl_->tls.wants_read();
}

bool DirectTlsTransport::wants_write() const noexcept {
    return impl_ && (impl_->tcp.is_connecting() || impl_->tls.wants_write());
}

} // namespace nimblefix
