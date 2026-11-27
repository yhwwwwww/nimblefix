#include "tls.hpp"

#include <nimblefix/protocol.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>

#if defined(NIMBLEFIX_ENABLE_TLS)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#endif

namespace nimblefix {
bool tls_available() noexcept {
#if defined(NIMBLEFIX_ENABLE_TLS)
    return true;
#else
    return false;
#endif
}
} // namespace nimblefix

namespace nimblefix::detail {

#if defined(NIMBLEFIX_ENABLE_TLS)
namespace {
constexpr std::size_t record_plaintext_bytes = 16'384;

bool safe_text(std::string_view text) noexcept {
    for (const unsigned char ch : text)
        if (ch < 0x20 || ch == 0x7f)
            return false;
    return true;
}
bool safe_file(const std::filesystem::path &path, std::size_t limit) noexcept {
    try {
        const auto native = path.native();
        if (native.empty() || native.find('\0') != std::string::npos)
            return false;
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        return !error && size != 0 && size <= limit;
    } catch (...) {
        return false;
    }
}
Status ssl_failure(ErrorCode code = ErrorCode::tls_error) noexcept {
    return {code, 0, static_cast<std::size_t>(ERR_peek_last_error())};
}

struct BioSocket {
    int fd{-1};
    bool eof{};
};
int bio_create(BIO *bio) {
    BIO_set_init(bio, 1);
    BIO_set_data(bio, nullptr);
    BIO_set_shutdown(bio, 0);
    return 1;
}
int bio_destroy(BIO *bio) {
    if (bio != nullptr) {
        BIO_set_data(bio, nullptr);
        BIO_set_init(bio, 0);
    }
    return 1;
}
int bio_read(BIO *bio, char *destination, std::size_t capacity, std::size_t *used) {
    *used = 0;
    BIO_clear_retry_flags(bio);
    auto *socket = static_cast<BioSocket *>(BIO_get_data(bio));
    if (socket == nullptr || destination == nullptr || capacity == 0)
        return 0;
    const auto result = ::recv(socket->fd, destination, capacity, 0);
    if (result > 0) {
        *used = static_cast<std::size_t>(result);
        return 1;
    }
    if (result == 0)
        socket->eof = true;
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        BIO_set_retry_read(bio);
    return 0;
}
int bio_write(BIO *bio, const char *data, std::size_t size, std::size_t *used) {
    *used = 0;
    BIO_clear_retry_flags(bio);
    auto *socket = static_cast<BioSocket *>(BIO_get_data(bio));
    if (socket == nullptr || data == nullptr || size == 0)
        return 0;
    const auto result = ::send(socket->fd, data, size, MSG_NOSIGNAL);
    if (result > 0) {
        *used = static_cast<std::size_t>(result);
        return 1;
    }
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        BIO_set_retry_write(bio);
    return 0;
}
long bio_control(BIO *bio, int command, long, void *argument) {
    auto *socket = static_cast<BioSocket *>(BIO_get_data(bio));
    switch (command) {
    case BIO_CTRL_FLUSH:
        return 1;
    case BIO_CTRL_EOF:
        return socket != nullptr && socket->eof;
    case BIO_C_GET_FD:
        if (socket == nullptr)
            return -1;
        if (argument != nullptr)
            *static_cast<int *>(argument) = socket->fd;
        return socket->fd;
    case BIO_CTRL_GET_CLOSE:
        return 0;
    case BIO_CTRL_SET_CLOSE:
        return 1;
    case BIO_CTRL_DUP:
        return 1;
    case BIO_CTRL_PENDING:
    case BIO_CTRL_WPENDING:
        return 0;
    default:
        return 0;
    }
}
const BIO_METHOD *socket_method() {
    struct Method {
        BIO_METHOD *value{
            BIO_meth_new(BIO_TYPE_SOURCE_SINK | BIO_get_new_index(), "NimbleFIX nonblocking socket")};
        Method() {
            if (value == nullptr)
                return;
            if (BIO_meth_set_create(value, bio_create) != 1 ||
                BIO_meth_set_destroy(value, bio_destroy) != 1 || BIO_meth_set_read_ex(value, bio_read) != 1 ||
                BIO_meth_set_write_ex(value, bio_write) != 1 || BIO_meth_set_ctrl(value, bio_control) != 1) {
                BIO_meth_free(value);
                value = nullptr;
            }
        }
        ~Method() { BIO_meth_free(value); }
    };
    static const Method method;
    return method.value;
}
} // namespace

struct TlsContext::Impl {
    SSL_CTX *context{};
    bool server{};
    ~Impl() { SSL_CTX_free(context); }
};

struct TlsStream::Impl {
    std::shared_ptr<TlsContext::Impl> context;
    BioSocket socket;
    SSL *ssl{};
    std::array<char, record_plaintext_bytes> retry{};
    std::size_t retry_size{};
    bool complete{};
    bool handshake_read{}, handshake_write{};
    bool read_read{}, read_write{}, write_read{}, write_write{};
    ~Impl() {
        SSL_free(ssl);
        OPENSSL_cleanse(retry.data(), retry.size());
    }

    SocketIoResult result(int result_code, bool &read_interest, bool &write_interest) noexcept {
        const auto error = SSL_get_error(ssl, result_code);
        read_interest = error == SSL_ERROR_WANT_READ;
        write_interest = error == SSL_ERROR_WANT_WRITE;
        if (read_interest || write_interest)
            return {0, true, false, {}};
        if (error == SSL_ERROR_ZERO_RETURN)
            return {0, false, true, {}};
        if (SSL_get_verify_result(ssl) != X509_V_OK)
            return {0,
                    false,
                    false,
                    {ErrorCode::certificate_error, 0, static_cast<std::size_t>(SSL_get_verify_result(ssl))}};
        return {0, false, false, ssl_failure()};
    }
};

Result<TlsContext> TlsContext::create(const TlsConfig &config, bool server) noexcept {
    try {
        if (!config.enabled || (config.minimum_version != 12 && config.minimum_version != 13) ||
            config.handshake_timeout_ms == 0 || config.max_certificate_bytes < 1024 ||
            config.max_certificate_bytes > 4U * 1024U * 1024U ||
            config.server_name.size() > max_tls_hostname_bytes ||
            !safe_text(config.server_name))
            return Status{ErrorCode::invalid_config};
        if (server && (config.certificate_file.empty() || config.private_key_file.empty()))
            return Status{ErrorCode::invalid_config};
        if (config.certificate_file.empty() != config.private_key_file.empty())
            return Status{ErrorCode::invalid_config};
        if ((!config.ca_file.empty() && !safe_file(config.ca_file, 4U * 1024U * 1024U)) ||
            (!config.certificate_file.empty() &&
             !safe_file(config.certificate_file, config.max_certificate_bytes)) ||
            (!config.private_key_file.empty() && !safe_file(config.private_key_file, 65'536)))
            return Status{ErrorCode::certificate_error};
        auto impl = std::make_shared<Impl>();
        impl->server = server;
        ERR_clear_error();
        impl->context = SSL_CTX_new(server ? TLS_server_method() : TLS_client_method());
        if (impl->context == nullptr)
            return ssl_failure();
        SSL_CTX_set_default_passwd_cb(impl->context, +[](char *, int, int, void *) -> int { return 0; });
        if (SSL_CTX_set_min_proto_version(impl->context, config.minimum_version == 13 ? TLS1_3_VERSION
                                                                                      : TLS1_2_VERSION) != 1)
            return ssl_failure();
        SSL_CTX_set_options(impl->context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
        SSL_CTX_set_mode(impl->context, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        SSL_CTX_set_read_ahead(impl->context, 0);
        SSL_CTX_set_max_cert_list(impl->context, static_cast<long>(config.max_certificate_bytes));
        SSL_CTX_set_verify_depth(impl->context, 8);
        SSL_CTX_set_session_cache_mode(impl->context, SSL_SESS_CACHE_OFF);
        SSL_CTX_set_num_tickets(impl->context, 0);
        if (!config.ca_file.empty()) {
            if (SSL_CTX_load_verify_locations(impl->context, config.ca_file.c_str(), nullptr) != 1)
                return ssl_failure(ErrorCode::certificate_error);
        } else if ((!server || config.require_client_certificate) &&
                   SSL_CTX_set_default_verify_paths(impl->context) != 1)
            return ssl_failure(ErrorCode::certificate_error);
        if (!config.certificate_file.empty()) {
            if (SSL_CTX_use_certificate_chain_file(impl->context, config.certificate_file.c_str()) != 1 ||
                SSL_CTX_use_PrivateKey_file(impl->context, config.private_key_file.c_str(),
                                            SSL_FILETYPE_PEM) != 1 ||
                SSL_CTX_check_private_key(impl->context) != 1)
                return ssl_failure(ErrorCode::certificate_error);
        }
        SSL_CTX_set_verify(impl->context,
                           server ? (config.require_client_certificate
                                         ? SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT
                                         : SSL_VERIFY_NONE)
                                  : SSL_VERIFY_PEER,
                           nullptr);
        return TlsContext{std::move(impl)};
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded};
    }
}

Result<TlsStream> TlsStream::create(const TlsContext &context, int socket,
                                    std::string_view peer_name) noexcept {
    if (!context.impl_ || socket < 0 || peer_name.size() > max_tls_hostname_bytes || !safe_text(peer_name))
        return Status{ErrorCode::invalid_config};
    try {
        auto impl = std::make_unique<Impl>();
        impl->context = context.impl_;
        impl->socket.fd = socket;
        ERR_clear_error();
        impl->ssl = SSL_new(context.impl_->context);
        if (impl->ssl == nullptr)
            return ssl_failure();
        const auto *method = socket_method();
        BIO *bio = method != nullptr ? BIO_new(method) : nullptr;
        if (bio == nullptr)
            return ssl_failure();
        BIO_set_data(bio, &impl->socket);
        SSL_set_bio(impl->ssl, bio, bio);
        if (context.impl_->server)
            SSL_set_accept_state(impl->ssl);
        else {
            if (peer_name.empty())
                return Status{ErrorCode::invalid_config};
            std::string name{peer_name};
            in_addr ipv4{};
            in6_addr ipv6{};
            const bool numeric = ::inet_pton(AF_INET, name.c_str(), &ipv4) == 1 ||
                                 ::inet_pton(AF_INET6, name.c_str(), &ipv6) == 1;
            if (numeric) {
                if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(impl->ssl), name.c_str()) != 1)
                    return ssl_failure(ErrorCode::certificate_error);
            } else if (SSL_set1_host(impl->ssl, name.c_str()) != 1 ||
                       SSL_set_tlsext_host_name(impl->ssl, name.c_str()) != 1)
                return ssl_failure(ErrorCode::certificate_error);
            SSL_set_connect_state(impl->ssl);
        }
        return TlsStream{std::move(impl)};
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded};
    }
}

SocketIoResult TlsStream::handshake() noexcept {
    if (!impl_)
        return {0, false, false, {ErrorCode::engine_stopped}};
    if (impl_->complete)
        return {};
    ERR_clear_error();
    const auto result = SSL_do_handshake(impl_->ssl);
    if (result != 1)
        return impl_->result(result, impl_->handshake_read, impl_->handshake_write);
    impl_->complete = true;
    impl_->handshake_read = impl_->handshake_write = false;
    return {};
}

SocketIoResult TlsStream::read(std::span<char> output) noexcept {
    if (!impl_ || !impl_->complete)
        return {0, false, false, {ErrorCode::not_ready}};
    if (output.empty())
        return {};
    ERR_clear_error();
    std::size_t size{};
    const auto result = SSL_read_ex(impl_->ssl, output.data(), output.size(), &size);
    if (result != 1)
        return impl_->result(result, impl_->read_read, impl_->read_write);
    impl_->read_read = impl_->read_write = false;
    return {size, false, false, {}};
}

SocketIoResult TlsStream::write(std::span<const char> input) noexcept {
    if (!impl_ || !impl_->complete)
        return {0, false, false, {ErrorCode::not_ready}};
    if (input.empty())
        return {};
    if (impl_->retry_size == 0) {
        impl_->retry_size = std::min(input.size(), impl_->retry.size());
        std::copy_n(input.data(), impl_->retry_size, impl_->retry.data());
    } else if (input.size() < impl_->retry_size ||
               std::memcmp(input.data(), impl_->retry.data(), impl_->retry_size) != 0)
        return {0, false, false, {ErrorCode::invalid_value}};
    ERR_clear_error();
    std::size_t size{};
    const auto result = SSL_write_ex(impl_->ssl, impl_->retry.data(), impl_->retry_size, &size);
    if (result != 1)
        return impl_->result(result, impl_->write_read, impl_->write_write);
    impl_->write_read = impl_->write_write = false;
    OPENSSL_cleanse(impl_->retry.data(), impl_->retry_size);
    impl_->retry_size = 0;
    return {size, false, false, {}};
}

bool TlsStream::handshake_complete() const noexcept {
    return impl_ && impl_->complete;
}
bool TlsStream::wants_read() const noexcept {
    return impl_ && (impl_->handshake_read || impl_->read_read || impl_->write_read);
}
bool TlsStream::wants_write() const noexcept {
    return impl_ && (impl_->handshake_write || impl_->read_write || impl_->write_write);
}
bool TlsStream::read_wants_write() const noexcept {
    return impl_ && impl_->read_write;
}
bool TlsStream::write_wants_read() const noexcept {
    return impl_ && impl_->write_read;
}
std::size_t TlsStream::pending_plaintext() const noexcept {
    return impl_ && impl_->complete ? static_cast<std::size_t>(SSL_pending(impl_->ssl)) : 0;
}
std::size_t TlsStream::retry_plaintext_bytes() const noexcept {
    return impl_ ? impl_->retry_size : 0;
}
void TlsStream::close() noexcept {
    if (impl_ && impl_->complete) {
        ERR_clear_error();
        (void)SSL_shutdown(impl_->ssl);
    }
    impl_.reset();
}
bool TlsContext::valid() const noexcept {
    return impl_ && impl_->context != nullptr;
}

#else
struct TlsContext::Impl {};
struct TlsStream::Impl {};
Result<TlsContext> TlsContext::create(const TlsConfig &, bool) noexcept {
    return Status{ErrorCode::unsupported};
}
Result<TlsStream> TlsStream::create(const TlsContext &, int, std::string_view) noexcept {
    return Status{ErrorCode::unsupported};
}
SocketIoResult TlsStream::handshake() noexcept {
    return {0, false, false, {ErrorCode::unsupported}};
}
SocketIoResult TlsStream::read(std::span<char>) noexcept {
    return {0, false, false, {ErrorCode::unsupported}};
}
SocketIoResult TlsStream::write(std::span<const char>) noexcept {
    return {0, false, false, {ErrorCode::unsupported}};
}
bool TlsContext::valid() const noexcept {
    return false;
}
bool TlsStream::handshake_complete() const noexcept {
    return false;
}
bool TlsStream::wants_read() const noexcept {
    return false;
}
bool TlsStream::wants_write() const noexcept {
    return false;
}
bool TlsStream::read_wants_write() const noexcept {
    return false;
}
bool TlsStream::write_wants_read() const noexcept {
    return false;
}
std::size_t TlsStream::pending_plaintext() const noexcept {
    return 0;
}
std::size_t TlsStream::retry_plaintext_bytes() const noexcept {
    return 0;
}
void TlsStream::close() noexcept {
    impl_.reset();
}
#endif

TlsContext::TlsContext(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
TlsContext::TlsContext(TlsContext &&) noexcept = default;
TlsContext &TlsContext::operator=(TlsContext &&) noexcept = default;
TlsContext::~TlsContext() = default;
TlsStream::TlsStream() noexcept = default;
TlsStream::TlsStream(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
TlsStream::TlsStream(TlsStream &&) noexcept = default;
TlsStream &TlsStream::operator=(TlsStream &&) noexcept = default;
TlsStream::~TlsStream() = default;

} // namespace nimblefix::detail
