#include <nimblefix/engine.hpp>
#include <nimblefix/message.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nf = nimblefix;

namespace {

using Clock = std::chrono::steady_clock;

struct DirectoryGuard {
    std::filesystem::path path;
    ~DirectoryGuard() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

struct EngineGuard {
    nf::Engine<>* engine{};
    std::thread* thread{};
    std::array<int*, 3> sockets{};
    ~EngineGuard() {
        for (auto* socket : sockets) {
            if (socket != nullptr && *socket >= 0) {
                ::close(*socket);
                *socket = -1;
            }
        }
        if (engine != nullptr) engine->request_stop();
        if (thread != nullptr && thread->joinable()) thread->join();
    }
};

std::string make_frame(std::string_view type, std::uint64_t sequence,
                       std::string_view sender, std::string_view target,
                       std::string_view order_id = "ORDER-1") {
    std::array<char, 1024> output{};
    const auto sending_time = std::string_view{"20261026-12:00:00.000"};
    if (type == "A") {
        auto logon = nf::message<"A">().set<98>(0).set<108>(30);
        const auto encoded = nf::encode_fix44(logon,
            {sender, target, sequence, sending_time}, output);
        if (!encoded.ok()) return {};
        return {output.data(), encoded.value()};
    }
    if (type == "D") {
        auto order = nf::message<"D">().set<11>(order_id).set<55>("AAPL");
        const auto encoded = nf::encode_fix44(order,
            {sender, target, sequence, sending_time}, output);
        if (!encoded.ok()) return {};
        return {output.data(), encoded.value()};
    }
    return {};
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

bool send_all(int fd, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool write_fragmented(int fd, std::string_view bytes, std::size_t split) {
    if (split >= bytes.size() || !send_all(fd, bytes.substr(0, split))) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return send_all(fd, bytes.substr(split));
}

bool receive_frame(int fd, std::string& buffer, std::string& output,
                   std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto deadline = Clock::now() + timeout;
    std::array<char, 4096> chunk{};
    while (Clock::now() < deadline) {
        nf::FrameView frame{};
        const auto inspected = nf::inspect_frame(buffer, frame);
        if (inspected.ok()) {
            output.assign(frame.bytes);
            buffer.erase(0, frame.consumed);
            return true;
        }
        if (inspected.code != nf::ErrorCode::need_more) return false;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        pollfd descriptor{fd, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, static_cast<int>(std::max<std::int64_t>(1, remaining.count())));
        if (ready <= 0) return false;
        const auto count = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (count <= 0) return false;
        buffer.append(chunk.data(), static_cast<std::size_t>(count));
    }
    return false;
}

bool peer_closed(int fd) {
    pollfd descriptor{fd, static_cast<short>(POLLIN | POLLHUP), 0};
    const int ready = ::poll(&descriptor, 1, 1000);
    if (ready <= 0) return false;
    char byte{};
    const auto count = ::recv(fd, &byte, 1, MSG_DONTWAIT);
    return count == 0 || ((descriptor.revents & POLLHUP) != 0 && count < 0);
}

bool frame_type(std::string_view frame, std::string_view expected) {
    const auto marker = std::string{"35="} + std::string{expected} + '\x01';
    return frame.find(marker) != std::string_view::npos;
}

bool test_throwing_error_observer(const std::filesystem::path& directory) {
    nf::EngineConfig config;
    config.storage.directory = directory;
    config.max_sessions = 2;
    auto created = nf::Engine<>::create(std::move(config));
    if (!created.ok()) return false;
    auto engine = std::move(created).value();
    std::mutex mutex;
    std::condition_variable changed;
    bool observed = false;
    if (!engine.on_message([](nf::SessionContext&, const nf::FlatMessageView&) {
            return nf::Status{nf::ErrorCode::callback_failed};
        }).ok()) return false;
    if (!engine.on_error([&](const nf::ErrorEvent&) {
            {
                std::lock_guard lock(mutex);
                observed = true;
            }
            changed.notify_all();
            throw std::runtime_error("error observer test");
        }).ok()) return false;
    if (!engine.add_listener({.name = "loopback", .endpoint = {"127.0.0.1", 0}}).ok() ||
        !engine.add_acceptor({.name = "known", .listener = "loopback",
            .local_comp_id = "SELL", .remote_comp_id = "BUY"}).ok()) return false;

    nf::Status run_status{};
    std::thread engine_thread([&] { run_status = engine.run(); });
    int client = -1;
    EngineGuard guard{&engine, &engine_thread, {&client, nullptr, nullptr}};
    nf::Result<nf::TcpEndpoint> endpoint{nf::Status{nf::ErrorCode::need_more}};
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < deadline) {
        endpoint = engine.listener_endpoint("loopback");
        if (endpoint.ok()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!endpoint.ok()) return false;
    client = connect_loopback(endpoint.value().port);
    if (client < 0) return false;
    const auto logon = make_frame("A", 1, "BUY", "SELL");
    if (!send_all(client, logon)) return false;
    std::string buffer, frame;
    if (!receive_frame(client, buffer, frame) || !frame_type(frame, "A")) return false;
    const auto order = make_frame("D", 2, "BUY", "SELL");
    if (!send_all(client, order)) return false;
    {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(2), [&] { return observed; })) return false;
    }
    engine.request_stop();
    engine_thread.join();
    return run_status.code == nf::ErrorCode::callback_failed;
}

bool test_shutdown_during_existing_logout(const std::filesystem::path& directory, bool acknowledge,
    bool close_without_ack = false) {
    nf::EngineConfig config;
    config.storage.directory = directory;
    config.logout_timeout_ms = 500;
    auto created = nf::Engine<>::create(config);
    if (!created.ok()) return false;
    auto engine = std::move(created).value();
    unsigned errors = 0;
    if (!engine.on_message([](auto&, const auto&) { return nf::Status::success(); }).ok() ||
        !engine.on_error([&](const auto&) { ++errors; }).ok() ||
        !engine.on_session_active([&](nf::SessionContext& context) {
            const auto logout = context.logout("already stopping");
            engine.request_stop();
            return logout;
        }).ok()) return false;
    if (!engine.add_listener({.name="stop", .endpoint={"127.0.0.1", 0}}).ok() ||
        !engine.add_acceptor({.name="peer", .listener="stop", .local_comp_id="SELL",
            .remote_comp_id="BUY"}).ok()) return false;
    nf::Status run_status;
    std::thread owner([&] { run_status = engine.run(); });
    int client = -1;
    EngineGuard guard{&engine, &owner, {&client, nullptr, nullptr}};
    nf::Result<nf::TcpEndpoint> endpoint{nf::Status{nf::ErrorCode::need_more}};
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!endpoint.ok() && Clock::now() < deadline) {
        endpoint = engine.listener_endpoint("stop");
        if (!endpoint.ok()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!endpoint.ok()) return false;
    client = connect_loopback(endpoint.value().port);
    if (client < 0 || !send_all(client, make_frame("A", 1, "BUY", "SELL"))) return false;
    std::string buffer, frame;
    if (!receive_frame(client, buffer, frame) || !frame_type(frame, "A") ||
        !receive_frame(client, buffer, frame) || !frame_type(frame, "5")) return false;
    if (acknowledge) {
        std::array<char, 256> reply;
        const auto logout = nf::message<"5">();
        const auto encoded = nf::encode_fix44(logout,
            {"BUY", "SELL", 2, "20261027-00:00:00.000"}, reply);
        if (!encoded.ok() || !send_all(client, {reply.data(), encoded.value()})) return false;
    } else if (close_without_ack) {
        ::close(client);
        client = -1;
    }
    owner.join();
    if (close_without_ack) return errors == 1 &&
        (run_status.code == nf::ErrorCode::connection_closed || run_status.code == nf::ErrorCode::io_error);
    return acknowledge ? run_status.ok() && errors == 0
        : run_status.code == nf::ErrorCode::logout_timeout && errors == 1;
}

bool test_burst_before_eof(const std::filesystem::path& directory, bool half_close) {
    nf::EngineConfig config;
    config.storage.directory = directory;
    config.logout_timeout_ms = 100;
    auto created = nf::Engine<>::create(config);
    if (!created.ok()) return false;
    auto engine = std::move(created).value();
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t delivered = 0;
    if (!engine.on_message([&](auto&, const auto& message) {
            const auto identifier = message.find_unique(11);
            if (!identifier.ok() || identifier.value().value != "BEFORE-FIN")
                return nf::Status{nf::ErrorCode::invalid_value, 11};
            std::lock_guard lock(mutex);
            ++delivered;
            changed.notify_all();
            return nf::Status::success();
        }).ok()) return false;
    if (!engine.add_listener({.name="eof", .endpoint={"127.0.0.1", 0}}).ok() ||
        !engine.add_acceptor({.name="peer", .listener="eof", .local_comp_id="SELL",
            .remote_comp_id="BUY"}).ok()) return false;
    nf::Status run_status;
    std::thread owner([&] { run_status = engine.run(); });
    int client = -1;
    EngineGuard guard{&engine, &owner, {&client, nullptr, nullptr}};
    nf::Result<nf::TcpEndpoint> endpoint{nf::Status{nf::ErrorCode::need_more}};
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!endpoint.ok() && Clock::now() < deadline) {
        endpoint = engine.listener_endpoint("eof");
        if (!endpoint.ok()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!endpoint.ok()) return false;
    client = connect_loopback(endpoint.value().port);
    if (client < 0 || !send_all(client, make_frame("A", 1, "BUY", "SELL"))) return false;
    std::string buffer, frame;
    if (!receive_frame(client, buffer, frame) || !frame_type(frame, "A")) return false;
    std::string burst;
    for (std::uint64_t sequence = 2; sequence < 1202; ++sequence)
        burst.append(make_frame("D", sequence, "BUY", "SELL", "BEFORE-FIN"));
    if (burst.size() <= 64U * 1024U || !send_all(client, burst)) return false;
    if (half_close) {
        if (::shutdown(client, SHUT_WR) != 0) return false;
    } else {
        ::close(client);
        client = -1;
    }
    {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return delivered == 1200; }))
            return false;
    }
    std::filesystem::path journal_path;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
        if (entry.path().extension() == ".nfxj") journal_path = entry.path();
    if (journal_path.empty()) return false;
    bool drained = false;
    const auto drain_deadline = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < drain_deadline) {
        auto reopened = nf::Journal::open({.path=journal_path.string(), .session_key="FIX.4.4|4:SELL|3:BUY"});
        if (reopened.ok()) {
            if (reopened.value().next_inbound_seq() != 1202) return false;
            drained = true;
            break;
        }
        if (reopened.status().code != nf::ErrorCode::store_locked) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!drained) return false;
    engine.request_stop();
    owner.join();
    return run_status.ok();
}

} // namespace

int main() {
    char directory_template[] = "/tmp/nimblefix-network-XXXXXX";
    char* directory_name = ::mkdtemp(directory_template);
    if (directory_name == nullptr) return 1;
    const std::filesystem::path directory{directory_name};
    DirectoryGuard directory_guard{directory};

    nf::EngineConfig invalid_limits;
    invalid_limits.storage.directory = directory / "invalid-limits";
    invalid_limits.max_pending_bytes = 2 * nf::default_max_frame_bytes;
    auto invalid_engine = nf::Engine<>::create(std::move(invalid_limits));
    if (!invalid_engine.ok() ||
        invalid_engine.value().validate().code != nf::ErrorCode::invalid_config)
        return 2;

    nf::EngineConfig config;
    config.storage.directory = directory;
    config.max_pending_connections = 8;
    config.max_sessions = 4;
    config.logout_timeout_ms = 500;
    auto created = nf::Engine<>::create(std::move(config));
    if (!created.ok()) return 2;
    auto engine = std::move(created).value();
    const auto dns_listener = engine.add_listener({
        .name = "hostname-listener", .endpoint = {"localhost", 0}});
    const auto dns_initiator = engine.add_initiator({
        .name = "hostname-initiator", .sender_comp_id = "DNS-SENDER",
        .target_comp_id = "DNS-TARGET", .endpoint = {"localhost", 9876}});
    if (dns_listener.ok() || dns_listener.status().code != nf::ErrorCode::invalid_config ||
        dns_initiator.ok() || dns_initiator.status().code != nf::ErrorCode::invalid_config)
        return 2;

    std::mutex mutex;
    std::condition_variable changed;
    std::size_t active_count = 0;
    std::size_t order_count = 0;
    std::size_t burst_count = 0;
    std::size_t observer_errors = 0;
    std::array<nf::Status, 8> observer_statuses{};
    std::function<nf::Status(nf::SessionContext&, const nf::FlatMessageView&)> no_message;
    std::function<nf::Status(nf::SessionContext&)> no_active;
    std::function<void(const nf::ErrorEvent&)> no_error;
    if (engine.on_message(no_message).code != nf::ErrorCode::invalid_config ||
        engine.on_session_active(no_active).code != nf::ErrorCode::invalid_config ||
        engine.on_error(no_error).code != nf::ErrorCode::invalid_config)
        return 3;
    auto active_callback = engine.on_session_active([&](nf::SessionContext&) {
        std::lock_guard lock(mutex);
        ++active_count;
        changed.notify_all();
        return nf::Status::success();
    });
    auto message_callback = engine.on_message(
        [&](nf::SessionContext& context, const nf::FlatMessageView& message) {
            if (!frame_type(message.frame(), "D")) return nf::Status::success();
            const auto cl_ord_id = message.find_unique(11);
            if (!cl_ord_id.ok()) return cl_ord_id.status();
            if (cl_ord_id.value().value == "BURST") {
                {
                    std::lock_guard lock(mutex);
                    ++burst_count;
                    ++order_count;
                }
                changed.notify_all();
                return nf::Status::success();
            }
            if (cl_ord_id.value().value == "FAIL")
                return nf::Status{nf::ErrorCode::callback_failed};
            if (cl_ord_id.value().value == "SLOW") {
                const std::string large_text(40'000, 'X');
                for (int i = 0; i < 80; ++i) {
                    auto response = nf::message<"8">({.max_fields = 64, .max_value_bytes = 50'000})
                        .set<11>("SLOW")
                        .set<17>("EXEC-SLOW")
                        .set<58>(large_text);
                    const auto sent = context.send(response);
                    if (!sent.ok()) return sent.status();
                }
            } else {
                auto response = nf::message<"8">()
                    .set<11>(cl_ord_id.value().value)
                    .set<17>("EXEC-1");
                const auto sent = context.send(response);
                if (!sent.ok()) return sent.status();
            }
            {
                std::lock_guard lock(mutex);
                ++order_count;
            }
            changed.notify_all();
            return nf::Status::success();
        });
    auto error_callback = engine.on_error([&](const nf::ErrorEvent& error) {
        std::lock_guard lock(mutex);
        if (observer_errors < observer_statuses.size())
            observer_statuses[observer_errors] = error.status;
        ++observer_errors;
    });
    if (!active_callback.ok() || !message_callback.ok() || !error_callback.ok()) return 3;
    if (engine.on_message([](nf::SessionContext&, const nf::FlatMessageView&) {
            return nf::Status::success();
        }).code != nf::ErrorCode::duplicate_field ||
        engine.on_session_active([](nf::SessionContext&) {
            return nf::Status::success();
        }).code != nf::ErrorCode::duplicate_field ||
        engine.on_error([](const nf::ErrorEvent&) {}).code != nf::ErrorCode::duplicate_field)
        return 3;
    auto listener = engine.add_listener({.name = "loopback", .endpoint = {"127.0.0.1", 0}});
    if (!listener.ok()) return 4;
    auto acceptor = engine.add_acceptor({
        .name = "known-client",
        .listener = "loopback",
        .local_comp_id = "SELL",
        .remote_comp_id = "BUY",
    });
    if (!acceptor.ok() || !engine.validate().ok()) return 5;
    const auto duplicate_identity = engine.add_acceptor({
        .name = "same-peer-different-name",
        .listener = "loopback",
        .local_comp_id = "SELL",
        .remote_comp_id = "BUY",
    });
    if (duplicate_identity.ok() || duplicate_identity.status().code != nf::ErrorCode::invalid_config)
        return 5;
    auto second_acceptor = engine.add_acceptor({
        .name = "fast-client",
        .listener = "loopback",
        .local_comp_id = "SELL",
        .remote_comp_id = "BUY2",
    });
    if (!second_acceptor.ok() || !engine.validate().ok()) return 5;
    const auto third_acceptor = engine.add_acceptor({
        .name = "failing-client",
        .listener = "loopback",
        .local_comp_id = "SELL",
        .remote_comp_id = "BUY3",
    });
    if (!third_acceptor.ok() || !engine.validate().ok()) return 5;
    const auto before_run = engine.listener_endpoint("loopback");
    if (before_run.ok() || before_run.status().code != nf::ErrorCode::need_more) return 6;

    nf::Status run_status{};
    std::thread engine_thread([&] { run_status = engine.run(); });
    int bad_client = -1;
    int client = -1;
    int duplicate = -1;
    EngineGuard engine_guard{&engine, &engine_thread, {&bad_client, &client, &duplicate}};
    nf::Result<nf::TcpEndpoint> endpoint{nf::Status{nf::ErrorCode::need_more}};
    const auto bind_deadline = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < bind_deadline) {
        endpoint = engine.listener_endpoint("loopback");
        if (endpoint.ok()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!endpoint.ok()) {
        return 7;
    }

    bad_client = connect_loopback(endpoint.value().port);
    if (bad_client < 0) {
        return 8;
    }
    const auto unknown_logon = make_frame("A", 1, "UNKNOWN", "SELL");
    if (unknown_logon.empty() || !send_all(bad_client, unknown_logon) || !peer_closed(bad_client)) {
        return 9;
    }
    ::close(bad_client);
    bad_client = connect_loopback(endpoint.value().port); // A silent pending Logon must not block peers.
    if (bad_client < 0) return 8;

    client = connect_loopback(endpoint.value().port);
    if (client < 0) {
        return 10;
    }
    int receive_buffer_size = 1024;
    (void)::setsockopt(client, SOL_SOCKET, SO_RCVBUF,
        &receive_buffer_size, sizeof(receive_buffer_size));
    const auto logon = make_frame("A", 1, "BUY", "SELL");
    if (logon.empty() || !send_all(client, logon)) return 11;
    std::string incoming;
    std::string response;
    if (!receive_frame(client, incoming, response) || !frame_type(response, "A")) return 12;
    {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(2), [&] { return active_count == 1; })) return 13;
    }

    // A second connection with the same configured identities is rejected;
    // it cannot open a second writer for the existing session journal.
    duplicate = connect_loopback(endpoint.value().port);
    if (duplicate < 0 || !send_all(duplicate, logon) || !peer_closed(duplicate)) return 14;
    ::close(duplicate);
    duplicate = -1;

    const auto order1 = make_frame("D", 2, "BUY", "SELL", "ORDER-1");
    const auto order2 = make_frame("D", 3, "BUY", "SELL", "ORDER-2");
    if (order1.empty() || order2.empty()) return 15;
    std::string combined = order1 + order2;
    if (!write_fragmented(client, combined, order1.size() / 2)) return 16;
    {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(3), [&] { return order_count == 2; })) return 17;
    }
    std::string first_report, second_report;
    if (!receive_frame(client, incoming, first_report) || !frame_type(first_report, "8") ||
        !receive_frame(client, incoming, second_report) || !frame_type(second_report, "8")) return 18;

    // Keep a peer with an incomplete Logon pending while the second allowed
    // identity connects. Pending sockets cannot block the owner loop.
    int fast_client = connect_loopback(endpoint.value().port);
    if (fast_client < 0) return 23;
    duplicate = fast_client;
    const auto fast_logon = make_frame("A", 1, "BUY2", "SELL");
    if (!send_all(fast_client, fast_logon)) return 24;
    std::string fast_buffer, fast_response;
    if (!receive_frame(fast_client, fast_buffer, fast_response) || !frame_type(fast_response, "A")) return 25;
    {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(2), [&] { return active_count == 2; })) return 26;
    }
    const auto fast_order = make_frame("D", 2, "BUY2", "SELL", "FAST");
    if (!send_all(fast_client, fast_order)) return 27;
    {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(3), [&] { return order_count == 3; })) return 28;
    }
    std::string fast_report;
    if (!receive_frame(fast_client, fast_buffer, fast_report) || !frame_type(fast_report, "8")) return 29;

    // More than one receive budget and well over sixteen frames arrive in a
    // single burst while the fast peer keeps receiving service.
    std::string burst;
    for (std::uint64_t seq = 4; seq < 1204; ++seq) {
        const auto frame = make_frame("D", seq, "BUY", "SELL", "BURST");
        if (frame.empty()) return 30;
        burst.append(frame);
    }
    if (burst.size() <= 64U * 1024U || !send_all(client, burst)) return 31;
    const auto fast_during_burst = make_frame("D", 3, "BUY2", "SELL", "FAST");
    if (!send_all(fast_client, fast_during_burst)) return 32;
    {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(5), [&] {
                return burst_count == 1200 && order_count == 1204;
            })) return 33;
    }
    if (!receive_frame(fast_client, fast_buffer, fast_report) || !frame_type(fast_report, "8")) return 34;

    // Stop reading the first peer after a large bounded send queue fills. A
    // second active peer must continue to receive messages through EAGAIN.
    const auto slow_order = make_frame("D", 1204, "BUY", "SELL", "SLOW");
    if (!send_all(client, slow_order)) return 35;
    {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(3), [&] { return order_count == 1205; })) return 36;
    }
    const auto fast_after_eagain = make_frame("D", 4, "BUY2", "SELL", "FAST");
    if (!send_all(fast_client, fast_after_eagain)) return 37;
    {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(3), [&] { return order_count == 1206; })) return 38;
    }
    if (!receive_frame(fast_client, fast_buffer, fast_report) || !frame_type(fast_report, "8")) return 39;

    // Drop a half frame, then reconnect the same identity from a new source
    // port. The incomplete old bytes must not advance MsgSeqNum or reach the
    // callback; the next complete order starts after the new Logon.
    const auto partial_order = make_frame("D", 5, "BUY2", "SELL", "PARTIAL");
    if (partial_order.size() < 2 ||
        !send_all(fast_client, std::string_view{partial_order}.substr(0, partial_order.size() / 2)))
        return 46;
    ::close(fast_client);
    duplicate = -1;
    const auto reconnect_logon = make_frame("A", 5, "BUY2", "SELL");
    bool reconnected = false;
    for (int attempt = 0; attempt < 20 && !reconnected; ++attempt) {
        duplicate = connect_loopback(endpoint.value().port);
        if (duplicate < 0) return 47;
        fast_buffer.clear();
        if (send_all(duplicate, reconnect_logon) &&
            receive_frame(duplicate, fast_buffer, fast_response,
                std::chrono::milliseconds(300)) && frame_type(fast_response, "A")) {
            reconnected = true;
        } else {
            ::close(duplicate);
            duplicate = -1;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    if (!reconnected) return 48;
    const auto after_reconnect = make_frame("D", 6, "BUY2", "SELL", "AFTER-RECONNECT");
    if (!send_all(duplicate, after_reconnect)) return 49;
    {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(3), [&] { return order_count == 1207; })) return 50;
    }
    if (!receive_frame(duplicate, fast_buffer, fast_report) || !frame_type(fast_report, "8")) return 51;
    ::close(duplicate);
    duplicate = -1;

    // Close the intentionally unread slow socket. Its reset is reported but
    // must leave the two other configured identities available.
    ::close(client);
    client = -1;
    {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(2), [&] {
                return observer_errors >= 1;
            })) return 52;
    }
    // A user callback failure closes only that session. The engine continues
    // to serve the other configured identities, and reports the failure once.
    ::close(bad_client);
    bad_client = connect_loopback(endpoint.value().port);
    if (bad_client < 0) return 40;
    const auto failing_logon = make_frame("A", 1, "BUY3", "SELL");
    if (!send_all(bad_client, failing_logon)) return 41;
    std::string failing_buffer, failing_response;
    if (!receive_frame(bad_client, failing_buffer, failing_response) ||
        !frame_type(failing_response, "A")) return 42;
    {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(2), [&] { return active_count == 4; })) return 43;
    }
    const auto failing_order = make_frame("D", 2, "BUY3", "SELL", "FAIL");
    if (!send_all(bad_client, failing_order) || !peer_closed(bad_client)) return 44;
    {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(2), [&] { return observer_errors == 2; })) return 45;
    }
    ::close(bad_client);
    bad_client = -1;

    engine.request_stop();
    engine_thread.join();
    if (!run_status.ok()) return 19;
    {
        std::lock_guard lock(mutex);
        bool saw_io_error = false;
        bool saw_callback_error = false;
        for (std::size_t i = 0; i < std::min(observer_errors, observer_statuses.size()); ++i) {
            saw_io_error = saw_io_error || observer_statuses[i].code == nf::ErrorCode::io_error;
            saw_callback_error = saw_callback_error ||
                observer_statuses[i].code == nf::ErrorCode::callback_failed;
        }
        if (active_count != 4 || order_count != 1207 || burst_count != 1200 ||
            observer_errors != 2 || !saw_io_error || !saw_callback_error) {
            std::cerr << "network-test counters: active=" << active_count
                      << " orders=" << order_count << " burst=" << burst_count
                      << " errors=" << observer_errors << '\n';
            for (std::size_t i = 0; i < std::min(observer_errors, observer_statuses.size()); ++i)
                std::cerr << "network-test error " << i << ": "
                          << nf::ErrorEvent{"session", observer_statuses[i]}.format() << '\n';
            return 20;
        }
    }
    if (!test_throwing_error_observer(directory / "throwing-observer")) return 21;
    if (!test_shutdown_during_existing_logout(directory / "existing-logout", true)) return 53;
    if (!test_shutdown_during_existing_logout(directory / "logout-timeout", false)) return 56;
    if (!test_shutdown_during_existing_logout(directory / "logout-eof", false, true)) return 57;
    if (!test_burst_before_eof(directory / "half-close", true)) return 54;
    if (!test_burst_before_eof(directory / "full-close", false)) return 55;
    return 0;
}
