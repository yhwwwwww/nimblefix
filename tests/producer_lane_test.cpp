#include <nimblefix/engine.hpp>
#include <nimblefix/message.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
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

void check(bool result, const char* expression, int line) {
    if (!result) throw std::runtime_error(std::string{"line "} + std::to_string(line) +
        ": check failed: " + expression);
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

std::string frame(std::string_view type, std::uint64_t sequence,
    std::string_view sender, std::string_view target, std::string_view id = {},
    std::optional<std::uint64_t> last_processed = {}) {
    std::array<char, 2048> bytes{};
    std::array<char, 32> stamp{};
    const auto now = std::time(nullptr);
    std::tm utc{};
    ::gmtime_r(&now, &utc);
    ::strftime(stamp.data(), stamp.size(), "%Y%m%d-%H:%M:%S", &utc);
    nf::SessionHeader header{sender, target, sequence, stamp.data()};
    header.last_msg_seq_num_processed = last_processed;
    nf::Result<std::size_t> encoded{nf::Status{nf::ErrorCode::invalid_value}};
    if (type == "A") {
        auto message = nf::message<"A">().set<98>(0).set<108>(30);
        encoded = nf::encode_fix44(message, header, bytes);
    } else if (type == "D") {
        auto message = nf::message<"D">().set<11>(id).set<55>("AAPL");
        encoded = nf::encode_fix44(message, header, bytes);
    } else if (type == "2") {
        auto message = nf::message<"2">().set<7>(2).set<16>(2);
        encoded = nf::encode_fix44(message, header, bytes);
    } else if (type == "5") {
        auto message = nf::message<"5">();
        encoded = nf::encode_fix44(message, header, bytes);
    }
    if (!encoded.ok()) throw std::runtime_error("could not encode test FIX frame");
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

bool field_is(std::string_view bytes, std::uint32_t tag, std::string_view value) {
    std::string marker = std::to_string(tag);
    marker.push_back('=');
    marker.append(value);
    marker.push_back('\x01');
    return bytes.find(marker) != std::string_view::npos;
}

bool receive_frame(int fd, std::string& buffered, std::string& result,
    std::chrono::milliseconds timeout = 4s) {
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
        if (::poll(&descriptor, 1,
                static_cast<int>(std::max<std::int64_t>(1, left.count()))) <= 0)
            return false;
        const auto count = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (count <= 0) return false;
        buffered.append(chunk.data(), static_cast<std::size_t>(count));
    }
    return false;
}

template <class Engine>
struct RuntimeGuard {
    Engine& engine;
    std::thread& owner;
    int& client;
    ~RuntimeGuard() {
        if (client >= 0) {
            ::close(client);
            client = -1;
        }
        engine.request_stop();
        if (owner.joinable()) owner.join();
    }
};

bool test_lanes(const std::filesystem::path& root) {
    nf::EngineConfig config;
    config.storage.directory = root / "lane-journal";
    config.max_producer_lanes_per_session = 2;
    auto opened = nf::Engine<>::create(std::move(config));
    CHECK(opened.ok());
    auto engine = std::move(opened).value();
    std::atomic<unsigned> errors{0};
    CHECK(engine.on_error([&](const nf::ErrorEvent&) {
        errors.fetch_add(1, std::memory_order_relaxed);
    }).ok());
    CHECK(engine.add_listener({.name = "lane-listener", .endpoint = {"127.0.0.1", 0}}).ok());
    auto session = engine.add_acceptor({.name = "lane-session", .listener = "lane-listener",
        .local_comp_id = "SELL", .remote_comp_id = "BUY"});
    CHECK(session.ok());

    auto lane1_result = session.value().producer_lane({.capacity_messages = 2,
        .capacity_bytes = 32U * 1024U, .receipt_capacity = 1});
    auto lane2_result = session.value().producer_lane({.capacity_messages = 2,
        .capacity_bytes = 32U * 1024U, .receipt_capacity = 1});
    CHECK(lane1_result.ok() && lane2_result.ok());
    auto lane1 = std::move(lane1_result).value();
    auto lane2 = std::move(lane2_result).value();
    const auto excess = session.value().producer_lane();
    CHECK(!excess.ok() && excess.status().code == nf::ErrorCode::capacity_exceeded);

    auto retained = nf::message<"D">().set<11>("NOT-READY").set<55>("AAPL");
    const auto before_active = lane1.send(retained);
    CHECK(!before_active.ok() && before_active.status().code == nf::ErrorCode::not_ready);

    nf::Status run_status{};
    std::thread owner([&] { run_status = engine.run(); });
    int client = -1;
    RuntimeGuard guard{engine, owner, client};
    nf::Result<nf::TcpEndpoint> endpoint{nf::Status{nf::ErrorCode::need_more}};
    const auto endpoint_deadline = std::chrono::steady_clock::now() + 4s;
    while (!endpoint.ok() && std::chrono::steady_clock::now() < endpoint_deadline) {
        endpoint = engine.listener_endpoint("lane-listener");
        if (!endpoint.ok()) std::this_thread::sleep_for(2ms);
    }
    CHECK(endpoint.ok());
    client = connect_loopback(endpoint.value().port);
    CHECK(client >= 0 && send_all(client, frame("A", 1, "BUY", "SELL")));
    std::string buffered, wire;
    CHECK(receive_frame(client, buffered, wire) && field_is(wire, 35, "A"));

    const auto active_deadline = std::chrono::steady_clock::now() + 3s;
    while ((!lane1.can_send() || !lane2.can_send()) &&
        std::chrono::steady_clock::now() < active_deadline) std::this_thread::sleep_for(1ms);
    CHECK(lane1.can_send() && lane2.can_send());

    constexpr std::size_t messages_per_lane = 128;
    std::array<std::vector<std::uint64_t>, 2> receipts;
    std::atomic<bool> producer_ok{true};
    const auto produce = [&](nf::ProducerLane<> lane, unsigned source,
        std::vector<std::uint64_t>& accepted_ids) {
        for (std::size_t i = 0; i < messages_per_lane; ++i) {
            const auto id = std::to_string(source) + ":" + std::to_string(i);
            auto message = nf::message<"D">().set<11>(id).set<55>("AAPL");
            for (;;) {
                const auto sent = lane.send(message);
                if (sent.ok()) {
                    accepted_ids.push_back(sent.value().id);
                    const auto receipt_deadline = std::chrono::steady_clock::now() + 4s;
                    bool written = false;
                    while (!written && std::chrono::steady_clock::now() < receipt_deadline) {
                        const auto receipt = lane.receipt(sent.value().id);
                        if (!receipt.ok()) {
                            producer_ok.store(false, std::memory_order_release);
                            return;
                        }
                        if (receipt.value().state == nf::ReceiptState::failed ||
                            receipt.value().state == nf::ReceiptState::expired) {
                            producer_ok.store(false, std::memory_order_release);
                            return;
                        }
                        written = receipt.value().state == nf::ReceiptState::written;
                        if (!written) std::this_thread::yield();
                    }
                    if (!written) {
                        producer_ok.store(false, std::memory_order_release);
                        return;
                    }
                    break;
                }
                if (sent.status().code != nf::ErrorCode::queue_full &&
                    sent.status().code != nf::ErrorCode::not_ready) {
                    producer_ok.store(false, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
            }
        }
    };
    std::thread first(produce, lane1, 1, std::ref(receipts[0]));
    std::thread second(produce, lane2, 2, std::ref(receipts[1]));

    std::set<std::string> received_ids;
    std::uint64_t expected_sequence = 2;
    const auto receive_deadline = std::chrono::steady_clock::now() + 8s;
    while (received_ids.size() < messages_per_lane * 2 &&
        std::chrono::steady_clock::now() < receive_deadline) {
        if (!receive_frame(client, buffered, wire, 500ms)) continue;
        CHECK(field_is(wire, 35, "D"));
        CHECK(field_is(wire, 34, std::to_string(expected_sequence++)));
        for (unsigned source = 1; source <= 2; ++source) {
            for (std::size_t i = 0; i < messages_per_lane; ++i) {
                const auto id = std::to_string(source) + ":" + std::to_string(i);
                if (field_is(wire, 11, id)) received_ids.insert(id);
            }
        }
    }
    first.join();
    second.join();
    CHECK(producer_ok.load(std::memory_order_acquire));
    CHECK(received_ids.size() == messages_per_lane * 2);
    CHECK(receipts[0].size() == messages_per_lane && receipts[1].size() == messages_per_lane);
    CHECK(lane1.send(retained).status().code == nf::ErrorCode::wrong_thread);
    CHECK(lane2.send(retained).status().code == nf::ErrorCode::wrong_thread);
    CHECK(!session.value().producer_lane().ok()); // lane creation is frozen at start().

    const auto receipt_deadline = std::chrono::steady_clock::now() + 4s;
    bool all_written = false;
    while (!all_written && std::chrono::steady_clock::now() < receipt_deadline) {
        all_written = true;
        const auto last1 = lane1.receipt(receipts[0].back());
        const auto last2 = lane2.receipt(receipts[1].back());
        all_written = last1.ok() && last1.value().state == nf::ReceiptState::written &&
            last2.ok() && last2.value().state == nf::ReceiptState::written;
        if (!all_written) std::this_thread::sleep_for(1ms);
    }
    if (!all_written) {
        std::cerr << "latest lane receipt did not reach written\n";
    }
    CHECK(all_written);
    CHECK(send_all(client, frame("5", 2, "BUY", "SELL")));
    CHECK(receive_frame(client, buffered, wire) && field_is(wire, 35, "5"));
    engine.request_stop();
    ::close(client);
    client = -1;
    owner.join();
    CHECK(run_status.ok());
    CHECK(errors.load(std::memory_order_relaxed) == 0);
    CHECK(lane1.send(retained).status().code == nf::ErrorCode::engine_stopped);
    return true;
}

bool test_lane_query_during_stop(const std::filesystem::path& root) {
    nf::EngineConfig config;
    config.storage.directory = root / "lane-stop-race";
    config.max_producer_lanes_per_session = 1;
    auto opened = nf::Engine<>::create(std::move(config));
    CHECK(opened.ok());
    auto engine = std::move(opened).value();
    CHECK(engine.add_listener({.name = "stop-listener", .endpoint = {"127.0.0.1", 0}}).ok());
    auto session = engine.add_acceptor({.name = "stop-session", .listener = "stop-listener",
        .local_comp_id = "SELL", .remote_comp_id = "BUY"});
    CHECK(session.ok());
    auto lane_result = session.value().producer_lane({.capacity_messages = 2,
        .capacity_bytes = 32U * 1024U, .receipt_capacity = 2});
    CHECK(lane_result.ok());
    auto lane = std::move(lane_result).value();

    nf::Status run_status{};
    std::atomic<bool> owner_done{false};
    std::thread owner([&] {
        run_status = engine.run();
        owner_done.store(true, std::memory_order_release);
    });
    nf::Result<nf::TcpEndpoint> endpoint{nf::Status{nf::ErrorCode::need_more}};
    const auto endpoint_deadline = std::chrono::steady_clock::now() + 4s;
    while (!endpoint.ok() && !owner_done.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < endpoint_deadline) {
        endpoint = engine.listener_endpoint("stop-listener");
        if (!endpoint.ok()) std::this_thread::sleep_for(2ms);
    }
    CHECK(endpoint.ok());
    int client = connect_loopback(endpoint.value().port);
    CHECK(client >= 0 && send_all(client, frame("A", 1, "BUY", "SELL")));
    std::string buffered, wire;
    CHECK(receive_frame(client, buffered, wire) && field_is(wire, 35, "A"));
    const auto active_deadline = std::chrono::steady_clock::now() + 3s;
    while (!lane.can_send() && std::chrono::steady_clock::now() < active_deadline)
        std::this_thread::sleep_for(1ms);
    CHECK(lane.can_send());

    std::atomic<std::uint64_t> latest_id{0};
    std::atomic<bool> send_failed{false};
    std::atomic<bool> saw_engine_stopped{false};
    std::atomic<std::uint64_t> query_count{0};
    std::thread producer([&] {
        auto request = nf::message<"D">().set<11>("STOP-RACE").set<55>("AAPL");
        for (;;) {
            const auto sent = lane.send(request);
            if (sent.ok()) {
                latest_id.store(sent.value().id, std::memory_order_release);
                continue;
            }
            if (sent.status().code == nf::ErrorCode::queue_full ||
                sent.status().code == nf::ErrorCode::not_ready) {
                std::this_thread::yield();
                continue;
            }
            if (sent.status().code == nf::ErrorCode::engine_stopped)
                saw_engine_stopped.store(true, std::memory_order_release);
            else
                send_failed.store(true, std::memory_order_release);
            return;
        }
    });
    std::atomic<bool> saw_logout{false};
    std::thread peer([&] {
        bool logout_answered = false;
        while (!owner_done.load(std::memory_order_acquire)) {
            if (!receive_frame(client, buffered, wire, 200ms)) continue;
            if (!logout_answered && field_is(wire, 35, "5")) {
                logout_answered = true;
                saw_logout.store(true, std::memory_order_release);
                if (!send_all(client, frame("5", 2, "BUY", "SELL")))
                    send_failed.store(true, std::memory_order_release);
            }
        }
    });
    std::thread reader([&] {
        while (!owner_done.load(std::memory_order_acquire)) {
            const auto id = latest_id.load(std::memory_order_acquire);
            if (id != 0) {
                const auto receipt = lane.receipt(id);
                if (!receipt.ok() && receipt.status().code != nf::ErrorCode::receipt_expired)
                    send_failed.store(true, std::memory_order_release);
                query_count.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    });
    std::this_thread::sleep_for(20ms);
    engine.request_stop();
    owner.join();
    producer.join();
    reader.join();
    peer.join();
    ::close(client);
    CHECK(run_status.ok());
    CHECK(saw_logout.load(std::memory_order_acquire));
    CHECK(saw_engine_stopped.load(std::memory_order_acquire));
    CHECK(!send_failed.load(std::memory_order_acquire));
    CHECK(query_count.load(std::memory_order_relaxed) != 0);
    const auto last_receipt = lane.receipt(latest_id.load(std::memory_order_acquire));
    if (last_receipt.ok())
        CHECK(last_receipt.value().state == nf::ReceiptState::written ||
            last_receipt.value().state == nf::ReceiptState::failed ||
            last_receipt.value().state == nf::ReceiptState::expired);
    else
        CHECK(last_receipt.status().code == nf::ErrorCode::receipt_expired);
    CHECK(lane.send(nf::message<"D">().set<11>("LATE").set<55>("AAPL")).status().code ==
        nf::ErrorCode::engine_stopped);
    return true;
}

bool test_old_generation_reply(const std::filesystem::path& root, bool same_reply) {
    using QueuedEngine = nf::Engine<nf::FlatSchema, nf::Queued>;
    nf::EngineConfig config;
    config.storage.directory = root / (same_reply ? "same-reply" : "changed-reply");
    config.queued.worker_threads = 1;
    config.queued.max_pending_callbacks = 8;
    config.queued.max_pending_callback_bytes = 128U * 1024U;
    auto opened = QueuedEngine::create(std::move(config));
    CHECK(opened.ok());
    auto engine = std::move(opened).value();

    std::mutex mutex;
    std::condition_variable changed;
    bool first_callback_entered = false;
    bool release_first_callback = false;
    std::atomic<unsigned> callback_count{0};
    std::atomic<std::uint64_t> first_receipt_id{0};
    std::atomic<std::uint64_t> retry_receipt_id{0};
    std::atomic<unsigned> observed_errors{0};
    CHECK(engine.on_message([&](nf::SessionContext& context,
        const nf::FlatMessageView& message) -> nf::Status {
        if (!field_is(message.frame(), 35, "D")) return nf::Status::success();
        const auto attempt = callback_count.fetch_add(1, std::memory_order_acq_rel);
        const auto exec_id = attempt == 0 || same_reply ? "EXEC-OLD" : "EXEC-NEW";
        auto reply = nf::message<"8">().set<11>("ORDER-1").set<17>(exec_id);
        const auto sent = context.send(reply);
        if (!sent.ok()) return sent.status();
        if (attempt == 0) first_receipt_id.store(sent.value().id, std::memory_order_release);
        else retry_receipt_id.store(sent.value().id, std::memory_order_release);

        if (attempt == 0) {
            std::unique_lock lock(mutex);
            first_callback_entered = true;
            changed.notify_all();
            if (!changed.wait_for(lock, 8s, [&] { return release_first_callback; }))
                return nf::Status{nf::ErrorCode::callback_failed};
        }
        return nf::Status::success();
    }).ok());
    CHECK(engine.on_error([&](const nf::ErrorEvent&) {
        observed_errors.fetch_add(1, std::memory_order_relaxed);
    }).ok());
    CHECK(engine.add_listener({.name = "recovery-listener", .endpoint = {"127.0.0.1", 0}}).ok());
    auto session = engine.add_acceptor({.name = "recovery-session", .listener = "recovery-listener",
        .local_comp_id = "SELL", .remote_comp_id = "BUY"});
    CHECK(session.ok());
    auto sender = session.value().sender<>();

    nf::Status run_status{};
    std::thread owner([&] { run_status = engine.run(); });
    int client = -1;
    RuntimeGuard guard{engine, owner, client};
    nf::Result<nf::TcpEndpoint> endpoint{nf::Status{nf::ErrorCode::need_more}};
    const auto endpoint_deadline = std::chrono::steady_clock::now() + 4s;
    while (!endpoint.ok() && std::chrono::steady_clock::now() < endpoint_deadline) {
        endpoint = engine.listener_endpoint("recovery-listener");
        if (!endpoint.ok()) std::this_thread::sleep_for(2ms);
    }
    CHECK(endpoint.ok());

    client = connect_loopback(endpoint.value().port);
    CHECK(client >= 0 && send_all(client, frame("A", 1, "BUY", "SELL")));
    std::string buffered, wire;
    CHECK(receive_frame(client, buffered, wire) && field_is(wire, 35, "A"));
    CHECK(send_all(client, frame("D", 2, "BUY", "SELL", "ORDER-1")));
    {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, 4s, [&] { return first_callback_entered; }));
    }

    const auto first_id_deadline = std::chrono::steady_clock::now() + 3s;
    while (first_receipt_id.load(std::memory_order_acquire) == 0 &&
        std::chrono::steady_clock::now() < first_id_deadline) std::this_thread::sleep_for(1ms);
    const auto first_id = first_receipt_id.load(std::memory_order_acquire);
    CHECK(first_id != 0);
    const auto appended_deadline = std::chrono::steady_clock::now() + 3s;
    bool appended = false;
    while (!appended && std::chrono::steady_clock::now() < appended_deadline) {
        const auto receipt = sender.receipt(first_id);
        appended = receipt.ok() && (receipt.value().state == nf::ReceiptState::appended ||
            receipt.value().state == nf::ReceiptState::written);
        if (!appended) std::this_thread::sleep_for(1ms);
    }
    if (!appended) {
        const auto receipt = sender.receipt(first_id);
        std::cerr << "first dependent receipt=" << first_id << " state=" <<
            (receipt.ok() ? static_cast<int>(receipt.value().state) : -1) << " error=" <<
            (receipt.ok() ? receipt.value().status.message() : receipt.status().message()) <<
            " callbacks=" << callback_count.load() << " errors=" << observed_errors.load() << '\n';
    }
    CHECK(appended);

    ::close(client);
    client = -1;
    const auto disconnect_deadline = std::chrono::steady_clock::now() + 3s;
    bool disconnected = false;
    while (!disconnected && std::chrono::steady_clock::now() < disconnect_deadline) {
        const auto snapshot = engine.snapshot("recovery-session");
        disconnected = snapshot.ok() && snapshot.value().state == nf::SessionState::disconnected;
        if (!disconnected) std::this_thread::sleep_for(1ms);
    }
    CHECK(disconnected);

    client = connect_loopback(endpoint.value().port);
    CHECK(client >= 0 && send_all(client, frame("A", 3, "BUY", "SELL", {}, 1)));
    buffered.clear();
    bool saw_logon = false;
    const auto logon_deadline = std::chrono::steady_clock::now() + 4s;
    while (!saw_logon && std::chrono::steady_clock::now() < logon_deadline) {
        if (!receive_frame(client, buffered, wire, 300ms)) continue;
        if (field_is(wire, 35, "A")) saw_logon = true;
    }
    CHECK(saw_logon);

    CHECK(send_all(client, frame("2", 4, "BUY", "SELL")));
    std::vector<std::string> reply_exec_ids;
    bool saw_replay = false;
    const auto replay_deadline = std::chrono::steady_clock::now() + 4s;
    while (!saw_replay && std::chrono::steady_clock::now() < replay_deadline) {
        if (!receive_frame(client, buffered, wire, 300ms)) continue;
        if (field_is(wire, 35, "8")) {
            if (field_is(wire, 17, "EXEC-OLD")) {
                reply_exec_ids.emplace_back("EXEC-OLD");
                saw_replay = true;
            } else if (field_is(wire, 17, "EXEC-NEW")) {
                reply_exec_ids.emplace_back("EXEC-NEW");
            }
        }
    }
    CHECK(saw_replay);

    {
        std::lock_guard lock(mutex);
        release_first_callback = true;
    }
    changed.notify_all();

    bool retry_called = false;
    const auto recovery_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < recovery_deadline) {
        retry_called = callback_count.load(std::memory_order_acquire) >= 2;
        if (retry_called && std::chrono::steady_clock::now() + 150ms >= recovery_deadline) break;
        if (!receive_frame(client, buffered, wire, 200ms)) continue;
        if (field_is(wire, 35, "8")) {
            if (field_is(wire, 17, "EXEC-OLD")) reply_exec_ids.emplace_back("EXEC-OLD");
            if (field_is(wire, 17, "EXEC-NEW")) reply_exec_ids.emplace_back("EXEC-NEW");
        }
    }
    CHECK(retry_called);
    if (reply_exec_ids.size() != 1 || reply_exec_ids.front() != "EXEC-OLD") {
        std::cerr << "recovery replies same=" << same_reply << " count=" << reply_exec_ids.size()
            << " callbacks=" << callback_count.load() << " errors=" << observed_errors.load();
        for (const auto& value : reply_exec_ids) std::cerr << " [" << value << ']';
        std::cerr << '\n';
    }
    if (same_reply) {
        CHECK(retry_receipt_id.load(std::memory_order_acquire) != 0);
        CHECK(reply_exec_ids.size() == 1 && reply_exec_ids.front() == "EXEC-OLD");
        const auto retry_id = retry_receipt_id.load(std::memory_order_acquire);
        const auto retry_deadline = std::chrono::steady_clock::now() + 3s;
        bool retry_appended = false;
        while (!retry_appended && std::chrono::steady_clock::now() < retry_deadline) {
            const auto receipt = sender.receipt(retry_id);
            retry_appended = receipt.ok() && (receipt.value().state == nf::ReceiptState::appended ||
                receipt.value().state == nf::ReceiptState::written);
            if (!retry_appended) std::this_thread::sleep_for(1ms);
        }
        CHECK(retry_appended);
    } else {
        CHECK(reply_exec_ids.size() == 1 && reply_exec_ids.front() == "EXEC-OLD");
        const auto retry_id = retry_receipt_id.load(std::memory_order_acquire);
        CHECK(retry_id != 0);
        const auto retry_deadline = std::chrono::steady_clock::now() + 3s;
        bool retry_failed = false;
        while (!retry_failed && std::chrono::steady_clock::now() < retry_deadline) {
            const auto receipt = sender.receipt(retry_id);
            retry_failed = receipt.ok() && receipt.value().state == nf::ReceiptState::failed;
            if (!retry_failed) std::this_thread::sleep_for(1ms);
        }
        CHECK(retry_failed);
    }

    if (same_reply) {
        CHECK(send_all(client, frame("5", 5, "BUY", "SELL")));
        const auto logout_deadline = std::chrono::steady_clock::now() + 3s;
        bool saw_logout = false;
        while (!saw_logout && std::chrono::steady_clock::now() < logout_deadline) {
            if (!receive_frame(client, buffered, wire, 200ms)) continue;
            saw_logout = field_is(wire, 35, "5");
        }
        CHECK(saw_logout);
    }
    engine.request_stop();
    ::close(client);
    client = -1;
    owner.join();
    if (same_reply) CHECK(run_status.ok());
    return true;
}

} // namespace

int main() {
    char pattern[] = "/tmp/nimblefix-producer-lane-XXXXXX";
    const auto* directory = ::mkdtemp(pattern);
    if (directory == nullptr) return 1;
    const std::filesystem::path root{directory};
    bool passed = false;
    try {
        passed = test_lanes(root) && test_lane_query_during_stop(root) &&
            test_old_generation_reply(root, true) &&
            test_old_generation_reply(root, false);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    std::filesystem::remove_all(root);
    if (!passed) return 1;
    std::cout << "producer lanes and old-generation dependent reply recovery passed\n";
    return 0;
}
