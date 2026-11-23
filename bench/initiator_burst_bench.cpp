#include <nimblefix/codec.hpp>
#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/direct_queue.hpp>
#include <nimblefix/engine.hpp>
#include <nimblefix/fix44.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nf = nimblefix;
namespace fix = nimblefix::fix44;

namespace {
std::atomic<bool> g_count_malloc{false};
std::atomic<std::uint64_t> g_malloc_calls{0};
} // namespace

extern "C" {
void* __real_malloc(std::size_t n);
void* __wrap_malloc(std::size_t n) {
    if (g_count_malloc.load(std::memory_order_relaxed)) {
        g_malloc_calls.fetch_add(1, std::memory_order_relaxed);
    }
    return __real_malloc(n);
}
} // extern "C"

namespace {

[[nodiscard]] inline std::uint64_t now_ns() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

inline void spin_ns(std::uint64_t duration_ns) noexcept {
    if (duration_ns == 0) return;
    const std::uint64_t target = now_ns() + duration_ns;
    while (now_ns() < target) {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }
}

[[nodiscard]] std::string current_utc_timestamp() {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    std::tm utc{};
    ::gmtime_r(&ts.tv_sec, &utc);
    char buf[96];
    const auto ms = static_cast<unsigned>(ts.tv_nsec / 1'000'000L);
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d.%03u",
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
        utc.tm_hour, utc.tm_min, utc.tm_sec, ms);
    return std::string{buf};
}

struct Stats {
    double p50_ns{};
    double p95_ns{};
    double p99_ns{};
    double max_ns{};
};

[[nodiscard]] Stats compute_stats(std::vector<std::uint64_t> values) {
    Stats s{};
    if (values.empty()) return s;
    std::sort(values.begin(), values.end());
    auto at = [&](double q) -> double {
        const auto idx = std::min<std::size_t>(values.size() - 1,
            static_cast<std::size_t>(q * static_cast<double>(values.size() - 1)));
        return static_cast<double>(values[idx]);
    };
    s.p50_ns = at(0.50);
    s.p95_ns = at(0.95);
    s.p99_ns = at(0.99);
    s.max_ns = static_cast<double>(values.back());
    return s;
}

struct TempDir {
    std::string path;
    TempDir() {
        char pat[] = "/tmp/nf-iburst-XXXXXX";
        if (char* p = ::mkdtemp(pat)) path = p;
    }
    ~TempDir() {
        if (!path.empty()) std::filesystem::remove_all(path);
    }
};

// Format "<t_burst_ns>-<t_send_ns>" into ClOrdID(11) without heap allocation.
[[nodiscard]] std::string_view format_cl_ord_id(char* buf, std::size_t cap,
                                                std::uint64_t t_burst,
                                                std::uint64_t t_send) noexcept {
    auto r1 = std::to_chars(buf, buf + cap, t_burst);
    if (r1.ec != std::errc{} || r1.ptr >= buf + cap - 2) return {};
    *r1.ptr++ = '-';
    auto r2 = std::to_chars(r1.ptr, buf + cap, t_send);
    if (r2.ec != std::errc{}) return {};
    return {buf, static_cast<std::size_t>(r2.ptr - buf)};
}

[[nodiscard]] bool parse_cl_ord_id(std::string_view text,
                                   std::uint64_t& t_burst,
                                   std::uint64_t& t_send) noexcept {
    const auto dash = text.find('-');
    if (dash == std::string_view::npos) return false;
    const auto a = text.substr(0, dash);
    const auto b = text.substr(dash + 1);
    const auto r1 = std::from_chars(a.data(), a.data() + a.size(), t_burst);
    const auto r2 = std::from_chars(b.data(), b.data() + b.size(), t_send);
    return r1.ec == std::errc{} && r1.ptr == a.data() + a.size() &&
           r2.ec == std::errc{} && r2.ptr == b.data() + b.size();
}

// ============================================================================
// Part 0: Pre-TCP Message Construction + Encode micro-benchmark
// Compares Scheme A (SSO make() + optional), Scheme B (SSO Draft + native if),
// and Fixed<ByteCap, fix::field::...> on the exact same FIX 4.4 NewOrderSingle!
// ============================================================================
void run_message_construction_bench(std::size_t warmup, std::size_t samples) {
    const std::string utc = current_utc_timestamp();
    std::array<char, 2048> wire{};
    const nf::SessionHeader hdr{"BUY", "SELL", 1, utc};
    volatile std::size_t sink = 0;

    auto bench_fn = [&](const char* name, auto&& fn) {
        std::vector<std::uint64_t> lats;
        lats.reserve(samples);
        for (std::size_t i = 0; i < warmup; ++i) sink += fn(i);
        g_malloc_calls.store(0);
        g_count_malloc.store(true);
        for (std::size_t i = 0; i < samples; ++i) {
            const std::uint64_t t0 = now_ns();
            sink += fn(i);
            const std::uint64_t t1 = now_ns();
            lats.push_back(t1 - t0);
        }
        g_count_malloc.store(false);
        const auto s = compute_stats(std::move(lats));
        std::cout << "  " << std::left << std::setw(42) << name
                  << " p50=" << std::right << std::fixed << std::setprecision(1) << std::setw(6) << s.p50_ns << " ns"
                  << " | p99=" << std::setw(6) << s.p99_ns << " ns"
                  << " | allocs=" << g_malloc_calls.load() << '\n';
    };

    std::cout << "=== Part 0: Message Construction + FIX 4.4 Single-Pass Encode (" << samples
              << " samples, warmup " << warmup << ", alternating market/limit) ===\n";

    // Scheme A: SSO make() + std::optional
    bench_fn("Scheme A: make() + std::optional (SSO)", [&](std::size_t i) -> std::size_t {
        const bool is_limit = (i & 1U) != 0;
        const std::optional<nf::Decimal> opt_px = is_limit ? std::optional{nf::Decimal{15025, 2}} : std::nullopt;
        char id_buf[32];
        const auto cid = format_cl_ord_id(id_buf, sizeof(id_buf), 1000, i);
        auto order = fix::NewOrderSingle::make()
            .cl_ord_id(cid).handl_inst('1').symbol("AAPL").side('1')
            .transact_time(utc).order_qty(nf::Decimal{100, 0})
            .ord_type(is_limit ? '2' : '1').price(opt_px);
        const auto enc = nf::encode_fix44(order, hdr, wire);
        return enc.ok() ? enc.value() : 0;
    });

    // Scheme B: SSO Draft + native if + reset()
    fix::NewOrderSingle::Draft draft;
    bench_fn("Scheme B: Draft + native if + reset (SSO)", [&](std::size_t i) -> std::size_t {
        const bool is_limit = (i & 1U) != 0;
        char id_buf[32];
        const auto cid = format_cl_ord_id(id_buf, sizeof(id_buf), 1000, i);
        draft.reset();
        draft.cl_ord_id(cid).handl_inst('1').symbol("AAPL").side('1')
             .transact_time(utc).order_qty(nf::Decimal{100, 0})
             .ord_type(is_limit ? '2' : '1');
        if (is_limit) draft.price(nf::Decimal{15025, 2});
        const auto enc = nf::encode_fix44(draft, hdr, wire);
        return enc.ok() ? enc.value() : 0;
    });

    // Fixed<128, fix::field::...>
    using FixedOrder = fix::NewOrderSingle::Fixed<128,
        fix::field::ClOrdID, fix::field::HandlInst, fix::field::Symbol,
        fix::field::Side, fix::field::TransactTime, fix::field::OrderQty,
        fix::field::OrdType, fix::field::Price>;
    FixedOrder fixed;
    bench_fn("Strict:   Fixed<128, fix::field::...>", [&](std::size_t i) -> std::size_t {
        const bool is_limit = (i & 1U) != 0;
        char id_buf[32];
        const auto cid = format_cl_ord_id(id_buf, sizeof(id_buf), 1000, i);
        fixed.reset();
        (void)fixed.cl_ord_id(cid);
        (void)fixed.handl_inst('1');
        (void)fixed.symbol("AAPL");
        (void)fixed.side('1');
        (void)fixed.transact_time(utc);
        (void)fixed.order_qty(nf::Decimal{100, 0});
        (void)fixed.ord_type(is_limit ? '2' : '1');
        if (is_limit) (void)fixed.price(nf::Decimal{15025, 2});
        const auto enc = nf::encode_fix44(fixed, hdr, wire);
        return enc.ok() ? enc.value() : 0;
    });
    (void)sink;
}

// ============================================================================
// Real TCP Loopback Timestamping Acceptor Peer
// Receives FIX 4.4 frames over loopback TCP, records t_recv_ns = now_ns() on
// every NewOrderSingle(35=D), parses ClOrdID "<t_burst>-<t_send>", and optionally
// echoes ExecutionReport(35=8) for Ping-Pong mode.
// ============================================================================
class TimestampAcceptorPeer {
public:
    explicit TimestampAcceptorPeer(bool echo_er) : echo_er_(echo_er) {}

    bool start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) return false;
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 8) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        port_ = ntohs(addr.sin_port);
        stop_.store(false, std::memory_order_relaxed);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    void begin_capture(std::size_t expected_orders) {
        burst_lats_.clear();
        send_lats_.clear();
        burst_lats_.reserve(expected_orders);
        send_lats_.reserve(expected_orders);
        capture_.store(true, std::memory_order_release);
    }

    void end_capture() {
        capture_.store(false, std::memory_order_release);
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] std::uint64_t received_orders() const noexcept {
        return received_orders_.load(std::memory_order_acquire);
    }
    [[nodiscard]] const std::vector<std::uint64_t>& burst_latencies() const noexcept { return burst_lats_; }
    [[nodiscard]] const std::vector<std::uint64_t>& send_latencies() const noexcept { return send_lats_; }

private:
    static bool send_all(int fd, std::string_view bytes) noexcept {
        std::size_t off = 0;
        while (off < bytes.size()) {
            const ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            off += static_cast<std::size_t>(n);
        }
        return true;
    }

    void run() {
        const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) return;
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        timeval tv{};
        tv.tv_usec = 50'000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::string rx;
        rx.reserve(256U * 1024U);
        std::size_t rx_head = 0;
        char chunk[65536];
        std::array<nf::FieldView, 128> fields{};
        std::array<char, 2048> tx_buf{};
        std::uint64_t out_seq = 1;
        const std::string utc = current_utc_timestamp();

        while (!stop_.load(std::memory_order_acquire)) {
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                break;
            }
            if (n == 0) break;
            const std::uint64_t t_recv = now_ns();
            rx.append(chunk, static_cast<std::size_t>(n));

            while (rx_head < rx.size()) {
                nf::FrameView fv{};
                const auto st = nf::inspect_frame(std::string_view{rx.data() + rx_head, rx.size() - rx_head}, fv);
                if (st.code == nf::ErrorCode::need_more) break;
                if (!st.ok()) { rx_head = rx.size(); break; }
                rx_head += fv.consumed;

                nf::FlatMessageView msg{};
                if (!nf::parse_flat_verified(fv, fields, msg).ok()) continue;
                const auto mt = msg.msg_type();

                if (mt == "A") {
                    auto ack = nf::message<"A">().set<98>(0).set<108>(30);
                    const auto enc = nf::encode_fix44(ack, nf::SessionHeader{"SELL", "BUY", out_seq++, utc}, tx_buf);
                    if (enc.ok()) (void)send_all(fd, {tx_buf.data(), enc.value()});
                } else if (mt == "5") {
                    auto ack = nf::message<"5">();
                    const auto enc = nf::encode_fix44(ack, nf::SessionHeader{"SELL", "BUY", out_seq++, utc}, tx_buf);
                    if (enc.ok()) (void)send_all(fd, {tx_buf.data(), enc.value()});
                    ::close(fd);
                    return;
                } else if (mt == "D") {
                    const auto f11 = msg.find_unique(11);
                    if (capture_.load(std::memory_order_acquire) && f11.ok()) {
                        std::uint64_t t_burst = 0, t_send = 0;
                        if (parse_cl_ord_id(f11.value().value, t_burst, t_send)) {
                            burst_lats_.push_back(t_recv >= t_burst ? (t_recv - t_burst) : 0);
                            send_lats_.push_back(t_recv >= t_send ? (t_recv - t_send) : 0);
                        }
                    }
                    received_orders_.fetch_add(1, std::memory_order_release);

                    if (echo_er_) {
                        const auto cid = f11.ok() ? f11.value().value : std::string_view{"0-0"};
                        auto er = fix::ExecutionReport::make()
                            .order_id("ORD-1").exec_id("EX-1").cl_ord_id(cid)
                            .exec_type('0').ord_status('0').symbol("AAPL").side('1')
                            .leaves_qty(nf::Decimal{100, 0}).cum_qty(nf::Decimal{0, 0}).avg_px(nf::Decimal{0, 0});
                        const auto enc = nf::encode_fix44(er, nf::SessionHeader{"SELL", "BUY", out_seq++, utc}, tx_buf);
                        if (enc.ok()) (void)send_all(fd, {tx_buf.data(), enc.value()});
                    }
                }
            }
            if (rx_head == rx.size()) {
                rx.clear();
                rx_head = 0;
            } else if (rx_head > 65536) {
                rx.erase(0, rx_head);
                rx_head = 0;
            }
        }
        ::close(fd);
    }

    bool echo_er_{false};
    int listen_fd_{-1};
    std::uint16_t port_{0};
    std::atomic<bool> stop_{false};
    std::atomic<bool> capture_{false};
    std::atomic<std::uint64_t> received_orders_{0};
    std::vector<std::uint64_t> burst_lats_;
    std::vector<std::uint64_t> send_lats_;
    std::thread thread_;
};

enum class SendMode {
    direct_send,
    direct_batch_writev,
    direct_spsc_queue,
    engine_producer_lane,
};

const char* mode_name(SendMode m) noexcept {
    switch (m) {
    case SendMode::direct_send:          return "1. DirectInitiator::send() (1-thread, no queue)";
    case SendMode::direct_batch_writev:  return "2. DirectInitiator enqueue+flush (1-thr writev)";
    case SendMode::direct_spsc_queue:    return "3. DirectInitiator + DirectSendQueue (2-thr SPSC)";
    case SendMode::engine_producer_lane: return "4. Engine(busy_poll)+ProducerLane (2-thr SPSC)";
    }
    return "?";
}

struct ModeResult {
    Stats burst_oneway{}; // t_recv - t_burst_start (Coordinated-Omission-Free!)
    Stats send_oneway{};  // t_recv - t_send_stamp  (Per-order enqueue -> recv)
    Stats rtt{};          // Ping-Pong only
    double orders_per_sec{};
};

struct ErCounterHandler {
    std::atomic<std::uint64_t>* er_count{};
    [[nodiscard]] nf::Status on_message(const nf::FlatMessageView& msg, const nf::DeliveryInfo&) noexcept {
        if (msg.is<fix::ExecutionReport>() && er_count != nullptr) {
            er_count->fetch_add(1, std::memory_order_release);
        }
        return nf::Status::success();
    }
};

// Unified runner for Ping-Pong (burst_size==1, ping_pong==true) and Bursts (burst_size>1).
bool run_workload_for_mode(SendMode mode, bool ping_pong, std::size_t warmup_bursts,
                           std::size_t measured_bursts, std::size_t burst_size,
                           std::uint64_t strategy_compute_ns, ModeResult& out) {
    TimestampAcceptorPeer peer{ping_pong};
    if (!peer.start()) return false;

    TempDir dir;
    const std::string utc = current_utc_timestamp();
    std::atomic<std::uint64_t> er_count{0};
    const std::size_t total_warmup_orders = warmup_bursts * burst_size;
    const std::size_t total_measured_orders = measured_bursts * burst_size;

    auto wait_peer_orders = [&](std::uint64_t target) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (peer.received_orders() < target && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
    };

    std::vector<std::uint64_t> rtt_samples;
    if (ping_pong) rtt_samples.reserve(total_measured_orders);
    std::uint64_t measured_wall_ns = 0;

    if (mode == SendMode::direct_send || mode == SendMode::direct_batch_writev) {
        nf::DirectInitiatorConfig cfg{};
        cfg.host = "127.0.0.1";
        cfg.port = peer.port();
        cfg.storage_directory = dir.path;
        cfg.local_comp_id = "BUY";
        cfg.remote_comp_id = "SELL";
        cfg.journal_capacity_bytes = 64U * 1024U * 1024U;
        cfg.journal_segment_bytes = 8U * 1024U * 1024U;
        cfg.journal_max_records = 131'072;
        cfg.max_pending_messages = 1024;

        auto opened = nf::DirectInitiator<fix::Schema, ErCounterHandler>::open(std::move(cfg), ErCounterHandler{&er_count});
        if (!opened.ok()) { peer.stop(); return false; }
        auto init = std::move(opened).value();
        if (!init.connect().ok()) { peer.stop(); return false; }
        while (!init.is_active()) {
            (void)init.poll_once();
            std::this_thread::yield();
        }

        fix::NewOrderSingle::Draft order;
        auto run_bursts = [&](std::size_t n_bursts, bool record_rtt) {
            std::uint64_t sent_so_far = peer.received_orders();
            for (std::size_t b = 0; b < n_bursts; ++b) {
                const std::uint64_t t_burst = now_ns();
                for (std::size_t i = 0; i < burst_size; ++i) {
                    spin_ns(strategy_compute_ns);
                    const std::uint64_t t_send = now_ns();
                    char cid_buf[48];
                    const auto cid = format_cl_ord_id(cid_buf, sizeof(cid_buf), t_burst, t_send);
                    order.reset();
                    order.cl_ord_id(cid).handl_inst('1').symbol("AAPL").side('1')
                         .transact_time(utc).order_qty(nf::Decimal{100, 0}).ord_type('2')
                         .price(nf::Decimal{15025, 2});
                    const std::uint64_t expect_er = er_count.load(std::memory_order_relaxed) + 1;
                    if (mode == SendMode::direct_send) {
                        while (true) {
                            const auto r = init.send(order);
                            if (r.ok()) break;
                            (void)init.poll_once();
                        }
                    } else {
                        while (true) {
                            const auto r = init.enqueue(order);
                            if (r.ok()) break;
                            (void)init.flush();
                            (void)init.poll_once();
                        }
                    }
                    ++sent_so_far;
                    if (ping_pong) {
                        if (mode == SendMode::direct_batch_writev) (void)init.flush();
                        const auto er_deadline = now_ns() + 5'000'000'000ULL;
                        while (er_count.load(std::memory_order_acquire) < expect_er && now_ns() < er_deadline) {
                            (void)init.poll_once();
                        }
                        if (record_rtt) rtt_samples.push_back(now_ns() - t_send);
                    }
                }
                if (!ping_pong) {
                    if (mode == SendMode::direct_batch_writev) (void)init.flush();
                    while (init.wants_write()) (void)init.poll_once();
                    wait_peer_orders(sent_so_far);
                }
            }
        };

        run_bursts(warmup_bursts, false);
        wait_peer_orders(total_warmup_orders);
        peer.begin_capture(total_measured_orders);
        const std::uint64_t t0 = now_ns();
        run_bursts(measured_bursts, ping_pong);
        wait_peer_orders(total_warmup_orders + total_measured_orders);
        measured_wall_ns = now_ns() - t0;
        peer.end_capture();

        (void)init.logout();
        for (int i = 0; i < 50 && init.valid_transport(); ++i) (void)init.poll_once();
    } else if (mode == SendMode::direct_spsc_queue) {
        nf::DirectInitiatorConfig cfg{};
        cfg.host = "127.0.0.1";
        cfg.port = peer.port();
        cfg.storage_directory = dir.path;
        cfg.local_comp_id = "BUY";
        cfg.remote_comp_id = "SELL";
        cfg.journal_capacity_bytes = 64U * 1024U * 1024U;
        cfg.journal_segment_bytes = 8U * 1024U * 1024U;
        cfg.journal_max_records = 131'072;
        cfg.max_pending_messages = 1024;

        auto opened = nf::DirectInitiator<fix::Schema, ErCounterHandler>::open(std::move(cfg), ErCounterHandler{&er_count});
        if (!opened.ok()) { peer.stop(); return false; }
        auto init = std::move(opened).value();
        if (!init.connect().ok()) { peer.stop(); return false; }
        while (!init.is_active()) {
            (void)init.poll_once();
            std::this_thread::yield();
        }

        nf::DirectSendQueue queue{1024, 512};
        std::atomic<bool> io_stop{false};
        std::thread io_thread([&] {
            while (!io_stop.load(std::memory_order_acquire) || !queue.empty() || init.wants_write()) {
                const auto d = init.drain(queue, 64);
                (void)d;
                if (ping_pong || init.wants_write()) {
                    (void)init.poll_once();
                } else if (!d.ok() || d.value() == 0) {
#if defined(__x86_64__) || defined(__i386__)
                    __builtin_ia32_pause();
#endif
                }
            }
        });

        fix::NewOrderSingle::Draft order;
        auto run_bursts = [&](std::size_t n_bursts, bool record_rtt) {
            std::uint64_t sent_so_far = peer.received_orders();
            for (std::size_t b = 0; b < n_bursts; ++b) {
                const std::uint64_t t_burst = now_ns();
                for (std::size_t i = 0; i < burst_size; ++i) {
                    spin_ns(strategy_compute_ns);
                    const std::uint64_t t_send = now_ns();
                    char cid_buf[48];
                    const auto cid = format_cl_ord_id(cid_buf, sizeof(cid_buf), t_burst, t_send);
                    order.reset();
                    order.cl_ord_id(cid).handl_inst('1').symbol("AAPL").side('1')
                         .transact_time(utc).order_qty(nf::Decimal{100, 0}).ord_type('2')
                         .price(nf::Decimal{15025, 2});
                    const std::uint64_t expect_er = er_count.load(std::memory_order_relaxed) + 1;
                    while (!queue.try_push(order).ok()) {
#if defined(__x86_64__) || defined(__i386__)
                        __builtin_ia32_pause();
#endif
                    }
                    ++sent_so_far;
                    if (ping_pong) {
                        const auto er_deadline = now_ns() + 5'000'000'000ULL;
                        while (er_count.load(std::memory_order_acquire) < expect_er && now_ns() < er_deadline) {
#if defined(__x86_64__) || defined(__i386__)
                            __builtin_ia32_pause();
#endif
                        }
                        if (record_rtt) rtt_samples.push_back(now_ns() - t_send);
                    }
                }
                if (!ping_pong) wait_peer_orders(sent_so_far);
            }
        };

        run_bursts(warmup_bursts, false);
        wait_peer_orders(total_warmup_orders);
        peer.begin_capture(total_measured_orders);
        const std::uint64_t t0 = now_ns();
        run_bursts(measured_bursts, ping_pong);
        wait_peer_orders(total_warmup_orders + total_measured_orders);
        measured_wall_ns = now_ns() - t0;
        peer.end_capture();

        io_stop.store(true, std::memory_order_release);
        io_thread.join();
        (void)init.logout();
        for (int i = 0; i < 50 && init.valid_transport(); ++i) (void)init.poll_once();
    } else {
        // Engine<fix44::Schema, Inline> with busy_poll + SPSC ProducerLane
        nf::EngineConfig cfg{};
        cfg.storage.directory = dir.path;
        cfg.storage.journal_capacity_bytes = 64U * 1024U * 1024U;
        cfg.storage.segment_bytes = 8U * 1024U * 1024U;
        cfg.storage.journal_max_records = 131'072;
        cfg.max_pending_messages = 1024;
        cfg.max_pending_bytes = 4U * 1024U * 1024U;
        cfg.io_backend = nf::IoBackendConfig::busy_poll();

        auto created = nf::Engine<fix::Schema, nf::Inline>::create(cfg);
        if (!created.ok()) { peer.stop(); return false; }
        auto engine = std::move(created).value();
        (void)engine.on<fix::ExecutionReport>([&](nf::SessionContext&, const fix::ExecutionReportView&) {
            er_count.fetch_add(1, std::memory_order_release);
            return nf::Status::success();
        });
        auto h = engine.add_initiator({"ini", "BUY", "SELL", {"127.0.0.1", peer.port()}});
        if (!h.ok()) { peer.stop(); return false; }
        auto lane_res = h.value().producer_lane<fix::Schema>({.capacity_messages = 1024,
            .capacity_bytes = 1024U * 1024U, .receipt_capacity = 2048});
        if (!lane_res.ok()) { peer.stop(); return false; }
        auto lane = std::move(lane_res).value();

        std::thread io_thread([&] { (void)engine.run(); });
        while (true) {
            auto s = engine.snapshot("ini");
            if (s.ok() && s.value().state == nf::SessionState::active) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        fix::NewOrderSingle::Draft order;
        auto run_bursts = [&](std::size_t n_bursts, bool record_rtt) {
            std::uint64_t sent_so_far = peer.received_orders();
            for (std::size_t b = 0; b < n_bursts; ++b) {
                const std::uint64_t t_burst = now_ns();
                for (std::size_t i = 0; i < burst_size; ++i) {
                    spin_ns(strategy_compute_ns);
                    const std::uint64_t t_send = now_ns();
                    char cid_buf[48];
                    const auto cid = format_cl_ord_id(cid_buf, sizeof(cid_buf), t_burst, t_send);
                    order.reset();
                    order.cl_ord_id(cid).handl_inst('1').symbol("AAPL").side('1')
                         .transact_time(utc).order_qty(nf::Decimal{100, 0}).ord_type('2')
                         .price(nf::Decimal{15025, 2});
                    const std::uint64_t expect_er = er_count.load(std::memory_order_relaxed) + 1;
                    while (!lane.send(order).ok()) {
#if defined(__x86_64__) || defined(__i386__)
                        __builtin_ia32_pause();
#endif
                    }
                    ++sent_so_far;
                    if (ping_pong) {
                        const auto er_deadline = now_ns() + 5'000'000'000ULL;
                        while (er_count.load(std::memory_order_acquire) < expect_er && now_ns() < er_deadline) {
#if defined(__x86_64__) || defined(__i386__)
                            __builtin_ia32_pause();
#endif
                        }
                        if (record_rtt) rtt_samples.push_back(now_ns() - t_send);
                    }
                }
                if (!ping_pong) wait_peer_orders(sent_so_far);
            }
        };

        run_bursts(warmup_bursts, false);
        wait_peer_orders(total_warmup_orders);
        peer.begin_capture(total_measured_orders);
        const std::uint64_t t0 = now_ns();
        run_bursts(measured_bursts, ping_pong);
        wait_peer_orders(total_warmup_orders + total_measured_orders);
        measured_wall_ns = now_ns() - t0;
        peer.end_capture();

        engine.request_stop();
        io_thread.join();
    }

    out.burst_oneway = compute_stats(peer.burst_latencies());
    out.send_oneway = compute_stats(peer.send_latencies());
    if (ping_pong) out.rtt = compute_stats(std::move(rtt_samples));
    out.orders_per_sec = measured_wall_ns > 0
        ? (static_cast<double>(total_measured_orders) * 1e9 / static_cast<double>(measured_wall_ns))
        : 0.0;
    const bool complete = peer.burst_latencies().size() == total_measured_orders;
    peer.stop();
    if (!complete) {
        std::cerr << "INCOMPLETE CAPTURE for " << mode_name(mode) << ": got "
                  << peer.burst_latencies().size() << " / " << total_measured_orders << '\n';
        return false;
    }
    return true;
}

} // namespace

int main() {
    run_message_construction_bench(5'000, 30'000);

    const std::array<SendMode, 4> modes{
        SendMode::direct_send,
        SendMode::direct_batch_writev,
        SendMode::direct_spsc_queue,
        SendMode::engine_producer_lane,
    };

    // Workload 1: Ping-Pong (1 in-flight)
    std::cout << "\n=== Workload 1: Ping-Pong (1 in-flight, 5000 orders, 500 warmup, real TCP + FIX 4.4 + v2 Journal) ===\n";
    for (const auto m : modes) {
        ModeResult r{};
        if (!run_workload_for_mode(m, true, 500, 5'000, 1, 0, r)) {
            std::cerr << "FAILED: " << mode_name(m) << '\n';
            return 1;
        }
        std::cout << "  " << std::left << std::setw(50) << mode_name(m)
                  << " 1-way p50=" << std::right << std::fixed << std::setprecision(2) << std::setw(6) << r.send_oneway.p50_ns / 1000.0 << " us"
                  << " (p99=" << std::setw(6) << r.send_oneway.p99_ns / 1000.0 << " us)"
                  << " | RTT p50=" << std::setw(6) << r.rtt.p50_ns / 1000.0 << " us"
                  << " | " << static_cast<std::uint64_t>(r.orders_per_sec) << " rt/s\n";
    }

    // Workload 2: Dense Burst (32 orders/burst, 0 ns strategy compute)
    std::cout << "\n=== Workload 2: Dense Burst (200 bursts x 32 orders, 0 us compute) ===\n"
              << "    [CO-Free = t_recv - t_burst_start (true latency from burst arrival); Enq->Recv = t_recv - t_enqueue]\n";
    for (const auto m : modes) {
        ModeResult r{};
        if (!run_workload_for_mode(m, false, 20, 200, 32, 0, r)) {
            std::cerr << "FAILED: " << mode_name(m) << '\n';
            return 1;
        }
        std::cout << "  " << std::left << std::setw(50) << mode_name(m)
                  << " CO-Free p50=" << std::right << std::fixed << std::setprecision(2) << std::setw(6) << r.burst_oneway.p50_ns / 1000.0 << " us"
                  << " (p99=" << std::setw(6) << r.burst_oneway.p99_ns / 1000.0 << " us)"
                  << " | Enq->Recv p50=" << std::setw(6) << r.send_oneway.p50_ns / 1000.0 << " us"
                  << " | " << static_cast<std::uint64_t>(r.orders_per_sec) << " ord/s\n";
    }

    // Workload 3: Dense Burst + 2 us simulated strategy compute per order
    std::cout << "\n=== Workload 3: Dense Burst + 2 us Strategy Compute (200 bursts x 32 orders) ===\n";
    for (const auto m : modes) {
        ModeResult r{};
        if (!run_workload_for_mode(m, false, 20, 200, 32, 2'000, r)) {
            std::cerr << "FAILED: " << mode_name(m) << '\n';
            return 1;
        }
        std::cout << "  " << std::left << std::setw(50) << mode_name(m)
                  << " CO-Free p50=" << std::right << std::fixed << std::setprecision(2) << std::setw(6) << r.burst_oneway.p50_ns / 1000.0 << " us"
                  << " (p99=" << std::setw(6) << r.burst_oneway.p99_ns / 1000.0 << " us)"
                  << " | Enq->Recv p50=" << std::setw(6) << r.send_oneway.p50_ns / 1000.0 << " us"
                  << " | " << static_cast<std::uint64_t>(r.orders_per_sec) << " ord/s\n";
    }
    return 0;
}
