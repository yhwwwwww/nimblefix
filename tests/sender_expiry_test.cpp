#include <nimblefix/engine.hpp>
#include <nimblefix/journal.hpp>
#include <nimblefix/message.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nf = nimblefix;
using namespace std::chrono_literals;

namespace {

void check(bool condition, const char* expression, int line) {
    if (!condition) throw std::runtime_error(std::string{"line "} +
        std::to_string(line) + ": " + expression);
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

std::string utc_now() {
    const auto now = std::time(nullptr);
    std::tm utc{};
    CHECK(::gmtime_r(&now, &utc) != nullptr);
    std::array<char, 32> result{};
    CHECK(::strftime(result.data(), result.size(), "%Y%m%d-%H:%M:%S", &utc) != 0);
    return result.data();
}

template <class Builder>
std::string encode(Builder&& builder, std::string_view sender,
    std::string_view target, std::uint64_t sequence) {
    std::array<char, 4096> bytes{};
    const auto stamp = utc_now();
    nf::SessionHeader header{sender, target, sequence, stamp};
    const auto result = nf::encode_fix44(std::forward<Builder>(builder), header, bytes);
    CHECK(result.ok());
    return {bytes.data(), result.value()};
}

std::string logon(std::uint64_t sequence) {
    return encode(nf::message<"A">().set<98>(0).set<108>(10), "BUY", "SELL", sequence);
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
    const auto fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
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

bool field_is(std::string_view wire, std::uint32_t tag, std::string_view value) {
    const auto marker = std::to_string(tag) + "=" + std::string{value} + '\x01';
    return wire.find(marker) != std::string_view::npos;
}

std::string_view field(std::string_view wire, std::uint32_t tag) {
    const auto marker = std::to_string(tag) + "=";
    const auto at = wire.find(marker);
    if (at == std::string_view::npos) return {};
    const auto start = at + marker.size();
    const auto end = wire.find('\x01', start);
    return end == std::string_view::npos ? std::string_view{} : wire.substr(start, end - start);
}

bool receive_frame(int fd, std::string& buffered, std::string& result,
    std::chrono::milliseconds timeout = 10ms) {
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

struct PeerEngine {
    std::optional<nf::Engine<nf::FlatSchema>> engine;
    nf::Sender<> sender;
    nf::ProducerLane<> lane;
    int peer{-1};
    std::string buffered;

    PeerEngine(const std::filesystem::path& path, std::uint32_t global_age,
        std::optional<std::uint32_t> flow_age = {}) {
        nf::EngineConfig config;
        config.storage.directory = path;
        config.heartbeat_ms = 10'000;
        config.max_pending_age_ms = global_age;
        config.write_stall_ms = 5'000;
        config.logout_timeout_ms = 100;
        auto opened = nf::Engine<nf::FlatSchema>::create(std::move(config));
        CHECK(opened.ok());
        engine.emplace(std::move(opened).value());
        CHECK(engine->add_listener({.name="listener", .endpoint={"127.0.0.1", 0}}).ok());
        nf::SessionSettings settings;
        if (flow_age) {
            nf::FlowControlConfig flow;
            flow.max_pending_age_ms = *flow_age;
            settings.flow_control = flow;
        }
        auto added = engine->add_acceptor({.name="session", .listener="listener",
            .local_comp_id="SELL", .remote_comp_id="BUY", .settings=std::move(settings)});
        CHECK(added.ok());
        auto session = std::move(added).value();
        sender = session.sender<>();
        auto lane_result = session.producer_lane<>({.capacity_messages=8,
            .capacity_bytes=32U * 1024U, .receipt_capacity=16});
        CHECK(lane_result.ok());
        lane = std::move(lane_result).value();
        CHECK(engine->start().ok());
        const auto endpoint = engine->listener_endpoint("listener");
        CHECK(endpoint.ok());
        peer = connect_loopback(endpoint.value().port);
        CHECK(peer >= 0);
        CHECK(send_all(peer, logon(1)));
        const auto deadline = std::chrono::steady_clock::now() + 4s;
        bool got_logon = false;
        while (!got_logon && std::chrono::steady_clock::now() < deadline) {
            CHECK(engine->poll_once(2).ok());
            std::string wire;
            if (receive_frame(peer, buffered, wire, 2ms)) {
                if (field_is(wire, 35, "A")) got_logon = true;
            }
        }
        CHECK(got_logon);
        const auto active_deadline = std::chrono::steady_clock::now() + 2s;
        while (!sender.can_send() && std::chrono::steady_clock::now() < active_deadline)
            CHECK(engine->poll_once(2).ok());
        CHECK(sender.can_send());
    }

    void stop() {
        if (peer >= 0) {
            ::close(peer);
            peer = -1;
        }
        engine->request_stop();
        for (std::size_t i = 0; i < 100 && engine->running(); ++i) {
            const auto status = engine->poll_once(2);
            CHECK(status.ok() || !engine->running());
        }
        CHECK(!engine->running());
    }

    ~PeerEngine() {
        if (peer >= 0) ::close(peer);
    }
};

void test_expiry_and_zero_sequence(const std::filesystem::path& root) {
    PeerEngine peer{root / "expiry", 45};
    const auto sender_receipt = peer.sender.send(nf::message<"D">().set<11>("OLD-SENDER").set<55>("AAPL"));
    const auto lane_receipt = peer.lane.send(nf::message<"D">().set<11>("OLD-LANE").set<55>("AAPL"));
    CHECK(sender_receipt.ok() && lane_receipt.ok());
    std::this_thread::sleep_for(80ms);
    CHECK(peer.engine->poll_once(0).ok());
    const auto sender_status = peer.sender.receipt(sender_receipt.value().id);
    const auto lane_status = peer.lane.receipt(lane_receipt.value().id);
    CHECK(sender_status.ok() && sender_status.value().state == nf::ReceiptState::expired);
    CHECK(sender_status.value().status.code == nf::ErrorCode::backlog_timeout);
    CHECK(sender_status.value().msg_seq_num == 0);
    CHECK(lane_status.ok() && lane_status.value().state == nf::ReceiptState::expired);
    CHECK(lane_status.value().status.code == nf::ErrorCode::backlog_timeout);
    CHECK(lane_status.value().msg_seq_num == 0);

    std::string wire;
    CHECK(!receive_frame(peer.peer, peer.buffered, wire, 20ms));
    const auto fresh_sender = peer.sender.send(nf::message<"D">().set<11>("FRESH-SENDER").set<55>("AAPL"));
    const auto fresh_lane = peer.lane.send(nf::message<"D">().set<11>("FRESH-LANE").set<55>("AAPL"));
    CHECK(fresh_sender.ok() && fresh_lane.ok());
    std::vector<std::string> frames;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (frames.size() < 2 && std::chrono::steady_clock::now() < deadline) {
        CHECK(peer.engine->poll_once(2).ok());
        while (receive_frame(peer.peer, peer.buffered, wire, 1ms)) {
            if (field_is(wire, 35, "D")) frames.push_back(wire);
        }
    }
    CHECK(frames.size() == 2);
    CHECK(field_is(frames[0], 11, "FRESH-SENDER"));
    CHECK(field_is(frames[1], 11, "FRESH-LANE"));
    CHECK(field(frames[0], 34) == "2");
    CHECK(field(frames[1], 34) == "3");
    peer.stop();
}

void test_zero_disables_expiry(const std::filesystem::path& root) {
    PeerEngine peer{root / "no-expiry", 0};
    const auto receipt = peer.sender.send(nf::message<"D">().set<11>("ZERO-DISABLED").set<55>("AAPL"));
    CHECK(receipt.ok());
    std::this_thread::sleep_for(80ms);
    CHECK(peer.engine->poll_once(0).ok());
    const auto snapshot = peer.sender.receipt(receipt.value().id);
    CHECK(snapshot.ok());
    CHECK(snapshot.value().state != nf::ReceiptState::expired);
    CHECK(snapshot.value().status.code != nf::ErrorCode::backlog_timeout);
    std::string wire;
    bool found = false;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!found && std::chrono::steady_clock::now() < deadline) {
        CHECK(peer.engine->poll_once(2).ok());
        if (!receive_frame(peer.peer, peer.buffered, wire, 2ms)) continue;
        found = field_is(wire, 11, "ZERO-DISABLED");
    }
    CHECK(found);
    peer.stop();
}

void test_flow_control_age_override(const std::filesystem::path& root) {
    PeerEngine peer{root / "flow-age-override", 1'000, 45};
    const auto receipt = peer.sender.send(nf::message<"D">().set<11>("FLOW-OVERRIDE").set<55>("AAPL"));
    CHECK(receipt.ok());
    std::this_thread::sleep_for(80ms);
    CHECK(peer.engine->poll_once(0).ok());
    const auto snapshot = peer.sender.receipt(receipt.value().id);
    CHECK(snapshot.ok() && snapshot.value().state == nf::ReceiptState::expired);
    CHECK(snapshot.value().status.code == nf::ErrorCode::backlog_timeout);
    CHECK(snapshot.value().msg_seq_num == 0);
    peer.stop();
}

void test_queued_active_callback_expiry(const std::filesystem::path& root) {
    using QueuedEngine = nf::Engine<nf::FlatSchema, nf::Queued>;
    nf::EngineConfig config;
    config.storage.directory = root / "queued-active-expiry";
    config.heartbeat_ms = 10'000;
    config.max_pending_age_ms = 50;
    config.write_stall_ms = 5'000;
    config.logout_timeout_ms = 100;
    config.queued.worker_threads = 1;
    auto opened = QueuedEngine::create(std::move(config));
    CHECK(opened.ok());
    std::optional<QueuedEngine> engine;
    engine.emplace(std::move(opened).value());
    std::atomic<std::uint64_t> receipt_id{0};
    std::atomic<bool> callback_done{false};
    std::atomic<bool> allow_send{false};
    CHECK(engine->on_session_active([&](nf::SessionContext& context) {
        while (!allow_send.load(std::memory_order_acquire)) std::this_thread::yield();
        const auto result = context.send(nf::message<"D">().set<11>("ACTIVE-OLD").set<55>("AAPL"));
        if (!result.ok()) return result.status();
        receipt_id.store(result.value().id, std::memory_order_release);
        callback_done.store(true, std::memory_order_release);
        return nf::Status::success();
    }).ok());
    CHECK(engine->add_listener({.name="listener", .endpoint={"127.0.0.1", 0}}).ok());
    auto added = engine->add_acceptor({.name="session", .listener="listener",
        .local_comp_id="SELL", .remote_comp_id="BUY"});
    CHECK(added.ok());
    const auto sender = added.value().sender<>();
    CHECK(engine->start().ok());
    const auto endpoint = engine->listener_endpoint("listener");
    CHECK(endpoint.ok());
    const auto peer = connect_loopback(endpoint.value().port);
    CHECK(peer >= 0);
    std::string buffered;
    CHECK(send_all(peer, logon(1)));
    const auto logon_deadline = std::chrono::steady_clock::now() + 3s;
    bool got_logon = false;
    while (!got_logon && std::chrono::steady_clock::now() < logon_deadline) {
        CHECK(engine->poll_once(2).ok());
        std::string wire;
        if (receive_frame(peer, buffered, wire, 2ms) && field_is(wire, 35, "A"))
            got_logon = true;
    }
    CHECK(got_logon);
    allow_send.store(true, std::memory_order_release);
    const auto active_deadline = std::chrono::steady_clock::now() + 2s;
    while (!callback_done.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < active_deadline) std::this_thread::yield();
    const auto id = receipt_id.load(std::memory_order_acquire);
    CHECK(callback_done.load(std::memory_order_acquire) && id != 0);
    const auto accepted = sender.receipt(id);
    CHECK(accepted.ok() && accepted.value().state == nf::ReceiptState::accepted);
    std::this_thread::sleep_for(80ms);
    CHECK(engine->poll_once(0).ok());
    const auto expired = sender.receipt(id);
    CHECK(expired.ok() && expired.value().state == nf::ReceiptState::expired);
    CHECK(expired.value().status.code == nf::ErrorCode::backlog_timeout);
    CHECK(expired.value().msg_seq_num == 0);
    std::string wire;
    CHECK(!receive_frame(peer, buffered, wire, 20ms));
    engine->request_stop();
    ::close(peer);
    for (std::size_t i = 0; i < 100 && engine->running(); ++i) {
        const auto status = engine->poll_once(2);
        CHECK(status.ok() || !engine->running());
    }
    CHECK(!engine->running());
}

void test_queued_dependent_expiry_keeps_delivery_open(const std::filesystem::path& root) {
    using QueuedEngine = nf::Engine<nf::FlatSchema, nf::Queued>;
    const auto storage = root / "queued-expiry";
    nf::EngineConfig config;
    config.storage.directory = storage;
    config.heartbeat_ms = 10'000;
    config.max_pending_age_ms = 50;
    config.write_stall_ms = 5'000;
    config.logout_timeout_ms = 100;
    config.queued.worker_threads = 1;
    auto opened = QueuedEngine::create(std::move(config));
    CHECK(opened.ok());
    std::optional<QueuedEngine> engine;
    engine.emplace(std::move(opened).value());
    std::atomic<std::uint64_t> dependent_id{0};
    std::atomic<std::size_t> callback_count{0};
    CHECK(engine->on_message([&](nf::SessionContext& context,
        const nf::FlatMessageView&, const nf::DeliveryInfo&) {
        const auto receipt = context.send(nf::message<"8">().set<17>("EXPIRY-REPLY"));
        if (!receipt.ok()) return receipt.status();
        dependent_id.store(receipt.value().id, std::memory_order_release);
        callback_count.fetch_add(1, std::memory_order_release);
        return nf::Status::success();
    }).ok());
    CHECK(engine->add_listener({.name="listener", .endpoint={"127.0.0.1", 0}}).ok());
    auto added = engine->add_acceptor({.name="session", .listener="listener",
        .local_comp_id="SELL", .remote_comp_id="BUY"});
    CHECK(added.ok());
    const auto sender = added.value().sender<>();
    CHECK(engine->start().ok());
    const auto endpoint = engine->listener_endpoint("listener");
    CHECK(endpoint.ok());
    const auto peer = connect_loopback(endpoint.value().port);
    CHECK(peer >= 0);
    std::string buffered;
    CHECK(send_all(peer, logon(1)));
    const auto logon_deadline = std::chrono::steady_clock::now() + 3s;
    bool got_logon = false;
    while (!got_logon && std::chrono::steady_clock::now() < logon_deadline) {
        CHECK(engine->poll_once(2).ok());
        std::string wire;
        if (receive_frame(peer, buffered, wire, 2ms) && field_is(wire, 35, "A"))
            got_logon = true;
    }
    CHECK(got_logon);
    const auto active_deadline = std::chrono::steady_clock::now() + 2s;
    while (!sender.can_send() && std::chrono::steady_clock::now() < active_deadline)
        CHECK(engine->poll_once(2).ok());
    CHECK(sender.can_send());

    CHECK(send_all(peer, encode(nf::message<"D">().set<11>("INBOUND-1").set<55>("AAPL"),
        "BUY", "SELL", 2)));
    const auto inbound_deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < inbound_deadline) {
        CHECK(engine->poll_once(2).ok());
        const auto snapshot = engine->snapshot("session");
        if (snapshot.ok() && snapshot.value().received_frames >= 1) break;
    }
    const auto received_snapshot = engine->snapshot("session");
    CHECK(received_snapshot.ok() && received_snapshot.value().received_frames >= 1);
    const auto callback_deadline = std::chrono::steady_clock::now() + 2s;
    while (callback_count.load(std::memory_order_acquire) == 0 &&
        std::chrono::steady_clock::now() < callback_deadline) std::this_thread::yield();
    const auto id = dependent_id.load(std::memory_order_acquire);
    CHECK(id != 0 && callback_count.load(std::memory_order_acquire) == 1);
    const auto accepted = sender.receipt(id);
    CHECK(accepted.ok() && accepted.value().state == nf::ReceiptState::accepted);

    std::this_thread::sleep_for(80ms);
    CHECK(engine->poll_once(0).ok());
    const auto expired = sender.receipt(id);
    CHECK(expired.ok() && expired.value().state == nf::ReceiptState::expired);
    CHECK(expired.value().status.code == nf::ErrorCode::backlog_timeout);
    CHECK(expired.value().msg_seq_num == 0);
    for (std::size_t i = 0; i < 50 && engine->running(); ++i) {
        if (engine->poll_once(2).ok()) continue;
        break;
    }
    engine->request_stop();
    ::close(peer);
    for (std::size_t i = 0; i < 100 && engine->running(); ++i) {
        const auto status = engine->poll_once(2);
        CHECK(status.ok() || !engine->running());
    }
    CHECK(!engine->running());
    engine.reset();

    constexpr std::string_view key = "FIX.4.4|4:SELL|3:BUY";
    std::string hex_key;
    constexpr char digits[] = "0123456789abcdef";
    for (const unsigned char byte : key) {
        hex_key.push_back(digits[byte >> 4]);
        hex_key.push_back(digits[byte & 0x0f]);
    }
    auto journal_dir = storage / "sessions";
    for (std::size_t offset = 0; offset < hex_key.size(); offset += 64)
        journal_dir /= hex_key.substr(offset, std::min<std::size_t>(64, hex_key.size() - offset));
    const auto journal_path = (journal_dir / "session.nfxj").string();
    auto journal_result = nf::Journal::open(nf::JournalConfig{journal_path, key,
        16U * 1024U * 1024U, 65'536, {}, 1024U * 1024U, 0});
    CHECK(journal_result.ok());
    bool found_incomplete = false;
    for (const auto& record : journal_result.value().records()) {
        if (record.kind == nf::RecordKind::inbound && record.seq == 2) {
            found_incomplete = !journal_result.value().has_delivery(record.id);
        }
    }
    CHECK(found_incomplete);
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "nimblefix-sender-expiry-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);
    try {
        test_expiry_and_zero_sequence(root);
        test_zero_disables_expiry(root);
        test_flow_control_age_override(root);
        test_queued_active_callback_expiry(root);
        test_queued_dependent_expiry_keeps_delivery_open(root);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(root, ignored);
        return 1;
    }
    std::filesystem::remove_all(root, ignored);
    return 0;
}
