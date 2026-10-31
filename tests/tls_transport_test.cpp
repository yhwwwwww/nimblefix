#include "../src/transport/tls.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <sys/socket.h>

namespace nf = nimblefix;
namespace transport = nimblefix::detail;
#define CHECK(value)                                                                                         \
    do {                                                                                                     \
        if (!(value)) {                                                                                      \
            std::cerr << "TLS transport: " << __LINE__ << ": " #value "\n";                                  \
            std::exit(1);                                                                                    \
        }                                                                                                    \
    } while (false)

struct Pair {
    transport::TcpStream first;
    transport::TcpStream second;
    Pair() {
        int descriptors[2];
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, descriptors) == 0);
        first = transport::TcpStream{descriptors[0]};
        second = transport::TcpStream{descriptors[1]};
        const int size = 4096;
        CHECK(::setsockopt(first.native_handle(), SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
    }
};

nf::Status handshake(transport::TlsStream &client, transport::TlsStream &server) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!client.handshake_complete()) {
            const auto result = client.handshake();
            if (!result.status.ok())
                return result.status;
        }
        if (!server.handshake_complete()) {
            const auto result = server.handshake();
            if (!result.status.ok())
                return result.status;
        }
        if (client.handshake_complete() && server.handshake_complete())
            return nf::Status::success();
    }
    return {nf::ErrorCode::tls_handshake_timeout};
}

int main(int argc, char **argv) {
    nf::TlsConfig server_config;
    server_config.enabled = true;
    if (!nf::tls_available()) {
        CHECK(transport::TlsContext::create(server_config, true).status().code == nf::ErrorCode::unsupported);
        return 0;
    }
    CHECK(argc == 4);
    server_config.ca_file = argv[1];
    server_config.certificate_file = argv[2];
    server_config.private_key_file = argv[3];
    server_config.require_client_certificate = true;
    nf::TlsConfig client_config = server_config;
    client_config.require_client_certificate = false;
    auto server_created = transport::TlsContext::create(server_config, true);
    auto client_created = transport::TlsContext::create(client_config, false);
    CHECK(server_created.ok() && client_created.ok());
    auto server_context = std::move(server_created).value();
    auto client_context = std::move(client_created).value();
    Pair sockets;
    auto client_created_stream =
        transport::TlsStream::create(client_context, sockets.first.native_handle(), "localhost");
    auto server_created_stream = transport::TlsStream::create(server_context, sockets.second.native_handle());
    CHECK(client_created_stream.ok() && server_created_stream.ok());
    auto client = std::move(client_created_stream).value();
    auto server = std::move(server_created_stream).value();
    CHECK(handshake(client, server).ok());

    std::vector<char> input(128U * 1024U), output(input.size());
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<char>(i % 251);
    std::size_t sent = 0, received = 0;
    unsigned blocks = 0;
    for (unsigned i = 0; i < 100 && sent < input.size(); ++i) {
        const auto result = client.write(std::span{input}.subspan(sent));
        CHECK(result.status.ok());
        sent += result.bytes;
        if (result.would_block)
            ++blocks;
    }
    CHECK(blocks != 0 && client.retry_plaintext_bytes() <= 16'384);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received < input.size() && std::chrono::steady_clock::now() < deadline) {
        if (sent < input.size()) {
            const auto result = client.write(std::span{input}.subspan(sent));
            CHECK(result.status.ok());
            sent += result.bytes;
        }
        const auto result = server.read(
            std::span{output}.subspan(received, std::min<std::size_t>(113, output.size() - received)));
        CHECK(result.status.ok());
        received += result.bytes;
    }
    CHECK(sent == input.size() && received == input.size() && input == output);
    server.close();
    std::array<char, 32> scratch;
    bool closed = false;
    for (unsigned i = 0; i < 1000 && !closed; ++i) {
        const auto result = client.read(scratch);
        CHECK(result.status.ok());
        closed = result.peer_closed;
    }
    CHECK(closed);

    Pair wrong_sockets;
    auto wrong_client =
        transport::TlsStream::create(client_context, wrong_sockets.first.native_handle(), "wrong.invalid");
    auto wrong_server = transport::TlsStream::create(server_context, wrong_sockets.second.native_handle());
    CHECK(wrong_client.ok() && wrong_server.ok());
    CHECK(handshake(wrong_client.value(), wrong_server.value()).code == nf::ErrorCode::certificate_error);

    auto missing_client_cert = client_config;
    missing_client_cert.certificate_file.clear();
    missing_client_cert.private_key_file.clear();
    auto anonymous = transport::TlsContext::create(missing_client_cert, false);
    CHECK(anonymous.ok());
    Pair anonymous_sockets;
    auto anonymous_client =
        transport::TlsStream::create(anonymous.value(), anonymous_sockets.first.native_handle(), "localhost");
    auto rejecting_server =
        transport::TlsStream::create(server_context, anonymous_sockets.second.native_handle());
    CHECK(anonymous_client.ok() && rejecting_server.ok());
    CHECK(!handshake(anonymous_client.value(), rejecting_server.value()).ok());
    std::cout << "TLS verification, mTLS, bounded record retries and close_notify passed\n";
}
