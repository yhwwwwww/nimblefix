#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/engine.hpp>
#include <nimblefix/fix44_sample.hpp>
#include <nimblefix/io_backend.hpp>
#include <nimblefix/message.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nf = nimblefix;
namespace fix = nimblefix::fix44_sample;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << __FILE__ << ":" << __LINE__ << ": failed: " #cond "\n";                   \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

struct TempDir {
    std::string path;
    TempDir() {
        char pat[] = "/tmp/nimblefix-backpressure-XXXXXX";
        char* p = ::mkdtemp(pat);
        if (!p) std::abort();
        path = p;
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

struct FdGuard {
    int fd{-1};
    ~FdGuard() { close(); }
    void close() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

std::string utc_now() {
    std::array<char, 32> buf{};
    const auto now = std::time(nullptr);
    std::tm utc{};
    ::gmtime_r(&now, &utc);
    ::strftime(buf.data(), buf.size(), "%Y%m%d-%H:%M:%S", &utc);
    return buf.data();
}

std::string encode_logon(std::string_view sender, std::string_view target, std::uint64_t seq) {
    std::array<char, 1024> out{};
    const auto ts = utc_now();
    auto msg = nf::message<"A">().set<98>(0).set<108>(30);
    const auto n = nf::encode_fix44(msg, {sender, target, seq, ts}, out);
    if (!n.ok()) return {};
    return {out.data(), n.value()};
}

std::string encode_order(std::string_view sender, std::string_view target, std::uint64_t seq,
                         std::string_view cl_ord_id, std::string_view text = {}) {
    std::vector<char> out(text.size() + 1024U);
    const auto ts = utc_now();
    if (!text.empty()) {
        auto msg = nf::message<"D">(nf::MessageLimits{16, text.size() + 512U})
                       .set<11>(cl_ord_id)
                       .set<55>("AAPL")
                       .set<58>(text);
        const auto n = nf::encode_fix44(msg, {sender, target, seq, ts}, out);
        if (!n.ok()) return {};
        return {out.data(), n.value()};
    }
    auto msg = nf::message<"D">().set<11>(cl_ord_id).set<55>("AAPL");
    const auto n = nf::encode_fix44(msg, {sender, target, seq, ts}, out);
    if (!n.ok()) return {};
    return {out.data(), n.value()};
}

int connect_loopback(std::uint16_t port, int rcvbuf = 0, int sndbuf = 0, bool nonblock = false) {
    const int flags = SOCK_STREAM | SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0);
    const int fd = ::socket(AF_INET, flags, 0);
    if (fd < 0) return -1;
    const int one = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (rcvbuf > 0) (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    if (sndbuf > 0) (void)::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc != 0) {
        if (!nonblock || errno != EINPROGRESS) {
            ::close(fd);
            return -1;
        }
        pollfd pfd{fd, POLLOUT, 0};
        if (::poll(&pfd, 1, 3000) <= 0) {
            ::close(fd);
            return -1;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            ::close(fd);
            return -1;
        }
    }
    return fd;
}

bool send_all(int fd, std::string_view bytes, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t off = 0;
    while (off < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) return false;
            pollfd pfd{fd, POLLOUT, 0};
            if (::poll(&pfd, 1, static_cast<int>(left.count())) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

bool receive_frame(int fd, std::string& buf, std::string& out,
                   std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 8192> chunk{};
    while (std::chrono::steady_clock::now() < deadline) {
        nf::FrameView frame{};
        const auto st = nf::inspect_frame(buf, frame);
        if (st.ok()) {
            out.assign(frame.bytes);
            buf.erase(0, frame.consumed);
            return true;
        }
        if (st.code != nf::ErrorCode::need_more) return false;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return false;
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, static_cast<int>(left.count())) <= 0) return false;
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n > 0) {
            buf.append(chunk.data(), static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return false;
    }
    return false;
}

bool peer_closed(int fd, std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 256> sink{};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return false;
        pollfd pfd{fd, static_cast<short>(POLLIN | POLLHUP), 0};
        const int r = ::poll(&pfd, 1, static_cast<int>(left.count()));
        if (r <= 0) return false;
        const ssize_t n = ::recv(fd, sink.data(), sink.size(), MSG_DONTWAIT);
        if (n == 0) return true;
        if (n < 0 && (errno == ECONNRESET || errno == EPIPE)) return true;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if ((pfd.revents & POLLHUP) != 0) return true;
            continue;
        }
    }
    return false;
}

bool frame_has(std::string_view wire, std::uint32_t tag, std::string_view val) {
    const std::string needle = std::to_string(tag) + "=" + std::string{val} + '\x01';
    return wire.find(needle) != std::string_view::npos;
}

std::string frame_field(std::string_view wire, std::uint32_t tag) {
    const std::string prefix = std::to_string(tag) + "=";
    const auto pos = wire.find(prefix);
    if (pos == std::string_view::npos) return {};
    const auto start = pos + prefix.size();
    const auto end = wire.find('\x01', start);
    if (end == std::string_view::npos) return {};
    return std::string{wire.substr(start, end - start)};
}

template <class EngineT>
std::uint16_t wait_listener_port(const EngineT& engine, std::string_view name) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto ep = engine.listener_endpoint(name);
        if (ep.ok() && ep.value().port != 0) return ep.value().port;
        std::this_thread::sleep_for(1ms);
    }
    return 0;
}

// =============================================================================
// Test 1: Inbound Backpressure (Slow Consumer -> read_paused -> Peer EAGAIN ->
//         Healthy Session Isolation -> Full Lossless In-Order Recovery)
// =============================================================================
void test_inbound_backpressure_and_recovery(nf::IoBackendConfig io_cfg, const char* label) {
    TempDir dir;
    nf::EngineConfig cfg{};
    cfg.storage.directory = dir.path;
    cfg.storage.journal_capacity_bytes = 16U * 1024U * 1024U;
    cfg.storage.segment_bytes = 2U * 1024U * 1024U;
    cfg.storage.journal_max_records = 32'768;
    cfg.io_backend = io_cfg;
    cfg.heartbeat_ms = 30'000;
    cfg.logout_timeout_ms = 1000;
    cfg.queued.worker_threads = 2; // 1 blocked by slow session, 1 free for healthy peer

    auto opened = nf::Engine<nf::FlatSchema, nf::Queued>::create(cfg);
    if (!opened.ok()) {
        if (io_cfg.kind == nf::IoBackendKind::io_uring &&
            (opened.status().offset == ENOSYS || opened.status().offset == EPERM)) {
            std::cout << "[SKIP] " << label << " (io_uring blocked by sandbox/kernel)\n";
            return;
        }
    }
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto engine = std::move(opened).value();

    std::mutex mu;
    std::condition_variable cv;
    bool first_entered = false;
    bool release_slow = false;
    std::vector<std::uint64_t> slow_seqs;
    std::atomic<int> healthy_count{0};
    std::atomic<int> error_count{0};

    CHECK(engine.on_message([&](nf::SessionContext& ctx, const nf::FlatMessageView& msg,
                                const nf::DeliveryInfo& info) -> nf::Status {
        const auto t = msg.find_unique(35);
        if (!t.ok() || t.value().value != "D") return nf::Status::success();
        if (ctx.session_name() == "slow-consumer") {
            if (info.msg_seq_num == 2) {
                {
                    std::lock_guard lock(mu);
                    first_entered = true;
                }
                cv.notify_all();
                std::unique_lock lock(mu);
                if (!cv.wait_for(lock, 10s, [&] { return release_slow; }))
                    return nf::Status{nf::ErrorCode::callback_failed};
            }
            {
                std::lock_guard lock(mu);
                slow_seqs.push_back(info.msg_seq_num);
            }
            cv.notify_all();
            return nf::Status::success();
        }
        if (ctx.session_name() == "healthy-peer") {
            healthy_count.fetch_add(1, std::memory_order_relaxed);
            const auto cid = msg.find_unique(11);
            auto er = nf::message<"8">()
                          .set<11>(cid.ok() ? cid.value().value : "OK")
                          .set<17>("EX-HEALTHY");
            return ctx.send(er).status();
        }
        return nf::Status::success();
    }).ok());

    CHECK(engine.on_error([&](const nf::ErrorEvent& ev) {
        std::cerr << "[" << label << " ERR] " << ev.session_name << ": "
                  << ev.status.message() << "\n";
        error_count.fetch_add(1, std::memory_order_relaxed);
    }).ok());

    CHECK(engine.add_listener({"main", {"127.0.0.1", 0}}).ok());

    nf::AcceptorConfig slow_acc{"slow-consumer", "main", "SELL", "SLOW-BUY"};
    nf::FlowControlConfig slow_fc{};
    slow_fc.max_pending_inbound_messages = 4;
    slow_fc.max_pending_inbound_bytes = 256U * 1024U;
    slow_fc.receive_pause_timeout_ms = 10'000;
    slow_fc.max_delivery_age_ms = 10'000;
    slow_acc.settings.flow_control = slow_fc;
    CHECK(engine.add_acceptor(std::move(slow_acc)).ok());

    nf::AcceptorConfig healthy_acc{"healthy-peer", "main", "SELL", "FAST-BUY"};
    CHECK(engine.add_acceptor(std::move(healthy_acc)).ok());

    nf::Status run_status{};
    std::thread owner([&] { run_status = engine.run(); });
    const std::uint16_t port = wait_listener_port(engine, "main");
    CHECK(port != 0);

    FdGuard slow_fd{connect_loopback(port, 0, 16384, /*nonblock=*/true)};
    FdGuard fast_fd{connect_loopback(port)};
    CHECK(slow_fd.fd >= 0 && fast_fd.fd >= 0);

    std::string slow_buf, fast_buf, wire;
    CHECK(send_all(slow_fd.fd, encode_logon("SLOW-BUY", "SELL", 1)));
    CHECK(receive_frame(slow_fd.fd, slow_buf, wire) && frame_has(wire, 35, "A"));
    CHECK(send_all(fast_fd.fd, encode_logon("FAST-BUY", "SELL", 1)));
    CHECK(receive_frame(fast_fd.fd, fast_buf, wire) && frame_has(wire, 35, "A"));

    // Prepare 300 x ~8 KB orders (~2.4 MB) — easily fills inbound queue + kernel TCP window.
    constexpr std::uint64_t kTotalOrders = 300;
    const std::string pad(8000, 'B');
    std::string payload_stream;
    payload_stream.reserve(kTotalOrders * 8300U);
    for (std::uint64_t i = 0; i < kTotalOrders; ++i) {
        payload_stream.append(
            encode_order("SLOW-BUY", "SELL", 2 + i, "IN-" + std::to_string(i), pad));
    }

    // Blast nonblocking bytes until peer actually hits EAGAIN (kernel TCP backpressure)
    // AND server reports read_paused == true.
    std::size_t sent_bytes = 0;
    bool hit_eagain = false;
    bool saw_read_paused = false;
    const auto bp_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < bp_deadline) {
        while (sent_bytes < payload_stream.size()) {
            const ssize_t n = ::send(slow_fd.fd, payload_stream.data() + sent_bytes,
                                     payload_stream.size() - sent_bytes, MSG_NOSIGNAL);
            if (n > 0) {
                sent_bytes += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                hit_eagain = true;
                break;
            }
            CHECK(false && "unexpected send error during inbound backpressure");
            break;
        }
        const auto snap = engine.snapshot("slow-consumer");
        if (snap.ok() && snap.value().read_paused) saw_read_paused = true;
        if (hit_eagain && saw_read_paused) break;
        std::this_thread::sleep_for(2ms);
    }

    {
        std::unique_lock lock(mu);
        CHECK(cv.wait_for(lock, 3s, [&] { return first_entered; }));
    }
    CHECK(saw_read_paused);
    CHECK(hit_eagain);
    CHECK(sent_bytes < payload_stream.size()); // Kernel TCP window truly held back the sender!

    // While slow-consumer is read-paused & TCP-blocked, healthy-peer must remain completely unaffected.
    for (int i = 0; i < 5; ++i) {
        CHECK(send_all(fast_fd.fd,
                       encode_order("FAST-BUY", "SELL", 2 + i, "OK-" + std::to_string(i))));
        CHECK(receive_frame(fast_fd.fd, fast_buf, wire) && frame_has(wire, 35, "8"));
    }
    CHECK(healthy_count.load() == 5);

    // Release slow consumer callback -> read_paused clears -> peer finishes sending remainder ->
    // all 200 orders must arrive intact in strict MsgSeqNum order 2..201.
    {
        std::lock_guard lock(mu);
        release_slow = true;
    }
    cv.notify_all();

    CHECK(send_all(slow_fd.fd,
                   std::string_view{payload_stream}.substr(sent_bytes), 10s));

    {
        std::unique_lock lock(mu);
        CHECK(cv.wait_for(lock, 10s, [&] { return slow_seqs.size() == kTotalOrders; }));
        CHECK(slow_seqs.size() == kTotalOrders);
        for (std::uint64_t i = 0; i < std::min<std::size_t>(slow_seqs.size(), kTotalOrders); ++i) {
            if (slow_seqs[i] != 2 + i) {
                std::cerr << "[" << label << "] out-of-order seq at " << i
                          << ": got " << slow_seqs[i] << " expected " << (2 + i) << "\n";
                CHECK(false);
                break;
            }
        }
    }

    // Verify read_paused recovered back to false and next_inbound_seq == 202.
    bool cleared_pause = false;
    const auto clear_deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < clear_deadline) {
        const auto snap = engine.snapshot("slow-consumer");
        if (snap.ok() && !snap.value().read_paused &&
            snap.value().next_inbound_seq == 2 + kTotalOrders) {
            cleared_pause = true;
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    CHECK(cleared_pause);
    CHECK(error_count.load() == 0);

    // Clean Logout on both sessions
    std::array<char, 256> lbuf{};
    const auto ts = utc_now();
    auto lo1 = nf::encode_fix44(nf::message<"5">(), {"SLOW-BUY", "SELL", 2 + kTotalOrders, ts}, lbuf);
    CHECK(lo1.ok() && send_all(slow_fd.fd, {lbuf.data(), lo1.value()}));
    CHECK(receive_frame(slow_fd.fd, slow_buf, wire) && frame_has(wire, 35, "5"));
    auto lo2 = nf::encode_fix44(nf::message<"5">(), {"FAST-BUY", "SELL", 7, ts}, lbuf);
    CHECK(lo2.ok() && send_all(fast_fd.fd, {lbuf.data(), lo2.value()}));
    CHECK(receive_frame(fast_fd.fd, fast_buf, wire) && frame_has(wire, 35, "5"));

    slow_fd.close();
    fast_fd.close();
    engine.request_stop();
    owner.join();
    CHECK(run_status.ok());
    CHECK(error_count.load() == 0);

    if (g_failures == 0) {
        std::cout << "[PASS] " << label << " (" << kTotalOrders
                  << " orders, TCP EAGAIN + read_paused + lossless recovery)\n";
    }
}

// =============================================================================
// Test 2: Inbound Backpressure Timeout Enforcement
//   (a) receive_pause_timeout_ms -> backlog_timeout while healthy peer survives
//   (b) max_delivery_age_ms      -> backlog_timeout on single stalled callback
// =============================================================================
void test_inbound_backpressure_timeouts() {
    // (a) receive_pause_timeout_ms
    {
        TempDir dir;
        nf::EngineConfig cfg{};
        cfg.storage.directory = dir.path;
        cfg.heartbeat_ms = 30'000;
        cfg.logout_timeout_ms = 500;
        cfg.queued.worker_threads = 2;

        auto opened = nf::Engine<nf::FlatSchema, nf::Queued>::create(cfg);
        CHECK(opened.ok());
        auto engine = std::move(opened).value();

        std::mutex mu;
        std::condition_variable cv;
        bool release_stalled = false;
        bool saw_backlog_timeout = false;

        CHECK(engine.on_message([&](nf::SessionContext& ctx, const nf::FlatMessageView& msg) -> nf::Status {
            const auto t = msg.find_unique(35);
            if (!t.ok() || t.value().value != "D") return nf::Status::success();
            if (ctx.session_name() == "stalled") {
                std::unique_lock lock(mu);
                (void)cv.wait_for(lock, 5s, [&] { return release_stalled; });
                return nf::Status::success();
            }
            auto er = nf::message<"8">().set<11>("ALIVE").set<17>("EX-ALIVE");
            return ctx.send(er).status();
        }).ok());

        CHECK(engine.on_error([&](const nf::ErrorEvent& ev) {
            std::lock_guard lock(mu);
            if (ev.session_name == "stalled" && ev.status.code == nf::ErrorCode::backlog_timeout) {
                saw_backlog_timeout = true;
                cv.notify_all();
            }
        }).ok());

        CHECK(engine.add_listener({"main", {"127.0.0.1", 0}}).ok());
        nf::AcceptorConfig stalled{"stalled", "main", "SELL", "STALL-BUY"};
        nf::FlowControlConfig fc{};
        fc.max_pending_inbound_messages = 2;
        fc.max_pending_inbound_bytes = 128U * 1024U;
        fc.receive_pause_timeout_ms = 150;
        fc.max_delivery_age_ms = 10'000;
        stalled.settings.flow_control = fc;
        CHECK(engine.add_acceptor(std::move(stalled)).ok());
        CHECK(engine.add_acceptor({"healthy", "main", "SELL", "OK-BUY"}).ok());

        std::thread owner([&] { (void)engine.run(); });
        const std::uint16_t port = wait_listener_port(engine, "main");
        CHECK(port != 0);

        FdGuard stall_fd{connect_loopback(port)};
        FdGuard ok_fd{connect_loopback(port)};
        std::string sbuf, obuf, wire;
        CHECK(send_all(stall_fd.fd, encode_logon("STALL-BUY", "SELL", 1)));
        CHECK(receive_frame(stall_fd.fd, sbuf, wire) && frame_has(wire, 35, "A"));
        CHECK(send_all(ok_fd.fd, encode_logon("OK-BUY", "SELL", 1)));
        CHECK(receive_frame(ok_fd.fd, obuf, wire) && frame_has(wire, 35, "A"));

        // Send 6 orders to saturate max_pending_inbound_messages=2 and trigger read_pause_since.
        for (int i = 0; i < 6; ++i) {
            (void)send_all(stall_fd.fd,
                           encode_order("STALL-BUY", "SELL", 2 + i, "ST-" + std::to_string(i)));
        }

        {
            std::unique_lock lock(mu);
            CHECK(cv.wait_for(lock, 3s, [&] { return saw_backlog_timeout; }));
        }
        CHECK(peer_closed(stall_fd.fd, 3s));

        // Healthy session must still work cleanly after stalled session was evicted.
        CHECK(send_all(ok_fd.fd, encode_order("OK-BUY", "SELL", 2, "ALIVE")));
        CHECK(receive_frame(ok_fd.fd, obuf, wire) && frame_has(wire, 35, "8"));

        {
            std::lock_guard lock(mu);
            release_stalled = true;
        }
        cv.notify_all();
        stall_fd.close();
        ok_fd.close();
        engine.request_stop();
        owner.join();
    }

    // (b) max_delivery_age_ms (single stuck callback without filling inbound queue)
    {
        TempDir dir;
        nf::EngineConfig cfg{};
        cfg.storage.directory = dir.path;
        cfg.heartbeat_ms = 30'000;
        cfg.logout_timeout_ms = 500;
        cfg.queued.worker_threads = 1;

        auto opened = nf::Engine<nf::FlatSchema, nf::Queued>::create(cfg);
        CHECK(opened.ok());
        auto engine = std::move(opened).value();

        std::mutex mu;
        std::condition_variable cv;
        bool release_cb = false;
        bool saw_backlog_timeout = false;

        CHECK(engine.on_message([&](nf::SessionContext&, const nf::FlatMessageView& msg) -> nf::Status {
            const auto t = msg.find_unique(35);
            if (!t.ok() || t.value().value != "D") return nf::Status::success();
            std::unique_lock lock(mu);
            (void)cv.wait_for(lock, 5s, [&] { return release_cb; });
            return nf::Status::success();
        }).ok());

        CHECK(engine.on_error([&](const nf::ErrorEvent& ev) {
            std::lock_guard lock(mu);
            if (ev.status.code == nf::ErrorCode::backlog_timeout) {
                saw_backlog_timeout = true;
                cv.notify_all();
            }
        }).ok());

        CHECK(engine.add_listener({"main", {"127.0.0.1", 0}}).ok());
        nf::AcceptorConfig acc{"slow-cb", "main", "SELL", "CB-BUY"};
        nf::FlowControlConfig fc{};
        fc.max_pending_inbound_messages = 32;
        fc.receive_pause_timeout_ms = 10'000;
        fc.max_delivery_age_ms = 150;
        acc.settings.flow_control = fc;
        CHECK(engine.add_acceptor(std::move(acc)).ok());

        std::thread owner([&] { (void)engine.run(); });
        const std::uint16_t port = wait_listener_port(engine, "main");
        CHECK(port != 0);

        FdGuard fd{connect_loopback(port)};
        std::string buf, wire;
        CHECK(send_all(fd.fd, encode_logon("CB-BUY", "SELL", 1)));
        CHECK(receive_frame(fd.fd, buf, wire) && frame_has(wire, 35, "A"));
        CHECK(send_all(fd.fd, encode_order("CB-BUY", "SELL", 2, "STUCK-1")));

        {
            std::unique_lock lock(mu);
            CHECK(cv.wait_for(lock, 3s, [&] { return saw_backlog_timeout; }));
        }
        CHECK(peer_closed(fd.fd, 3s));

        {
            std::lock_guard lock(mu);
            release_cb = true;
        }
        cv.notify_all();
        fd.close();
        engine.request_stop();
        owner.join();
    }

    if (g_failures == 0) {
        std::cout << "[PASS] inbound backpressure timeouts (receive_pause_timeout_ms + max_delivery_age_ms)\n";
    }
}

// =============================================================================
// Test 3: Outbound Backpressure + Recovery (Engine<Inline> -> Slow TCP Peer
//         triggers EAGAIN + queue_full -> Peer Resumes Reading -> Lossless Drain)
// =============================================================================
void test_outbound_backpressure_and_recovery() {
    TempDir dir;
    nf::EngineConfig cfg{};
    cfg.storage.directory = dir.path;
    cfg.storage.journal_capacity_bytes = 16U * 1024U * 1024U;
    cfg.storage.segment_bytes = 2U * 1024U * 1024U;
    cfg.storage.journal_max_records = 16'384;
    cfg.max_frame_bytes = 24U * 1024U;
    cfg.max_pending_messages = 16;
    cfg.max_pending_bytes = 256U * 1024U;
    cfg.max_pending_age_ms = 0; // Keep all queued messages until slow peer resumes
    cfg.write_stall_ms = 10'000;
    cfg.heartbeat_ms = 30'000;
    cfg.logout_timeout_ms = 1000;
    cfg.max_pending_sender_messages = 16;
    cfg.max_pending_sender_bytes = 256U * 1024U;

    auto opened = nf::Engine<nf::FlatSchema, nf::Inline>::create(cfg);
    CHECK(opened.ok());
    auto engine = std::move(opened).value();

    std::atomic<int> error_count{0};
    CHECK(engine.on_message([](nf::SessionContext&, const nf::FlatMessageView&) {
        return nf::Status::success();
    }).ok());
    CHECK(engine.on_error([&](const nf::ErrorEvent& ev) {
        std::cerr << "[outbound-recovery ERR] " << ev.session_name << ": "
                  << ev.status.message() << "\n";
        error_count.fetch_add(1, std::memory_order_relaxed);
    }).ok());

    CHECK(engine.add_listener({"main", {"127.0.0.1", 0}}).ok());
    auto handle = engine.add_acceptor({"out-bp", "main", "SELL", "SLOW-RX"});
    CHECK(handle.ok());
    auto sender = handle.value().sender<>();

    nf::Status run_status{};
    std::thread owner([&] { run_status = engine.run(); });
    const std::uint16_t port = wait_listener_port(engine, "main");
    CHECK(port != 0);

    // Peer uses tiny SO_RCVBUF and stops reading right after Logon!
    FdGuard peer{connect_loopback(port, /*rcvbuf=*/2048)};
    CHECK(peer.fd >= 0);
    std::string buf, wire;
    CHECK(send_all(peer.fd, encode_logon("SLOW-RX", "SELL", 1)));
    CHECK(receive_frame(peer.fd, buf, wire) && frame_has(wire, 35, "A"));

    const auto act_deadline = std::chrono::steady_clock::now() + 3s;
    while (!sender.can_send() && std::chrono::steady_clock::now() < act_deadline)
        std::this_thread::sleep_for(1ms);
    CHECK(sender.can_send());

    // Push 16 KB orders until kernel TCP + SessionDriver + Sender saturate with queue_full.
    const std::string pad(16'000, 'W');
    std::vector<std::uint64_t> receipt_ids;
    bool hit_queue_full = false;
    for (int i = 0; i < 600; ++i) {
        auto msg = nf::message<"D">(nf::MessageLimits{16, 17'000})
                       .set<11>("OUT-" + std::to_string(i))
                       .set<55>("AAPL")
                       .set<58>(pad);
        const auto res = sender.send(msg);
        if (!res.ok()) {
            CHECK(res.status().code == nf::ErrorCode::queue_full);
            hit_queue_full = true;
            break;
        }
        receipt_ids.push_back(res.value().id);
    }
    CHECK(hit_queue_full);
    CHECK(!receipt_ids.empty());

    // Last accepted message should still be queued (not yet written) while peer isn't reading.
    const auto mid_rec = sender.receipt(receipt_ids.back());
    CHECK(mid_rec.ok() && (mid_rec.value().state == nf::ReceiptState::accepted ||
                           mid_rec.value().state == nf::ReceiptState::appended));

    // Peer resumes reading: every single accepted order must arrive intact & strictly ordered!
    for (std::size_t i = 0; i < receipt_ids.size(); ++i) {
        CHECK(receive_frame(peer.fd, buf, wire, 10s));
        CHECK(frame_has(wire, 35, "D"));
        CHECK(frame_field(wire, 11) == "OUT-" + std::to_string(i));
        CHECK(frame_field(wire, 34) == std::to_string(2 + i));
    }

    // All receipts must transition to written and sender.can_send() must recover.
    bool all_written = false;
    const auto rec_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < rec_deadline) {
        all_written = sender.can_send();
        for (const auto id : receipt_ids) {
            const auto r = sender.receipt(id);
            if (!r.ok() || r.value().state != nf::ReceiptState::written) {
                all_written = false;
                break;
            }
        }
        if (all_written) break;
        std::this_thread::sleep_for(2ms);
    }
    CHECK(all_written);

    // Send one fresh post-recovery order to verify normal traffic resumes with next seq.
    auto post = nf::message<"D">().set<11>("AFTER-RECOVERY").set<55>("MSFT");
    const auto post_res = sender.send(post);
    CHECK(post_res.ok());
    CHECK(receive_frame(peer.fd, buf, wire, 5s));
    CHECK(frame_field(wire, 11) == "AFTER-RECOVERY");
    CHECK(frame_field(wire, 34) == std::to_string(2 + receipt_ids.size()));

    // Clean Logout
    std::array<char, 256> lbuf{};
    const auto lo = nf::encode_fix44(nf::message<"5">(), {"SLOW-RX", "SELL", 2, utc_now()}, lbuf);
    CHECK(lo.ok() && send_all(peer.fd, {lbuf.data(), lo.value()}));
    CHECK(receive_frame(peer.fd, buf, wire) && frame_has(wire, 35, "5"));
    peer.close();
    engine.request_stop();
    owner.join();
    CHECK(run_status.ok());
    CHECK(error_count.load() == 0);

    if (g_failures == 0) {
        std::cout << "[PASS] outbound backpressure + recovery (" << receipt_ids.size()
                  << " buffered orders drained losslessly)\n";
    }
}

// =============================================================================
// Test 4: DirectInitiator Real-TCP Outbound Backpressure + Recovery
// =============================================================================
struct DirectBpHandler {
    int errors{0};
    nf::Status on_message(const nf::FlatMessageView&, const nf::DeliveryInfo&) noexcept {
        return nf::Status::success();
    }
    void on_error(nf::Status st) noexcept {
        std::cerr << "[DirectInitiator BP ERR] " << st.message() << "\n";
        ++errors;
    }
};

void test_direct_initiator_outbound_backpressure_and_recovery() {
    TempDir dir;

    // Minimal raw TCP acceptor with tiny SO_RCVBUF (set BEFORE listen() so the TCP handshake
    // negotiates a small receive window instead of shrinking it post-accept!) that stops reading after Logon.
    FdGuard listen_fd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    CHECK(listen_fd.fd >= 0);
    const int one = 1;
    (void)::setsockopt(listen_fd.fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    const int small_rcv = 2048;
    (void)::setsockopt(listen_fd.fd, SOL_SOCKET, SO_RCVBUF, &small_rcv, sizeof(small_rcv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(::bind(listen_fd.fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    CHECK(::listen(listen_fd.fd, 4) == 0);
    socklen_t alen = sizeof(addr);
    CHECK(::getsockname(listen_fd.fd, reinterpret_cast<sockaddr*>(&addr), &alen) == 0);
    const std::uint16_t port = ntohs(addr.sin_port);

    std::atomic<bool> start_draining{false};
    std::atomic<std::size_t> expected_orders{0};
    std::atomic<std::size_t> server_received_orders{0};
    std::vector<std::string> server_cl_ids;
    std::vector<std::string> server_seqs;
    std::atomic<bool> server_ok{true};

    std::thread server_thread([&] {
        FdGuard conn{::accept4(listen_fd.fd, nullptr, nullptr, SOCK_CLOEXEC)};
        if (conn.fd < 0) {
            server_ok = false;
            return;
        }
        const int nodelay = 1;
        (void)::setsockopt(conn.fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        std::string buf, wire;
        if (!receive_frame(conn.fd, buf, wire, 5s) || !frame_has(wire, 35, "A")) {
            server_ok = false;
            return;
        }
        if (!send_all(conn.fd, encode_logon("SELL", "BUY", 1))) {
            server_ok = false;
            return;
        }
        // Intentionally stop reading until DirectInitiator fills kernel TCP + tx_ring_!
        const auto wait_deadline = std::chrono::steady_clock::now() + 5s;
        while (!start_draining.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < wait_deadline) {
            std::this_thread::sleep_for(2ms);
        }
        const std::size_t target = expected_orders.load(std::memory_order_acquire);
        while (server_cl_ids.size() < target) {
            if (!receive_frame(conn.fd, buf, wire, 10s)) {
                server_ok = false;
                return;
            }
            if (frame_has(wire, 35, "D")) {
                server_cl_ids.push_back(frame_field(wire, 11));
                server_seqs.push_back(frame_field(wire, 34));
                server_received_orders.store(server_cl_ids.size(), std::memory_order_release);
            }
        }
        // Handle graceful Logout
        if (receive_frame(conn.fd, buf, wire, 5s) && frame_has(wire, 35, "5")) {
            std::array<char, 256> lbuf{};
            const auto lo = nf::encode_fix44(nf::message<"5">(), {"SELL", "BUY", 2, utc_now()}, lbuf);
            if (lo.ok()) (void)send_all(conn.fd, {lbuf.data(), lo.value()});
        }
    });

    nf::DirectInitiatorConfig cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.storage_directory = dir.path;
    cfg.local_comp_id = "BUY";
    cfg.remote_comp_id = "SELL";
    cfg.max_frame_bytes = 16'384;
    cfg.max_pending_messages = 16;
    cfg.write_stall_ms = 10'000;
    cfg.heartbeat_ms = 30'000;
    cfg.logout_timeout_ms = 5000;
    cfg.journal_capacity_bytes = 16U * 1024U * 1024U;
    cfg.journal_segment_bytes = 2U * 1024U * 1024U;
    cfg.journal_max_records = 16'384;

    auto opened = nf::DirectInitiator<fix::Schema, DirectBpHandler>::open(cfg);
    CHECK(opened.ok());
    auto init = std::move(opened).value();
    CHECK(init.connect().ok());

    const auto act_deadline = std::chrono::steady_clock::now() + 5s;
    while (!init.is_active() && std::chrono::steady_clock::now() < act_deadline) {
        CHECK(init.poll_once().ok());
        std::this_thread::sleep_for(1ms);
    }
    CHECK(init.is_active());

    // Push ~12 KB orders until tx_ring_ saturates with ErrorCode::queue_full under real TCP EAGAIN.
    const std::string pad(12'000, 'D');
    std::size_t accepted = 0;
    bool hit_queue_full = false;
    for (int i = 0; i < 600; ++i) {
        const auto seq_before = init.next_outbound_seq();
        const std::string cid = "DI-" + std::to_string(i) + "-" + pad;
        const auto st = init.send_stream<fix::NewOrderSingle>([&](auto& w) {
            w.cl_ord_id(cid)
             .symbol("AAPL")
             .side('1')
             .transact_time("20261126-12:00:00.000")
             .order_qty(nf::Decimal{100, 0})
             .ord_type('1');
        });
        if (!st.ok()) {
            CHECK(st.status().code == nf::ErrorCode::queue_full);
            CHECK(init.next_outbound_seq() == seq_before); // Sequence MUST NOT advance on queue_full!
            hit_queue_full = true;
            break;
        }
        ++accepted;
    }
    CHECK(hit_queue_full);
    CHECK(accepted > 0);
    CHECK(init.wants_write());

    // Tell slow server to resume reading (accepted + 1 post-recovery order)
    expected_orders.store(accepted + 1, std::memory_order_release);
    start_draining.store(true, std::memory_order_release);

    const auto drain_deadline = std::chrono::steady_clock::now() + 10s;
    while (init.wants_write() && std::chrono::steady_clock::now() < drain_deadline) {
        CHECK(init.poll_once().ok());
        std::this_thread::sleep_for(1ms);
    }
    CHECK(!init.wants_write());

    // Send 1 post-recovery order and drain it cleanly.
    CHECK(init.send_stream<fix::NewOrderSingle>([&](auto& w) {
        w.cl_ord_id("DI-AFTER-RECOVERY")
         .symbol("MSFT")
         .side('2')
         .transact_time("20261126-12:00:01.000")
         .order_qty(nf::Decimal{50, 0})
         .ord_type('1');
    }).ok());
    while ((init.wants_write() ||
            server_received_orders.load(std::memory_order_acquire) < accepted + 1) &&
           std::chrono::steady_clock::now() < drain_deadline) {
        CHECK(init.poll_once().ok());
        std::this_thread::sleep_for(1ms);
    }
    CHECK(!init.wants_write());
    CHECK(server_received_orders.load(std::memory_order_acquire) == accepted + 1);

    // Graceful Logout
    CHECK(init.logout("done").ok());
    const auto lo_deadline = std::chrono::steady_clock::now() + 5s;
    while (init.valid_transport() && std::chrono::steady_clock::now() < lo_deadline) {
        (void)init.poll_once();
        std::this_thread::sleep_for(1ms);
    }

    server_thread.join();
    CHECK(server_ok.load());
    CHECK(init.handler().errors == 0);
    CHECK(server_cl_ids.size() == accepted + 1);
    for (std::size_t i = 0; i < std::min(accepted, server_cl_ids.size()); ++i) {
        const std::string expected_prefix = "DI-" + std::to_string(i) + "-";
        if (server_cl_ids[i].rfind(expected_prefix, 0) != 0 ||
            server_seqs[i] != std::to_string(2 + i)) {
            std::cerr << "[DirectInitiator BP] mismatch at " << i << " seq=" << server_seqs[i] << "\n";
            CHECK(false);
            break;
        }
    }
    if (server_cl_ids.size() == accepted + 1) {
        CHECK(server_cl_ids.back() == "DI-AFTER-RECOVERY");
        CHECK(server_seqs.back() == std::to_string(2 + accepted));
    }

    if (g_failures == 0) {
        std::cout << "[PASS] DirectInitiator real-TCP outbound backpressure + recovery ("
                  << accepted << " buffered orders drained losslessly)\n";
    }
}

// =============================================================================
// Test 5: Bidirectional Simultaneous Backpressure / Anti-Deadlock Regression
//         (Engine <-> Engine blasting ~1.9 MB each way simultaneously)
// =============================================================================
void test_bidirectional_simultaneous_backpressure() {
    TempDir dir_acc;
    TempDir dir_ini;

    constexpr int kOrdersEachWay = 240;
    const std::string pad(8000, 'X');

    auto make_cfg = [](const std::string& storage) {
        nf::EngineConfig c{};
        c.storage.directory = storage;
        c.storage.journal_capacity_bytes = 12U * 1024U * 1024U;
        c.storage.segment_bytes = 2U * 1024U * 1024U;
        c.storage.journal_max_records = 32'768;
        c.max_frame_bytes = 24U * 1024U;
        c.max_pending_messages = 32;
        c.max_pending_bytes = 512U * 1024U;
        c.max_pending_age_ms = 0;
        c.write_stall_ms = 15'000;
        c.heartbeat_ms = 30'000;
        c.logout_timeout_ms = 2000;
        c.max_pending_sender_messages = 32;
        c.max_pending_sender_bytes = 512U * 1024U;
        return c;
    };

    auto acc_res = nf::Engine<nf::FlatSchema, nf::Inline>::create(make_cfg(dir_acc.path));
    auto ini_res = nf::Engine<nf::FlatSchema, nf::Inline>::create(make_cfg(dir_ini.path));
    CHECK(acc_res.ok() && ini_res.ok());
    auto acceptor = std::move(acc_res).value();
    auto initiator = std::move(ini_res).value();

    std::atomic<int> acc_got{0}, ini_got{0}, errors{0};
    std::mutex seq_mu;
    std::vector<std::uint64_t> acc_seqs, ini_seqs;

    CHECK(acceptor.on_message([&](nf::SessionContext&, const nf::FlatMessageView& msg,
                                  const nf::DeliveryInfo& info) {
        const auto t = msg.find_unique(35);
        if (t.ok() && t.value().value == "D") {
            std::lock_guard lock(seq_mu);
            acc_seqs.push_back(info.msg_seq_num);
            acc_got.fetch_add(1, std::memory_order_release);
        }
        return nf::Status::success();
    }).ok());
    CHECK(initiator.on_message([&](nf::SessionContext&, const nf::FlatMessageView& msg,
                                   const nf::DeliveryInfo& info) {
        const auto t = msg.find_unique(35);
        if (t.ok() && t.value().value == "D") {
            std::lock_guard lock(seq_mu);
            ini_seqs.push_back(info.msg_seq_num);
            ini_got.fetch_add(1, std::memory_order_release);
        }
        return nf::Status::success();
    }).ok());

    auto on_err = [&](const nf::ErrorEvent& ev) {
        std::cerr << "[bidir ERR] " << ev.session_name << ": " << ev.status.message() << "\n";
        errors.fetch_add(1, std::memory_order_relaxed);
    };
    CHECK(acceptor.on_error(on_err).ok());
    CHECK(initiator.on_error(on_err).ok());

    CHECK(acceptor.add_listener({"main", {"127.0.0.1", 0}}).ok());
    auto acc_h = acceptor.add_acceptor({"acc", "main", "SELL", "BUY"});
    CHECK(acc_h.ok());
    auto acc_sender = acc_h.value().sender<>();

    nf::Status acc_run{}, ini_run{};
    std::thread acc_thread([&] { acc_run = acceptor.run(); });
    const std::uint16_t port = wait_listener_port(acceptor, "main");
    CHECK(port != 0);

    auto ini_h = initiator.add_initiator({"ini", "BUY", "SELL", {"127.0.0.1", port}});
    CHECK(ini_h.ok());
    auto ini_sender = ini_h.value().sender<>();

    std::thread ini_thread([&] { ini_run = initiator.run(); });

    const auto act_deadline = std::chrono::steady_clock::now() + 5s;
    while ((!acc_sender.can_send() || !ini_sender.can_send()) &&
           std::chrono::steady_clock::now() < act_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(acc_sender.can_send() && ini_sender.can_send());

    // Two producer threads blast simultaneously from both ends, retrying on queue_full.
    std::atomic<int> acc_backpressure_retries{0}, ini_backpressure_retries{0};
    auto blast = [&](auto& sender, const char* prefix, std::atomic<int>& retries) {
        const auto send_deadline = std::chrono::steady_clock::now() + 15s;
        for (int i = 0; i < kOrdersEachWay; ++i) {
            auto msg = nf::message<"D">(nf::MessageLimits{16, 8500})
                           .set<11>(std::string{prefix} + std::to_string(i))
                           .set<55>("AAPL")
                           .set<58>(pad);
            while (std::chrono::steady_clock::now() < send_deadline) {
                const auto r = sender.send(msg);
                if (r.ok()) break;
                if (r.status().code == nf::ErrorCode::queue_full) {
                    retries.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(1ms);
                    continue;
                }
                std::cerr << "[bidir " << prefix << "] unexpected send err: "
                          << r.status().message() << "\n";
                CHECK(false);
                return;
            }
        }
    };

    std::thread p1([&] { blast(acc_sender, "A2I-", acc_backpressure_retries); });
    std::thread p2([&] { blast(ini_sender, "I2A-", ini_backpressure_retries); });
    p1.join();
    p2.join();

    const auto done_deadline = std::chrono::steady_clock::now() + 15s;
    while ((acc_got.load(std::memory_order_acquire) < kOrdersEachWay ||
            ini_got.load(std::memory_order_acquire) < kOrdersEachWay) &&
           std::chrono::steady_clock::now() < done_deadline) {
        std::this_thread::sleep_for(2ms);
    }
    CHECK(acc_got.load() == kOrdersEachWay);
    CHECK(ini_got.load() == kOrdersEachWay);
    {
        std::lock_guard lock(seq_mu);
        for (int i = 0; i < std::min<int>(static_cast<int>(acc_seqs.size()), kOrdersEachWay); ++i)
            CHECK(acc_seqs[static_cast<std::size_t>(i)] == static_cast<std::uint64_t>(2 + i));
        for (int i = 0; i < std::min<int>(static_cast<int>(ini_seqs.size()), kOrdersEachWay); ++i)
            CHECK(ini_seqs[static_cast<std::size_t>(i)] == static_cast<std::uint64_t>(2 + i));
    }

    initiator.request_stop();
    ini_thread.join();
    acceptor.request_stop();
    acc_thread.join();

    CHECK(ini_run.ok());
    CHECK(acc_run.ok());
    CHECK(errors.load() == 0);

    if (g_failures == 0) {
        std::cout << "[PASS] bidirectional simultaneous backpressure ("
                  << kOrdersEachWay << "x2 orders, retries acc="
                  << acc_backpressure_retries.load() << " ini="
                  << ini_backpressure_retries.load() << ", zero deadlock)\n";
    }
}

} // namespace

int main() {
    test_inbound_backpressure_and_recovery(nf::IoBackendConfig::epoll(), "inbound backpressure [epoll]");
    if (nf::io_uring_available()) {
        test_inbound_backpressure_and_recovery(nf::IoBackendConfig::io_uring(), "inbound backpressure [io_uring]");
    }
    test_inbound_backpressure_timeouts();
    test_outbound_backpressure_and_recovery();
    test_direct_initiator_outbound_backpressure_and_recovery();
    test_bidirectional_simultaneous_backpressure();
    return g_failures == 0 ? 0 : 1;
}
