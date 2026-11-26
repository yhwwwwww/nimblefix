#include <nimblefix/engine.hpp>
#include <nimblefix/journal.hpp>
#include <nimblefix/message.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nf = nimblefix;
using namespace std::chrono_literals;

namespace {

std::string frame(std::string_view type, std::uint64_t seq,
    std::string_view sender, std::string_view target) {
    std::array<char, 2048> bytes{};
    std::array<char,32> stamp{};
    const auto now = std::time(nullptr);
    std::tm utc{};
    ::gmtime_r(&now, &utc);
    ::strftime(stamp.data(), stamp.size(), "%Y%m%d-%H:%M:%S", &utc);
    const nf::SessionHeader header{sender, target, seq, stamp.data()};
    nf::Result<std::size_t> encoded{nf::Status{nf::ErrorCode::invalid_value}};
    if (type == "A") {
        auto message = nf::message<"A">().set<98>(0).set<108>(30);
        encoded = nf::encode_fix44(message, header, bytes);
    } else if (type == "D") {
        auto message = nf::message<"D">().set<11>("INBOUND").set<55>("AAPL");
        encoded = nf::encode_fix44(message, header, bytes);
    } else if (type == "1") {
        auto message = nf::message<"1">().set<112>("KEEPALIVE");
        encoded = nf::encode_fix44(message, header, bytes);
    } else if (type == "5") {
        auto message = nf::message<"5">();
        encoded = nf::encode_fix44(message, header, bytes);
    }
    if (!encoded.ok()) return {};
    return {bytes.data(), encoded.value()};
}

bool send_all(int fd, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
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

bool receive_frame(int fd, std::string& buffered, std::string& result,
    std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 4096> chunk{};
    while (std::chrono::steady_clock::now() < deadline) {
        nf::FrameView view{};
        const auto inspected = nf::inspect_frame(buffered, view);
        if (inspected.ok()) {
            result.assign(view.bytes);
            buffered.erase(0, view.consumed);
            return true;
        }
        if (inspected.code != nf::ErrorCode::need_more) return false;
        pollfd descriptor{fd, POLLIN, 0};
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (::poll(&descriptor, 1, static_cast<int>(std::max<std::int64_t>(1, left.count()))) <= 0)
            return false;
        const auto count = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (count <= 0) return false;
        buffered.append(chunk.data(), static_cast<std::size_t>(count));
    }
    return false;
}

bool is_type(std::string_view bytes, std::string_view type) {
    const auto marker = std::string{"35="} + std::string{type} + '\x01';
    return bytes.find(marker) != std::string_view::npos;
}

bool test_graceful_logout_during_callback_return() {
    char path[] = "/tmp/nimblefix-queued-logout-race-XXXXXX";
    const auto* directory = ::mkdtemp(path);
    if (directory == nullptr) return false;
    const std::filesystem::path root{directory};
    int client = -1;
    std::thread owner;
    auto created = nf::Engine<nf::FlatSchema, nf::Queued>::create({
        .storage = {.directory = root / "journal"}, .logout_timeout_ms = 1000});
    if (!created.ok()) {
        std::filesystem::remove_all(root);
        return false;
    }
    auto engine = std::move(created).value();

    std::mutex mutex;
    std::condition_variable cv;
    bool release_return = false;
    std::atomic<unsigned> error_count{};

    if (!engine.on_message([&](nf::SessionContext& context, const nf::FlatMessageView& message) {
            const auto type = message.find_unique(35);
            if (!type.ok() || type.value().value != "D") return nf::Status::success();
            auto reply = nf::message<"8">().set<11>("INBOUND").set<17>("EXEC-RACE");
            const auto sent = context.send(reply);
            if (!sent.ok()) return sent.status();
            // Deliberately hold the worker thread AFTER context.send() returns so
            // the client receives 35=8, sends Logout (35=5), receives Logout (35=5),
            // closes its socket, and triggers request_stop() before this callback returns.
            std::unique_lock lock(mutex);
            if (!cv.wait_for(lock, 5s, [&] { return release_return; }))
                return nf::Status{nf::ErrorCode::callback_failed};
            return nf::Status::success();
        }).ok()) {
        std::filesystem::remove_all(root);
        return false;
    }
    if (!engine.on_error([&](const nf::ErrorEvent& ev) {
            std::cerr << "graceful_logout_during_callback_return: unexpected on_error: "
                      << ev.status.message() << "\n";
            error_count.fetch_add(1, std::memory_order_relaxed);
        }).ok()) {
        std::filesystem::remove_all(root);
        return false;
    }
    if (!engine.add_listener({.name = "loopback", .endpoint = {"127.0.0.1", 0}}).ok() ||
        !engine.add_acceptor({.name = "peer", .listener = "loopback",
            .local_comp_id = "SELL", .remote_comp_id = "BUY"}).ok()) {
        std::filesystem::remove_all(root);
        return false;
    }

    nf::Status run_status{};
    owner = std::thread([&] { run_status = engine.run(); });
    auto endpoint = nf::Result<nf::TcpEndpoint>{nf::Status{nf::ErrorCode::need_more}};
    const auto endpoint_deadline = std::chrono::steady_clock::now() + 3s;
    while (!endpoint.ok() && std::chrono::steady_clock::now() < endpoint_deadline) {
        endpoint = engine.listener_endpoint("loopback");
        if (!endpoint.ok()) std::this_thread::sleep_for(2ms);
    }

    bool ok = false;
    std::string buffered, wire;
    if (endpoint.ok()) {
        client = connect_loopback(endpoint.value().port);
        if (client >= 0 &&
            send_all(client, frame("A", 1, "BUY", "SELL")) &&
            receive_frame(client, buffered, wire) && is_type(wire, "A") &&
            send_all(client, frame("D", 2, "BUY", "SELL")) &&
            receive_frame(client, buffered, wire) && is_type(wire, "8") &&
            send_all(client, frame("5", 3, "BUY", "SELL")) &&
            receive_frame(client, buffered, wire) && is_type(wire, "5")) {
            ::close(client);
            client = -1;
            engine.request_stop();
            std::this_thread::sleep_for(15ms);
            {
                std::lock_guard lock(mutex);
                release_return = true;
            }
            cv.notify_all();
            owner.join();

            bool saw_inbound = false;
            bool saw_delivery = false;
            nf::RecordId inbound_id = 0;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "journal")) {
                if (entry.path().extension() != ".nfxj") continue;
                const auto jpath = entry.path().string();
                auto tailer = nf::JournalTailer::open({.path = jpath});
                if (!tailer.ok()) break;
                nf::JournalRecord rec{};
                while (tailer.value().next(rec).ok()) {
                    if (rec.kind == nf::RecordKind::inbound && rec.seq == 2) {
                        saw_inbound = true;
                        inbound_id = rec.id;
                    } else if (rec.kind == nf::RecordKind::delivery && rec.seq == 2 &&
                               inbound_id != 0 && rec.dependency == inbound_id) {
                        saw_delivery = true;
                    }
                }
            }
            ok = run_status.ok() && error_count.load() == 0 && saw_inbound && saw_delivery;
            if (!ok) {
                std::cerr << "graceful_logout_during_callback_return failed: run="
                          << run_status.message() << " errors=" << error_count.load()
                          << " saw_inbound=" << saw_inbound
                          << " saw_delivery=" << saw_delivery << "\n";
            }
        }
    }

    {
        std::lock_guard lock(mutex);
        release_return = true;
    }
    cv.notify_all();
    engine.request_stop();
    if (client >= 0) ::close(client);
    if (owner.joinable()) owner.join();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return ok;
}

} // namespace

int main() {
    char path[] = "/tmp/nimblefix-queued-runtime-XXXXXX";
    const auto* directory = ::mkdtemp(path);
    if (directory == nullptr) return 1;
    const std::filesystem::path root{directory};
    int client = -1;
    std::thread owner;
    nf::Engine<nf::FlatSchema, nf::Queued> engine = [&] {
        auto opened = nf::Engine<nf::FlatSchema, nf::Queued>::create({
            .storage = {.directory = root / "journal"}, .logout_timeout_ms = 1000});
        if (!opened.ok()) throw std::runtime_error("engine create failed");
        return std::move(opened).value();
    }();

    std::mutex mutex;
    std::condition_variable changed;
    bool inbound_entered = false;
    bool release_inbound = false;
    std::atomic<bool> active_on_worker{false};
    std::atomic<bool> message_on_worker{false};
    std::atomic<bool> callback_send_ok{false};
    std::atomic<std::uint64_t> active_receipt_id{};
    std::atomic<unsigned> error_count{};
    std::mutex owner_id_mutex;
    std::thread::id owner_id{};
    const auto get_owner_id = [&]() {
        std::lock_guard lock(owner_id_mutex);
        return owner_id;
    };

    if (!engine.on_session_active([&](nf::SessionContext& context) {
            active_on_worker.store(std::this_thread::get_id() != get_owner_id(), std::memory_order_release);
            auto message = nf::message<"D">().set<11>("ACTIVE").set<55>("AAPL");
            auto sent = context.send(message);
            if (sent.ok()) active_receipt_id.store(sent.value().id, std::memory_order_release);
            callback_send_ok.store(sent.ok(), std::memory_order_release);
            return sent.status();
        }).ok()) return 2;
    if (!engine.on_message([&](nf::SessionContext& context, const nf::FlatMessageView& message) {
            const auto type = message.find_unique(35);
            if (!type.ok() || type.value().value != "D") return nf::Status::success();
            message_on_worker.store(std::this_thread::get_id() != get_owner_id(), std::memory_order_release);
            {
                std::lock_guard lock(mutex);
                inbound_entered = true;
            }
            changed.notify_all();
            {
                std::unique_lock lock(mutex);
                if (!changed.wait_for(lock, 5s, [&] { return release_inbound; }))
                    return nf::Status{nf::ErrorCode::callback_failed};
            }
            auto reply = nf::message<"8">().set<11>("INBOUND").set<17>("EXEC-1");
            return context.send(reply).status();
        }).ok()) return 3;
    if (!engine.on_error([&](const nf::ErrorEvent&) {
            error_count.fetch_add(1, std::memory_order_relaxed);
        }).ok()) return 4;

    auto listener = engine.add_listener({.name = "loopback", .endpoint = {"127.0.0.1", 0}});
    if (!listener.ok()) return 5;
    auto session = engine.add_acceptor({.name = "peer", .listener = "loopback",
        .local_comp_id = "SELL", .remote_comp_id = "BUY"});
    if (!session.ok()) return 6;
    auto sender = session.value().sender<>();

    nf::Status run_status{};
    owner = std::thread([&] {
        {
            std::lock_guard lock(owner_id_mutex);
            owner_id = std::this_thread::get_id();
        }
        run_status = engine.run();
    });
    auto endpoint = nf::Result<nf::TcpEndpoint>{nf::Status{nf::ErrorCode::need_more}};
    const auto endpoint_deadline = std::chrono::steady_clock::now() + 3s;
    while (!endpoint.ok() && std::chrono::steady_clock::now() < endpoint_deadline) {
        endpoint = engine.listener_endpoint("loopback");
        if (!endpoint.ok()) std::this_thread::sleep_for(2ms);
    }
    if (!endpoint.ok()) goto cleanup;
    client = connect_loopback(endpoint.value().port);
    if (client < 0 || !send_all(client, frame("A", 1, "BUY", "SELL"))) goto cleanup;

    {
        std::string buffered, wire;
        if (!receive_frame(client, buffered, wire) || !is_type(wire, "A")) goto cleanup;

        const auto send_deadline = std::chrono::steady_clock::now() + 3s;
        while (!sender.can_send() && std::chrono::steady_clock::now() < send_deadline)
            std::this_thread::sleep_for(1ms);
        if (!sender.can_send()) goto cleanup;

        constexpr unsigned producers = 4;
        constexpr unsigned per_producer = 8;
        std::mutex receipts_mutex;
        std::vector<std::uint64_t> receipt_ids;
        std::atomic<bool> producers_ok{true};
        std::vector<std::thread> producers_threads;
        for (unsigned producer = 0; producer < producers; ++producer) {
            producers_threads.emplace_back([&, producer] {
                for (unsigned item = 0; item < per_producer; ++item) {
                    const auto id = std::to_string(producer) + ":" + std::to_string(item);
                    auto outbound = nf::message<"D">().set<11>(id).set<55>("AAPL");
                    auto accepted = sender.send(outbound);
                    if (!accepted.ok()) {
                        producers_ok.store(false, std::memory_order_release);
                        continue;
                    }
                    std::lock_guard lock(receipts_mutex);
                    receipt_ids.push_back(accepted.value().id);
                }
            });
        }
        for (auto& producer : producers_threads) producer.join();
        if (!producers_ok.load(std::memory_order_acquire) ||
            receipt_ids.size() != producers * per_producer) goto cleanup;

        if (!send_all(client, frame("D", 2, "BUY", "SELL"))) goto cleanup;
        {
            std::unique_lock lock(mutex);
            if (!changed.wait_for(lock, 5s, [&] { return inbound_entered; })) goto cleanup;
        }
        if (!send_all(client, frame("1", 3, "BUY", "SELL"))) goto cleanup;

        unsigned outgoing_orders = 0;
        bool saw_heartbeat = false;
        const unsigned expected_orders = producers * per_producer + 1;
        while (!saw_heartbeat || outgoing_orders < expected_orders) {
            if (!receive_frame(client, buffered, wire, 5s)) {
                std::cerr << "queued_runtime_test: timed out waiting for heartbeat/orders "
                          << "(saw_heartbeat=" << saw_heartbeat
                          << ", outgoing_orders=" << outgoing_orders
                          << "/" << expected_orders << ")\n";
                goto cleanup;
            }
            if (is_type(wire, "D")) ++outgoing_orders;
            else if (is_type(wire, "0")) saw_heartbeat = true;
        }
        if (outgoing_orders != expected_orders) {
            std::cerr << "queued_runtime_test: unexpected outgoing_orders=" << outgoing_orders
                      << " expected=" << expected_orders << "\n";
            goto cleanup;
        }
        {
            std::lock_guard lock(mutex);
            release_inbound = true;
        }
        changed.notify_all();

        bool saw_reply = false;
        while (!saw_reply) {
            if (!receive_frame(client, buffered, wire)) goto cleanup;
            saw_reply = is_type(wire, "8");
        }
        if (!callback_send_ok.load(std::memory_order_acquire) ||
            !active_on_worker.load(std::memory_order_acquire) ||
            !message_on_worker.load(std::memory_order_acquire)) goto cleanup;

        const auto receipt_deadline = std::chrono::steady_clock::now() + 3s;
        bool all_written = false;
        while (std::chrono::steady_clock::now() < receipt_deadline) {
            all_written = true;
            for (const auto id : receipt_ids) {
                const auto receipt = sender.receipt(id);
                if (!receipt.ok() || receipt.value().state != nf::ReceiptState::written) {
                    all_written = false;
                    break;
                }
            }
            const auto active_id = active_receipt_id.load(std::memory_order_acquire);
            const auto active_receipt = sender.receipt(active_id);
            all_written = all_written && active_receipt.ok() &&
                active_receipt.value().state == nf::ReceiptState::written;
            if (all_written) break;
            std::this_thread::sleep_for(2ms);
        }
        if (!all_written || !send_all(client, frame("5", 4, "BUY", "SELL"))) goto cleanup;
        while (receive_frame(client, buffered, wire)) {
            if (is_type(wire, "5")) break;
        }
        engine.request_stop();
        if (client >= 0) {
            ::close(client);
            client = -1;
        }
        owner.join();
        const bool passed = run_status.ok() && error_count.load() == 0 &&
            test_graceful_logout_during_callback_return();
        std::filesystem::remove_all(root);
        if (!passed) std::cerr << "queued owner/worker or MPSC behavior failed\n";
        else std::cout << "queued callbacks, dependent replies, and MPSC sender passed\n";
        return passed ? 0 : 1;
    }

cleanup:
    {
        {
            std::lock_guard lock(mutex);
            release_inbound = true;
        }
        changed.notify_all();
        engine.request_stop();
        if (client >= 0) ::close(client);
        if (owner.joinable()) owner.join();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        return 1;
    }
}
