#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/fix44.hpp>
#include <nimblefix/group.hpp>
#include <nimblefix/message.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <new>
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
bool g_track_alloc = false;
std::uint64_t g_alloc_calls = 0;
} // namespace

extern "C" void* __real_malloc(std::size_t);
extern "C" void* __wrap_malloc(std::size_t size) {
    if (g_track_alloc) ++g_alloc_calls;
    return __real_malloc(size);
}
void* operator new(std::size_t size) {
    if (auto* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

inline std::uint64_t now_ns() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

// Exact 0.1.0 FIX 4.4 NewOrderSingle parity workload:
// 7 scalar business fields (incl. Price) + 1 NoPartyIDs (453) repeating group with 3 fields (448, 447, 452).
using OrderShape = nf::Fields<
    nf::Field<11, nf::FieldKind::string>,
    nf::Field<55, nf::FieldKind::string>,
    nf::Field<54, nf::FieldKind::character>,
    nf::Field<60, nf::FieldKind::string>,
    nf::Field<38, nf::FieldKind::decimal>,
    nf::Field<40, nf::FieldKind::character>,
    nf::Field<44, nf::FieldKind::decimal>,
    nf::Field<453, nf::FieldKind::signed_integer>>;

using PartyEntryShape = nf::Fields<
    nf::Field<448, nf::FieldKind::string>,
    nf::Field<447, nf::FieldKind::character>,
    nf::Field<452, nf::FieldKind::signed_integer>>;

using PartyEntryFixed = nf::FixedShapeMessage<"__entry__", PartyEntryShape, 64, fix::NoPartyIDsGroupEntryPolicy>;
using PartyGroupFixed = nf::GroupBuilder<453, 448, PartyEntryFixed, 4>;

// Zero-allocation reusable NewOrderSingle with attached NoPartyIDs repeating group.
struct ReusablePartyOrder {
    using schema_type = fix::Schema;
    fix::NewOrderSingleFixed<OrderShape, 128> body;
    PartyGroupFixed parties;
    mutable std::array<nf::detail::RepeatingGroupView, 1> group_views{};

    [[nodiscard]] nf::Status populate(std::string_view cl_ord_id) noexcept {
        body.reset();
        parties.reset();
        if (const auto s = body.cl_ord_id(cl_ord_id); !s.ok()) return s;
        if (const auto s = body.symbol("AAPL"); !s.ok()) return s;
        if (const auto s = body.side('1'); !s.ok()) return s;
        if (const auto s = body.transact_time("20261123-00:00:00.000"); !s.ok()) return s;
        if (const auto s = body.order_qty(nf::Decimal{100, 0}); !s.ok()) return s;
        if (const auto s = body.ord_type('2'); !s.ok()) return s;
        if (const auto s = body.price(nf::Decimal{18525, 2}); !s.ok()) return s;

        PartyEntryFixed entry;
        if (const auto s = entry.set<448>("CLIENT1"); !s.ok()) return s;
        if (const auto s = entry.set<447>('D'); !s.ok()) return s;
        if (const auto s = entry.set<452>(std::int64_t{1}); !s.ok()) return s;
        if (const auto s = parties.add(std::move(entry)); !s.ok()) return s;
        return body.template set<453>(static_cast<std::int64_t>(parties.size()));
    }

    [[nodiscard]] nf::detail::MessageView view() const noexcept {
        auto v = body.view();
        if (!v.status.ok()) return v;
        if (!parties.status().ok()) {
            v.status = parties.status();
            return v;
        }
        group_views[0] = parties.view();
        v.groups = group_views.data();
        v.group_count = 1;
        return v;
    }
};

// Exact 0.1.0 FIX 4.4 ExecutionReport (OrderAck) parity workload: 11 business fields.
using ErShape = nf::Fields<
    nf::Field<37, nf::FieldKind::string>,
    nf::Field<11, nf::FieldKind::string>,
    nf::Field<17, nf::FieldKind::string>,
    nf::Field<150, nf::FieldKind::character>,
    nf::Field<39, nf::FieldKind::character>,
    nf::Field<54, nf::FieldKind::character>,
    nf::Field<38, nf::FieldKind::decimal>,
    nf::Field<151, nf::FieldKind::decimal>,
    nf::Field<14, nf::FieldKind::decimal>,
    nf::Field<6, nf::FieldKind::decimal>,
    nf::Field<55, nf::FieldKind::string>>;

fix::ExecutionReportFixed<ErShape, 128> make_exec_report() {
    fix::ExecutionReportFixed<ErShape, 128> er;
    (void)er.order_id("ORDER-1");
    (void)er.cl_ord_id("ORD-000001");
    (void)er.exec_id("EXEC-1");
    (void)er.exec_type('0');
    (void)er.ord_status('0');
    (void)er.side('1');
    (void)er.order_qty(nf::Decimal{100, 0});
    (void)er.leaves_qty(nf::Decimal{100, 0});
    (void)er.cum_qty(nf::Decimal{0, 0});
    (void)er.avg_px(nf::Decimal{0, 0});
    (void)er.symbol("AAPL");
    return er;
}

// Zero-syscall in-memory DirectTransport to measure pure pre-TCP FIX engine latency
// (Codec + full 912-field FIX 4.4 Schema validation + SessionCore + v2 mmap Journal + Handler callback).
class InMemoryBenchTransport {
public:
    [[nodiscard]] nf::Status connect(std::string_view, std::uint16_t) noexcept {
        open_ = true;
        rx_head_ = 0;
        rx_tail_ = 0;
        return nf::Status::success();
    }
    [[nodiscard]] nf::Status finish_connect() noexcept { return nf::Status::success(); }

    [[nodiscard]] nf::TransportIoResult read(std::span<char> output) noexcept {
        nf::TransportIoResult res{};
        if (!open_) {
            res.peer_closed = true;
            return res;
        }
        const std::size_t avail = rx_tail_ - rx_head_;
        if (avail == 0) {
            res.would_block = true;
            return res;
        }
        const std::size_t n = std::min(avail, output.size());
        std::memcpy(output.data(), rx_.data() + rx_head_, n);
        rx_head_ += n;
        if (rx_head_ == rx_tail_) {
            rx_head_ = 0;
            rx_tail_ = 0;
        }
        res.bytes = n;
        return res;
    }

    [[nodiscard]] nf::TransportIoResult write(std::span<const char> input) noexcept {
        nf::TransportIoResult res{};
        if (!open_) {
            res.status = nf::Status{nf::ErrorCode::connection_closed};
            return res;
        }
        const std::size_t n = std::min(input.size(), tx_sink_.size());
        std::memcpy(tx_sink_.data(), input.data(), n);
        tx_bytes_total_ += input.size();
        last_tx_len_ = n;
        res.bytes = input.size();
        return res;
    }

    void close() noexcept {
        open_ = false;
        rx_head_ = 0;
        rx_tail_ = 0;
    }

    [[nodiscard]] int native_fd() const noexcept { return -1; }
    [[nodiscard]] bool valid() const noexcept { return open_; }
    [[nodiscard]] bool is_connecting() const noexcept { return false; }

    void feed_rx(std::span<const char> frame) noexcept {
        if (rx_head_ == rx_tail_) {
            rx_head_ = 0;
            rx_tail_ = 0;
        }
        const std::size_t n = std::min(frame.size(), rx_.size() - rx_tail_);
        std::memcpy(rx_.data() + rx_tail_, frame.data(), n);
        rx_tail_ += n;
    }

    [[nodiscard]] std::uint8_t sink_checksum() const noexcept {
        return last_tx_len_ > 0 ? static_cast<std::uint8_t>(tx_sink_[last_tx_len_ - 1]) : 0;
    }

private:
    bool open_{false};
    std::array<char, 8192> rx_{};
    std::size_t rx_head_{0};
    std::size_t rx_tail_{0};
    std::array<char, 4096> tx_sink_{};
    std::size_t last_tx_len_{0};
    std::uint64_t tx_bytes_total_{0};
};

static_assert(nf::DirectTransport<InMemoryBenchTransport>);

struct EchoPeer {
    int listen_fd{-1};
    std::uint16_t port{};
    std::atomic<bool> stop{false};
    std::thread worker;

    bool start() {
        listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd < 0) return false;
        int one = 1;
        ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (::listen(listen_fd, 4) != 0) return false;
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
        port = ntohs(addr.sin_port);

        worker = std::thread([this] { run(); });
        return true;
    }

    void finish() {
        stop.store(true, std::memory_order_release);
        if (listen_fd >= 0) {
            ::shutdown(listen_fd, SHUT_RDWR);
            ::close(listen_fd);
            listen_fd = -1;
        }
        if (worker.joinable()) worker.join();
    }

    void run() {
        int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd < 0) return;
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        std::vector<char> rx(65536);
        std::size_t head = 0, tail = 0;
        std::array<char, 4096> tx_buf{};
        std::array<nf::FieldView, 64> fields{};
        std::uint64_t out_seq = 1;
        const auto er = make_exec_report();

        constexpr std::string_view wt{"20261123-00:00:00.000"};
        while (!stop.load(std::memory_order_relaxed)) {
            if (tail == rx.size()) {
                std::size_t rem = tail - head;
                std::memmove(rx.data(), rx.data() + head, rem);
                head = 0;
                tail = rem;
            }
            const ssize_t n = ::recv(fd, rx.data() + tail, rx.size() - tail, 0);
            if (n <= 0) break;
            tail += static_cast<std::size_t>(n);
            while (head < tail) {
                nf::FrameView fv{};
                const auto st = nf::inspect_frame({rx.data() + head, tail - head}, fv);
                if (st.code == nf::ErrorCode::need_more) break;
                if (!st.ok()) { ::close(fd); return; }
                head += fv.consumed;
                nf::FlatMessageView msg{};
                if (!nf::parse_flat_verified(fv, fields, msg).ok()) { ::close(fd); return; }
                const auto mt = msg.find_unique(35);
                if (!mt.ok()) continue;
                nf::SessionHeader h{"SELL", "BUY", out_seq++, wt};
                std::size_t out_n = 0;
                if (mt.value().value == "A") {
                    auto l = nf::message<"A">().set<98>(0).set<108>(60);
                    auto r = nf::encode_fix(l, h, tx_buf);
                    if (r.ok()) out_n = r.value();
                } else if (mt.value().value == "D") {
                    auto r = nf::encode_fix(er, h, tx_buf);
                    if (r.ok()) out_n = r.value();
                } else if (mt.value().value == "5") {
                    auto l = nf::message<"5">();
                    auto r = nf::encode_fix(l, h, tx_buf);
                    if (r.ok()) (void)::send(fd, tx_buf.data(), r.value(), MSG_NOSIGNAL);
                    ::close(fd);
                    return;
                }
                if (out_n != 0) {
                    std::size_t sent = 0;
                    while (sent < out_n) {
                        const ssize_t w = ::send(fd, tx_buf.data() + sent, out_n - sent, MSG_NOSIGNAL);
                        if (w <= 0) { ::close(fd); return; }
                        sent += static_cast<std::size_t>(w);
                    }
                }
            }
            if (head == tail) head = tail = 0;
        }
        ::close(fd);
    }
};

struct BenchHandler {
    std::uint64_t received{};
    [[nodiscard]] nf::Status on_message(const nf::FlatMessageView&, const nf::DeliveryInfo&) noexcept {
        ++received;
        return nf::Status::success();
    }
};

struct LatencyStats {
    double p50_ns{};
    double p95_ns{};
    double p99_ns{};
    double p999_ns{};
    double max_ns{};
    double avg_ns{};
    std::uint64_t alloc_calls{};
};

LatencyStats compute_stats(std::vector<std::uint64_t>& samples_ns, std::uint64_t alloc_calls) {
    std::sort(samples_ns.begin(), samples_ns.end());
    auto pct = [&](double p) {
        return static_cast<double>(
            samples_ns[static_cast<std::size_t>(p * static_cast<double>(samples_ns.size() - 1))]);
    };
    std::uint64_t sum = 0;
    for (auto v : samples_ns) sum += v;
    return LatencyStats{
        pct(0.50),
        pct(0.95),
        pct(0.99),
        pct(0.999),
        static_cast<double>(samples_ns.back()),
        static_cast<double>(sum) / static_cast<double>(samples_ns.size()),
        alloc_calls,
    };
}

void print_ns_row(std::string_view title, const LatencyStats& s, std::string_view unit_label) {
    const double rate = s.avg_ns > 0.0 ? (1e9 / s.avg_ns) : 0.0;
    std::cout << title << ":\n"
              << std::fixed << std::setprecision(1)
              << "  p50  = " << s.p50_ns << " ns (" << std::setprecision(3) << s.p50_ns / 1000.0 << " us)\n"
              << std::setprecision(1)
              << "  p95  = " << s.p95_ns << " ns (" << std::setprecision(3) << s.p95_ns / 1000.0 << " us)\n"
              << std::setprecision(1)
              << "  p99  = " << s.p99_ns << " ns (" << std::setprecision(3) << s.p99_ns / 1000.0 << " us)\n"
              << std::setprecision(1)
              << "  p99.9= " << s.p999_ns << " ns (" << std::setprecision(3) << s.p999_ns / 1000.0 << " us)\n"
              << std::setprecision(1)
              << "  max  = " << s.max_ns << " ns (" << std::setprecision(3) << s.max_ns / 1000.0 << " us)\n"
              << "  rate = " << static_cast<std::uint64_t>(rate) << " " << unit_label
              << " | allocs = " << s.alloc_calls << "\n";
}

bool run_user_encode_bench(std::size_t warmup, std::size_t samples) {
    ReusablePartyOrder order;
    std::array<char, 1024> wire{};
    constexpr std::string_view wt{"20261123-00:00:00.000"};

    for (std::size_t i = 0; i < warmup; ++i) {
        if (!order.populate("ORD-000001").ok()) return false;
        nf::SessionHeader h{"BUY", "SELL", i + 2, wt};
        if (!nf::encode_fix44(order.view(), h, wire).ok()) return false;
    }

    std::vector<std::uint64_t> enc_ns;
    enc_ns.reserve(samples);
    std::uint64_t sink = 0;
    g_alloc_calls = 0;
    g_track_alloc = true;
    for (std::size_t i = 0; i < samples; ++i) {
        nf::SessionHeader h{"BUY", "SELL", i + 2, wt};
        const std::uint64_t t0 = now_ns();
        if (!order.populate("ORD-000001").ok()) { g_track_alloc = false; return false; }
        const auto res = nf::encode_fix44(order.view(), h, wire);
        const std::uint64_t t1 = now_ns();
        if (!res.ok()) { g_track_alloc = false; return false; }
        sink += static_cast<unsigned char>(wire[res.value() - 1]);
        enc_ns.push_back(t1 - t0);
    }
    g_track_alloc = false;
    const volatile std::uint64_t keep = sink;
    (void)keep;

    const auto stats = compute_stats(enc_ns, g_alloc_calls);
    std::cout << "=== 0.1.0-Parity Workload: Full 912-Field FIX 4.4 Schema | NewOrderSingle (7 fields + Price + NoPartyIDs[3]) & ExecutionReport (11 fields) ===\n";
    print_ns_row("  User Encode (in-loop fill + encode_fix44 only, no session/journal)", stats, "msgs/sec");
    return true;
}

bool run_pre_tcp_bench(std::size_t warmup, std::size_t samples, LatencyStats& combined_out) {
    char pat[] = "/tmp/nimblefix-direct-pretcp-XXXXXX";
    char* dir_raw = ::mkdtemp(pat);
    if (!dir_raw) return false;
    std::string dir{dir_raw};

    nf::DirectInitiatorConfig cfg{};
    cfg.host = "in-memory";
    cfg.port = 0;
    cfg.storage_directory = dir;
    cfg.session_key = "FIX.4.4/BUY/SELL";
    cfg.local_comp_id = "BUY";
    cfg.remote_comp_id = "SELL";
    cfg.heartbeat_ms = 60'000;
    cfg.journal_capacity_bytes = 64U * 1024U * 1024U;
    cfg.journal_segment_bytes = 4U * 1024U * 1024U;
    cfg.journal_max_records = 262'144;

    auto opened = nf::DirectInitiator<fix::Schema, BenchHandler, InMemoryBenchTransport>::open(std::move(cfg));
    if (!opened.ok()) {
        std::cerr << "pre-tcp open failed: " << opened.status().message() << '\n';
        std::filesystem::remove_all(dir);
        return false;
    }
    auto init = std::move(opened).value();
    if (!init.connect().ok()) {
        std::cerr << "pre-tcp connect failed\n";
        std::filesystem::remove_all(dir);
        return false;
    }

    constexpr std::string_view wt{"20261123-00:00:00.000"};
    std::array<char, 512> peer_buf{};
    std::uint64_t peer_seq = 1;
    {
        auto logon = nf::message<"A">().set<98>(0).set<108>(60);
        nf::SessionHeader h{"SELL", "BUY", peer_seq++, wt};
        auto enc = nf::encode_fix(logon, h, peer_buf);
        if (!enc.ok()) { std::filesystem::remove_all(dir); return false; }
        init.transport().feed_rx({peer_buf.data(), enc.value()});
        if (!init.poll_once().ok() || !init.is_active()) {
            std::cerr << "pre-tcp logon handshake failed\n";
            std::filesystem::remove_all(dir);
            return false;
        }
    }

    ReusablePartyOrder order;
    const auto er = make_exec_report();

    auto feed_next_er = [&]() -> bool {
        nf::SessionHeader h{"SELL", "BUY", peer_seq++, wt};
        auto enc = nf::encode_fix(er, h, peer_buf);
        if (!enc.ok()) return false;
        init.transport().feed_rx({peer_buf.data(), enc.value()});
        return true;
    };

    // Warmup
    for (std::size_t i = 0; i < warmup; ++i) {
        if (!order.populate("ORD-000001").ok()) { std::filesystem::remove_all(dir); return false; }
        if (!init.send(order).ok()) { std::filesystem::remove_all(dir); return false; }
        if (!feed_next_er()) { std::filesystem::remove_all(dir); return false; }
        const auto target = init.handler().received + 1;
        if (!init.poll_once().ok() || init.handler().received != target) {
            std::filesystem::remove_all(dir);
            return false;
        }
        if (init.needs_maintenance()) (void)init.poll_maintenance();
    }

    std::vector<std::uint64_t> tx_ns;
    std::vector<std::uint64_t> rx_ns;
    std::vector<std::uint64_t> total_ns;
    tx_ns.reserve(samples);
    rx_ns.reserve(samples);
    total_ns.reserve(samples);

    std::uint64_t tx_allocs = 0;
    std::uint64_t rx_allocs = 0;

    for (std::size_t i = 0; i < samples; ++i) {
        // 1. Outbound (pre-TCP): in-loop populate (7 scalars + NoPartyIDs group[3]) + encode into v2 mmap Journal + SSE4.2 CRC32C + SessionCore + write handoff
        g_alloc_calls = 0;
        g_track_alloc = true;
        const std::uint64_t t0 = now_ns();
        if (!order.populate("ORD-000001").ok()) { g_track_alloc = false; std::filesystem::remove_all(dir); return false; }
        if (!init.send(order).ok()) { g_track_alloc = false; std::filesystem::remove_all(dir); return false; }
        const std::uint64_t t1 = now_ns();
        g_track_alloc = false;
        tx_allocs += g_alloc_calls;

        // Prepare inbound peer frame outside timed window.
        if (!feed_next_er()) { std::filesystem::remove_all(dir); return false; }
        const auto target = init.handler().received + 1;

        // 2. Inbound (pre-TCP): read buffer -> frame + checksum + parse_flat_verified + full 912-field FIX 4.4 Schema + SessionCore + v2 mmap Journal (inbound + delivery) + Handler
        g_alloc_calls = 0;
        g_track_alloc = true;
        const std::uint64_t t2 = now_ns();
        if (!init.poll_once().ok() || init.handler().received != target) {
            g_track_alloc = false;
            std::filesystem::remove_all(dir);
            return false;
        }
        const std::uint64_t t3 = now_ns();
        g_track_alloc = false;
        rx_allocs += g_alloc_calls;

        const std::uint64_t d_tx = t1 - t0;
        const std::uint64_t d_rx = t3 - t2;
        tx_ns.push_back(d_tx);
        rx_ns.push_back(d_rx);
        total_ns.push_back(d_tx + d_rx);

        if (init.needs_maintenance()) (void)init.poll_maintenance();
    }

    const volatile std::uint8_t sink = init.transport().sink_checksum();
    (void)sink;
    std::filesystem::remove_all(dir);

    const auto tx_stats = compute_stats(tx_ns, tx_allocs);
    const auto rx_stats = compute_stats(rx_ns, rx_allocs);
    combined_out = compute_stats(total_ns, tx_allocs + rx_allocs);

    std::cout << "\n=== Pre-TCP Session Engine (In-Memory Transport, Full FIX 4.4 Schema + v2 mmap Journal, "
              << samples << " samples, warmup " << warmup << ") ===\n";
    print_ns_row("  Session Outbound (in-loop fill + DirectInitiator::send + v2 mmap Journal)", tx_stats, "msgs/sec");
    print_ns_row("  Session Inbound  (recv + frame + parse + full FIX 4.4 Schema + v2 mmap Journal + callback)",
                 rx_stats, "msgs/sec");
    print_ns_row("  Combined (Outbound + Inbound pure FIX engine budget)", combined_out, "round-trips/sec");
    return true;
}

bool run_tcp_bench(std::size_t warmup, std::size_t samples, LatencyStats& tcp_out) {
    char pat[] = "/tmp/nimblefix-direct-tcp-XXXXXX";
    char* dir_raw = ::mkdtemp(pat);
    if (!dir_raw) return false;
    std::string dir{dir_raw};

    EchoPeer peer;
    if (!peer.start()) { std::filesystem::remove_all(dir); return false; }

    nf::DirectInitiatorConfig cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = peer.port;
    cfg.storage_directory = dir;
    cfg.session_key = "FIX.4.4/BUY/SELL";
    cfg.local_comp_id = "BUY";
    cfg.remote_comp_id = "SELL";
    cfg.heartbeat_ms = 60'000;
    cfg.journal_capacity_bytes = 64U * 1024U * 1024U;
    cfg.journal_segment_bytes = 4U * 1024U * 1024U;
    cfg.journal_max_records = 262'144;

    auto opened = nf::DirectInitiator<fix::Schema, BenchHandler>::open(std::move(cfg));
    if (!opened.ok()) {
        std::cerr << "tcp open failed: " << opened.status().message() << '\n';
        peer.finish();
        std::filesystem::remove_all(dir);
        return false;
    }
    auto init = std::move(opened).value();
    if (!init.connect().ok()) {
        std::cerr << "tcp connect failed\n";
        peer.finish();
        std::filesystem::remove_all(dir);
        return false;
    }
    while (!init.is_active()) {
        if (!init.poll_once().ok()) {
            std::cerr << "tcp handshake failed\n";
            peer.finish();
            std::filesystem::remove_all(dir);
            return false;
        }
    }

    ReusablePartyOrder order;

    for (std::size_t i = 0; i < warmup; ++i) {
        const auto target = init.handler().received + 1;
        if (!order.populate("ORD-000001").ok()) { peer.finish(); std::filesystem::remove_all(dir); return false; }
        if (!init.send(order).ok()) { peer.finish(); std::filesystem::remove_all(dir); return false; }
        while (init.handler().received < target) {
            if (!init.poll_once().ok()) { peer.finish(); std::filesystem::remove_all(dir); return false; }
        }
        if (init.needs_maintenance()) (void)init.poll_maintenance();
    }

    std::vector<std::uint64_t> rtt_ns;
    rtt_ns.reserve(samples);
    const std::uint64_t t_start = now_ns();
    g_alloc_calls = 0;
    for (std::size_t i = 0; i < samples; ++i) {
        const auto target = init.handler().received + 1;
        g_track_alloc = true;
        const std::uint64_t t0 = now_ns();
        if (!order.populate("ORD-000001").ok()) { g_track_alloc = false; peer.finish(); std::filesystem::remove_all(dir); return false; }
        if (!init.send(order).ok()) { g_track_alloc = false; peer.finish(); std::filesystem::remove_all(dir); return false; }
        while (init.handler().received < target) {
            if (!init.poll_once().ok()) { g_track_alloc = false; peer.finish(); std::filesystem::remove_all(dir); return false; }
        }
        const std::uint64_t t1 = now_ns();
        g_track_alloc = false;
        rtt_ns.push_back(t1 - t0);
        if (init.needs_maintenance()) (void)init.poll_maintenance();
    }
    const std::uint64_t total_wall_ns = now_ns() - t_start;

    (void)init.logout("bench done");
    for (int i = 0; i < 200 && init.valid_transport(); ++i) (void)init.poll_once();
    peer.finish();
    std::filesystem::remove_all(dir);

    tcp_out = compute_stats(rtt_ns, g_alloc_calls);
    const double throughput = (static_cast<double>(samples) * 1e9) / static_cast<double>(total_wall_ns);
    std::cout << "\n=== Real TCP Loopback FIX 4.4 Ping-Pong (" << samples << " samples, warmup " << warmup << ") ===\n"
              << std::fixed << std::setprecision(3)
              << "  p50  = " << tcp_out.p50_ns / 1000.0 << " us\n"
              << "  p95  = " << tcp_out.p95_ns / 1000.0 << " us\n"
              << "  p99  = " << tcp_out.p99_ns / 1000.0 << " us\n"
              << "  p99.9= " << tcp_out.p999_ns / 1000.0 << " us\n"
              << "  max  = " << tcp_out.max_ns / 1000.0 << " us\n"
              << "  rate = " << static_cast<std::uint64_t>(throughput) << " round-trips/sec"
              << " | allocs = " << tcp_out.alloc_calls << "\n";
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t warmup = argc > 1 ? std::stoull(argv[1]) : 5'000;
    const std::size_t samples = argc > 2 ? std::stoull(argv[2]) : 30'000;

    if (!run_user_encode_bench(warmup, samples)) return 1;

    LatencyStats pre_tcp{};
    if (!run_pre_tcp_bench(warmup, samples, pre_tcp)) return 2;

    LatencyStats tcp{};
    if (!run_tcp_bench(warmup, samples, tcp)) return 3;

    const double fix_share_pct = tcp.p50_ns > 0.0 ? (pre_tcp.p50_ns * 100.0 / tcp.p50_ns) : 0.0;
    std::cout << "\n=== Breakdown (p50) ===\n"
              << std::fixed << std::setprecision(3)
              << "  Pure FIX engine (pre-TCP) : " << pre_tcp.p50_ns / 1000.0 << " us ("
              << std::setprecision(1) << pre_tcp.p50_ns << " ns, " << fix_share_pct << "% of TCP RTT)\n"
              << std::setprecision(3)
              << "  Kernel TCP + EchoPeer     : " << (tcp.p50_ns - pre_tcp.p50_ns) / 1000.0 << " us ("
              << std::setprecision(1) << (100.0 - fix_share_pct) << "% of TCP RTT)\n";
    return 0;
}
