#include "test_process_utils.hpp"
#include <nimblefix/engine.hpp>
#include <nimblefix/message.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nf = nimblefix;
using namespace std::chrono_literals;

namespace {

std::filesystem::path temp_directory(std::string_view prefix) {
    std::string pattern = "/tmp/nimblefix-engine-tls-" + std::string{prefix} + "-XXXXXX";
    std::vector<char> bytes(pattern.begin(), pattern.end());
    bytes.push_back('\0');
    const auto* path = ::mkdtemp(bytes.data());
    return path == nullptr ? std::filesystem::path{} : std::filesystem::path{path};
}

int connect_loopback(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

nf::TlsConfig tls_config(const std::filesystem::path& certificate,
    const std::filesystem::path& ca, const std::filesystem::path& key,
    bool client_certificate, std::string server_name = "localhost") {
    nf::TlsConfig tls;
    tls.enabled = true;
    tls.ca_file = ca;
    tls.certificate_file = certificate;
    tls.private_key_file = key;
    tls.server_name = std::move(server_name);
    tls.require_client_certificate = client_certificate;
    tls.handshake_timeout_ms = 1200;
    return tls;
}

int test_engine_to_engine_tls(const std::filesystem::path& root,
    const std::filesystem::path& certificate, const std::filesystem::path& ca,
    const std::filesystem::path& key) {
    auto server_created = nf::Engine<>::create({
        .storage = {.directory = root / "server"}, .logout_timeout_ms = 800});
    auto client_created = nf::Engine<>::create({
        .storage = {.directory = root / "client"}, .logout_timeout_ms = 800});
    if (!server_created.ok() || !client_created.ok()) return 1;
    auto server = std::move(server_created).value();
    auto client = std::move(client_created).value();

    std::atomic<bool> server_active{false}, client_active{false}, business_seen{false};
    std::atomic<unsigned> server_errors{0}, client_errors{0};
    if (!server.on_session_active([&](nf::SessionContext&) {
            server_active.store(true, std::memory_order_release);
            return nf::Status::success();
        }).ok() ||
        !client.on_session_active([&](nf::SessionContext&) {
            client_active.store(true, std::memory_order_release);
            return nf::Status::success();
        }).ok() ||
        !server.on_message([&](nf::SessionContext&, const nf::FlatMessageView& message) {
            const auto type = message.find_unique(35);
            if (type.ok() && type.value().value == "D")
                business_seen.store(true, std::memory_order_release);
            return nf::Status::success();
        }).ok() ||
        !server.on_error([&](const nf::ErrorEvent&) { server_errors.fetch_add(1); }).ok() ||
        !client.on_error([&](const nf::ErrorEvent&) { client_errors.fetch_add(1); }).ok()) return 2;

    nf::ListenerConfig listener{.name = "listener", .endpoint = {"127.0.0.1", 0}};
    listener.tls = tls_config(certificate, ca, key, true);
    if (!server.add_listener(std::move(listener)).ok() ||
        !server.add_acceptor({.name = "acceptor", .listener = "listener",
            .local_comp_id = "SERVER", .remote_comp_id = "CLIENT"}).ok() ||
        !server.start().ok()) return 3;
    const auto endpoint = server.listener_endpoint("listener");
    if (!endpoint.ok()) return 4;

    nf::InitiatorConfig initiator{.name = "initiator", .sender_comp_id = "CLIENT",
        .target_comp_id = "SERVER", .endpoint = {"127.0.0.1", endpoint.value().port}};
    initiator.tls = tls_config(certificate, ca, key, false);
    auto session = client.add_initiator(std::move(initiator));
    if (!session.ok()) return 5;
    auto sender = session.value().sender<>();
    nf::Status client_status{};
    std::thread client_owner([&] { client_status = client.run(); });

    const auto active_deadline = std::chrono::steady_clock::now() + 5s;
    while ((!server_active.load(std::memory_order_acquire) ||
            !client_active.load(std::memory_order_acquire)) &&
        std::chrono::steady_clock::now() < active_deadline) {
        const auto status = server.poll_once(10);
        if (!status.ok()) break;
    }
    if (!server_active.load(std::memory_order_acquire) ||
        !client_active.load(std::memory_order_acquire)) {
        server.request_stop();
        client.request_stop();
        while (server.running()) (void)server.poll_once(5);
        client_owner.join();
        return 6;
    }

    const auto accepted = sender.send(nf::message<"D">().set<11>("TLS-ORDER").set<55>("AAPL"));
    const auto business_deadline = std::chrono::steady_clock::now() + 3s;
    while (accepted.ok() && !business_seen.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < business_deadline) {
        const auto status = server.poll_once(10);
        if (!status.ok()) break;
    }
    if (!accepted.ok() || !business_seen.load(std::memory_order_acquire)) {
        std::cerr << "TLS business failure accepted=" << accepted.ok()
                  << " server_active=" << server_active.load()
                  << " client_active=" << client_active.load()
                  << " seen=" << business_seen.load() << '\n';
        server.request_stop();
        client.request_stop();
        while (server.running()) (void)server.poll_once(5);
        client_owner.join();
        return 7;
    }

    server.request_stop();
    client.request_stop();
    nf::Status server_status{};
    while (server.running()) {
        server_status = server.poll_once(10);
        if (!server_status.ok()) break;
    }
    client_owner.join();
    if (!server_status.ok() || !client_status.ok() || server_errors.load() != 0 ||
        client_errors.load() != 0) return 8;
    return 0;
}

int test_engine_certificate_rejection(const std::filesystem::path& root,
    const std::filesystem::path& certificate, const std::filesystem::path& ca,
    const std::filesystem::path& key) {
    auto server_created = nf::Engine<>::create({.storage = {.directory = root / "server"}});
    auto client_created = nf::Engine<>::create({.storage = {.directory = root / "client"}});
    if (!server_created.ok() || !client_created.ok()) return 10;
    auto server = std::move(server_created).value();
    auto client = std::move(client_created).value();
    std::atomic<bool> rejected{false}, active{false};
    if (!client.on_session_active([&](nf::SessionContext&) {
            active.store(true, std::memory_order_release);
            return nf::Status::success();
        }).ok() ||
        !client.on_error([&](const nf::ErrorEvent& event) {
            if (event.status.code == nf::ErrorCode::certificate_error)
                rejected.store(true, std::memory_order_release);
        }).ok()) return 11;
    nf::ListenerConfig listener{.name = "listener", .endpoint = {"127.0.0.1", 0}};
    listener.tls = tls_config(certificate, ca, key, false);
    if (!server.add_listener(std::move(listener)).ok() ||
        !server.add_acceptor({.name = "acceptor", .listener = "listener",
            .local_comp_id = "SERVER", .remote_comp_id = "CLIENT"}).ok() ||
        !server.start().ok()) return 12;
    const auto endpoint = server.listener_endpoint("listener");
    if (!endpoint.ok()) return 13;
    nf::InitiatorConfig initiator{.name = "initiator", .sender_comp_id = "CLIENT",
        .target_comp_id = "SERVER", .endpoint = {"127.0.0.1", endpoint.value().port}};
    initiator.tls = tls_config(certificate, ca, key, false, "wrong.invalid");
    if (!client.add_initiator(std::move(initiator)).ok()) return 14;
    std::thread client_owner([&] { (void)client.run(); });
    const auto rejection_deadline = std::chrono::steady_clock::now() + 4s;
    while (!rejected.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < rejection_deadline) {
        const auto status = server.poll_once(10);
        if (!status.ok()) break;
    }
    const bool got_rejection = rejected.load(std::memory_order_acquire);
    const bool did_not_activate = !active.load(std::memory_order_acquire);
    server.request_stop();
    client.request_stop();
    while (server.running()) (void)server.poll_once(10);
    client_owner.join();
    return got_rejection && did_not_activate ? 0 : 15;
}

int test_engine_handshake_timeout(const std::filesystem::path& root,
    const std::filesystem::path& certificate, const std::filesystem::path& ca,
    const std::filesystem::path& key) {
    auto created = nf::Engine<>::create({.storage = {.directory = root / "server"},
        .logout_timeout_ms = 300, .diagnostic_capacity = 8});
    if (!created.ok()) return 20;
    auto engine = std::move(created).value();
    std::atomic<bool> timed_out{false};
    if (!engine.on_error([&](const nf::ErrorEvent& event) {
            if (event.status.code == nf::ErrorCode::tls_handshake_timeout)
                timed_out.store(true, std::memory_order_release);
        }).ok()) return 21;
    nf::ListenerConfig listener{.name = "listener", .endpoint = {"127.0.0.1", 0}};
    listener.tls = tls_config(certificate, ca, key, false);
    listener.tls.handshake_timeout_ms = 120;
    if (!engine.add_listener(std::move(listener)).ok() || !engine.start().ok()) return 22;
    const auto endpoint = engine.listener_endpoint("listener");
    if (!endpoint.ok()) return 23;
    const int fd = connect_loopback(endpoint.value().port);
    if (fd < 0) return 24;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    nf::Status final{};
    while (!timed_out.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < deadline) {
        final = engine.poll_once(10);
        if (!final.ok()) break;
    }
    const bool observed = timed_out.load(std::memory_order_acquire);
    ::close(fd);
    engine.request_stop();
    nf::Status stopped{};
    while (engine.running()) {
        stopped = engine.poll_once(10);
        if (!stopped.ok()) break;
    }
    return observed ? 0 : 25;
}

} // namespace

int main(int argc, char** argv) {
    if (!nf::tls_available()) return 0;
    std::optional<nimblefix::test_util::TempDir> auto_dir;
    std::filesystem::path certificate, ca, key;
    if (argc == 4) {
        certificate = argv[1];
        ca = argv[2];
        key = argv[3];
    } else if (argc == 1) {
        auto_dir.emplace("nimblefix-engine-tls-");
        if (!nimblefix::test_util::generate_self_signed_cert(auto_dir->path, certificate, key)) {
            std::cerr << "failed to generate self-signed TLS certificate\n";
            return 1;
        }
        ca = certificate;
    } else {
        std::cerr << "usage: engine_tls_probe [CERTIFICATE CA_FILE PRIVATE_KEY]\n";
        return 2;
    }
    const auto root = temp_directory("probe");
    if (root.empty()) return 1;
    int result = test_engine_to_engine_tls(root / "exchange", certificate, ca, key);
    if (result == 0) result = test_engine_certificate_rejection(root / "reject", certificate, ca, key);
    if (result == 0) result = test_engine_handshake_timeout(root / "timeout", certificate, ca, key);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    if (result == 0) std::cout << "Engine TLS exchange, certificate rejection and handshake timeout passed\n";
    return result;
}
