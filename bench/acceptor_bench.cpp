#include <nimblefix/engine.hpp>
#include <nimblefix/fix44.hpp>
#include <nimblefix/group.hpp>
#include <nimblefix/message.hpp>
#include <nimblefix/session.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
[[gnu::noinline]] void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

inline std::uint64_t now_ns() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

// Exact 0.1.0 / DirectInitiator parity FIX 4.4 workload:
// Inbound NewOrderSingle (7 scalar fields incl. Price + 3-field NoPartyIDs repeating group)
// Outbound ExecutionReport (11 business fields populated inside handler on every order).
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
        if (!parties.status().ok()) { v.status = parties.status(); return v; }
        group_views[0] = parties.view();
        v.groups = group_views.data();
        v.group_count = 1;
        return v;
    }
};

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

inline nf::Status populate_er(fix::ExecutionReportFixed<ErShape, 128>& er,
                              std::string_view cl_ord_id) noexcept {
    er.reset();
    if (const auto s = er.order_id("ORDER-1"); !s.ok()) return s;
    if (const auto s = er.cl_ord_id(cl_ord_id); !s.ok()) return s;
    if (const auto s = er.exec_id("EXEC-1"); !s.ok()) return s;
    if (const auto s = er.exec_type('0'); !s.ok()) return s;
    if (const auto s = er.ord_status('0'); !s.ok()) return s;
    if (const auto s = er.side('1'); !s.ok()) return s;
    if (const auto s = er.order_qty(nf::Decimal{100, 0}); !s.ok()) return s;
    if (const auto s = er.leaves_qty(nf::Decimal{100, 0}); !s.ok()) return s;
    if (const auto s = er.cum_qty(nf::Decimal{0, 0}); !s.ok()) return s;
    if (const auto s = er.avg_px(nf::Decimal{0, 0}); !s.ok()) return s;
    return er.symbol("AAPL");
}

struct LatencyStats {
    double p50_ns{};
    double p95_ns{};
    double p99_ns{};
    double p999_ns{};
    double max_ns{};
    double avg_ns{};
    std::uint64_t alloc_calls{};
};

LatencyStats compute_stats(std::vector<std::uint64_t>& samples_ns, std::uint64_t alloc_calls = 0) {
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

void print_ns_row(std::string_view title, const LatencyStats& s, std::string_view unit_label, bool show_allocs = true) {
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
              << "  rate = " << static_cast<std::uint64_t>(rate) << " " << unit_label;
    if (show_allocs) std::cout << " | allocs = " << s.alloc_calls;
    std::cout << "\n";
}

struct SinkWriter {
    std::array<char, 4096> buf{};
    std::size_t last_len{};
    std::uint64_t frames{};
    static nf::WriteResult write(void* ctx, std::span<const char> bytes) noexcept {
        auto* self = static_cast<SinkWriter*>(ctx);
        const std::size_t n = std::min(bytes.size(), self->buf.size());
        std::memcpy(self->buf.data(), bytes.data(), n);
        self->last_len = n;
        ++self->frames;
        return {bytes.size(), false, nf::Status::success()};
    }
};

// Tier 1: Pre-TCP Acceptor (SessionDriver in-memory, full 912-field FIX 4.4 Schema + v2 mmap Journal).
bool run_pre_tcp_acceptor(std::size_t warmup, std::size_t samples, LatencyStats& turnaround_out) {
    char pat[] = "/tmp/nimblefix-acceptor-pretcp-XXXXXX";
    char* dir_raw = ::mkdtemp(pat);
    if (!dir_raw) return false;
    std::string dir{dir_raw};

    fix::ExecutionReportFixed<ErShape, 128> er;
    std::uint64_t replies_sent = 0;

    nf::SessionCallbacks callbacks{};
    callbacks.wire_hints = fix::Schema::wire_hints();
    callbacks.validate_message = [](const nf::FlatMessageView& msg) noexcept -> nf::Status {
        const auto fields = msg.fields();
        if (fields.size() >= 3 && fields[2].tag == 35) {
            return nf::validate_schema_message_for_type(
                fix::Schema::descriptor(), fields[2].value, fields[2].offset, fields, nf::ValidationLevel::strict);
        }
        return nf::validate_schema_message(fix::Schema::descriptor(), msg, nf::ValidationLevel::strict);
    };
    callbacks.on_message = [&](nf::SessionContext& ctx, const nf::FlatMessageView& msg, const nf::DeliveryInfo&) noexcept {
        const auto cid = msg.find_unique(11);
        const std::string_view id = cid.ok() ? cid.value().value : std::string_view{"ORD-000001"};
        if (const auto st = populate_er(er, id); !st.ok()) return st;
        const auto sent = ctx.send(er);
        if (!sent.ok()) return sent.status();
        ++replies_sent;
        return nf::Status::success();
    };

    nf::SessionConfig cfg{};
    cfg.role = nf::SessionRole::acceptor;
    cfg.local_comp_id = "SELL";
    cfg.remote_comp_id = "BUY";
    cfg.storage_directory = dir;
    cfg.session_key = "FIX.4.4|SELL|BUY";
    cfg.heartbeat_ms = 60'000;
    cfg.journal_capacity_bytes = 64U * 1024U * 1024U;
    cfg.journal_segment_bytes = 4U * 1024U * 1024U;
    cfg.journal_max_records = 262'144;

    auto created = nf::SessionDriver::create(cfg, std::move(callbacks));
    if (!created.ok()) {
        std::cerr << "pre-tcp acceptor create failed: " << created.status().message() << '\n';
        std::filesystem::remove_all(dir);
        return false;
    }
    auto driver = std::move(created).value();
    constexpr std::string_view wt{"20261123-00:00:00.000"};
    if (!driver.connected(1, wt).ok()) { std::filesystem::remove_all(dir); return false; }

    SinkWriter sink{};
    std::array<char, 1024> order_wire{};
    std::uint64_t in_seq = 1;
    {
        auto logon = nf::message<"A">().set<98>(0).set<108>(60);
        nf::SessionHeader h{"BUY", "SELL", in_seq++, wt};
        auto enc = nf::encode_fix44(logon, h, order_wire);
        if (!enc.ok()) { std::filesystem::remove_all(dir); return false; }
        if (!driver.receive({order_wire.data(), enc.value()}, 1, wt).ok()) {
            std::cerr << "pre-tcp acceptor logon receive failed\n";
            std::filesystem::remove_all(dir);
            return false;
        }
        if (!driver.flush(&SinkWriter::write, &sink, 1, 65536).ok() || driver.state() != nf::SessionState::active) {
            std::cerr << "pre-tcp acceptor not active\n";
            std::filesystem::remove_all(dir);
            return false;
        }
    }

    ReusablePartyOrder order;
    if (!order.populate("ORD-000001").ok()) { std::filesystem::remove_all(dir); return false; }

    auto encode_next_order = [&]() -> std::size_t {
        nf::SessionHeader h{"BUY", "SELL", in_seq++, wt};
        auto enc = nf::encode_fix44(order.view(), h, order_wire);
        return enc.ok() ? enc.value() : 0;
    };

    for (std::size_t i = 0; i < warmup; ++i) {
        const std::size_t len = encode_next_order();
        if (len == 0) { std::filesystem::remove_all(dir); return false; }
        if (!driver.receive({order_wire.data(), len}, i + 2, wt).ok()) { std::filesystem::remove_all(dir); return false; }
        if (!driver.flush(&SinkWriter::write, &sink, i + 2, 65536).ok()) { std::filesystem::remove_all(dir); return false; }
    }

    std::vector<std::uint64_t> recv_reply_ns;
    std::vector<std::uint64_t> flush_ns;
    std::vector<std::uint64_t> total_ns;
    recv_reply_ns.reserve(samples);
    flush_ns.reserve(samples);
    total_ns.reserve(samples);

    std::uint64_t total_allocs = 0;
    for (std::size_t i = 0; i < samples; ++i) {
        const std::size_t len = encode_next_order();
        if (len == 0) { std::filesystem::remove_all(dir); return false; }
        const std::uint64_t ts_ms = warmup + i + 2;

        g_alloc_calls = 0;
        g_track_alloc = true;
        const std::uint64_t t0 = now_ns();
        if (!driver.receive({order_wire.data(), len}, ts_ms, wt).ok()) {
            g_track_alloc = false;
            std::filesystem::remove_all(dir);
            return false;
        }
        const std::uint64_t t1 = now_ns();
        if (!driver.flush(&SinkWriter::write, &sink, ts_ms, 65536).ok()) {
            g_track_alloc = false;
            std::filesystem::remove_all(dir);
            return false;
        }
        const std::uint64_t t2 = now_ns();
        g_track_alloc = false;
        total_allocs += g_alloc_calls;

        recv_reply_ns.push_back(t1 - t0);
        flush_ns.push_back(t2 - t1);
        total_ns.push_back(t2 - t0);

        // Off-hot-path tick drives journal.poll_maintenance() (spare segment refill & checkpoint),
        // matching real Engine/Reactor maintenance behavior.
        if ((i & 63U) == 63U) (void)driver.tick(ts_ms, wt);
    }

    const volatile std::uint8_t keep = sink.last_len > 0 ? static_cast<std::uint8_t>(sink.buf[sink.last_len - 1]) : 0;
    (void)keep;
    std::filesystem::remove_all(dir);

    const auto rr_stats = compute_stats(recv_reply_ns, total_allocs);
    const auto fl_stats = compute_stats(flush_ns, 0);
    turnaround_out = compute_stats(total_ns, total_allocs);

    std::cout << "=== Tier 1: Pre-TCP Acceptor (SessionDriver, Full 912-Field FIX 4.4 Schema + v2 mmap Journal, "
              << samples << " samples, warmup " << warmup << ") ===\n";
    print_ns_row("  Receive + 912-field Schema + In-Handler Populate ER + Zero-Copy Journal Send + Delivery",
                 rr_stats, "orders/sec");
    print_ns_row("  Flush (mmap -> transport sink handoff)", fl_stats, "flushes/sec", false);
    print_ns_row("  Total Pre-TCP Acceptor Turn-Around (wire-in -> wire-out)", turnaround_out, "orders/sec");
    return true;
}

std::uint16_t pick_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { ::close(fd); return 0; }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) { ::close(fd); return 0; }
    const std::uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

bool tcp_send_all(int fd, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t w = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (w <= 0) return false;
        sent += static_cast<std::size_t>(w);
    }
    return true;
}

bool tcp_recv_frame(int fd, std::vector<char>& rx, std::size_t& head, std::size_t& tail, nf::FrameView& out) {
    while (true) {
        if (head < tail) {
            const auto st = nf::inspect_frame({rx.data() + head, tail - head}, out);
            if (st.ok()) {
                head += out.consumed;
                if (head == tail) head = tail = 0;
                return true;
            }
            if (st.code != nf::ErrorCode::need_more) return false;
        }
        if (tail == rx.size()) {
            const std::size_t rem = tail - head;
            std::memmove(rx.data(), rx.data() + head, rem);
            head = 0;
            tail = rem;
        }
        const ssize_t n = ::recv(fd, rx.data() + tail, rx.size() - tail, 0);
        if (n <= 0) return false;
        tail += static_cast<std::size_t>(n);
    }
}

std::string current_utc_timestamp() {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    std::tm utc{};
    ::gmtime_r(&ts.tv_sec, &utc);
    char buf[96]{};
    const int ms = static_cast<int>(ts.tv_nsec / 1'000'000L);
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d.%03d",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec, ms);
    return std::string{buf};
}

bool run_tcp_client_session(std::uint16_t port, std::string sender_comp, std::size_t warmup,
                            std::size_t samples, std::vector<std::uint64_t>& rtt_out,
                            std::atomic<int>& ready_count, std::atomic<bool>& go_flag) {
    struct ReadyGuard {
        std::atomic<int>& c;
        bool signaled{false};
        ~ReadyGuard() { if (!signaled) c.fetch_add(1, std::memory_order_release); }
    } ready_guard{ready_count};

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    bool connected = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            connected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!connected) { ::close(fd); return false; }

    const std::string wt_str = current_utc_timestamp();
    const std::string_view wt{wt_str};
    std::array<char, 1024> tx{};
    std::vector<char> rx(65536);
    std::size_t head = 0, tail = 0;
    std::uint64_t seq = 1;

    // FIX 4.4 Logon handshake
    {
        auto logon = nf::message<"A">().set<98>(0).set<108>(60);
        nf::SessionHeader h{sender_comp, "SELL", seq++, wt};
        auto enc = nf::encode_fix44(logon, h, tx);
        if (!enc.ok() || !tcp_send_all(fd, tx.data(), enc.value())) { ::close(fd); return false; }
        nf::FrameView fv{};
        if (!tcp_recv_frame(fd, rx, head, tail, fv)) { ::close(fd); return false; }
    }

    ReusablePartyOrder order;
    if (!order.populate("ORD-000001").ok()) { ::close(fd); return false; }

    for (std::size_t i = 0; i < warmup; ++i) {
        nf::SessionHeader h{sender_comp, "SELL", seq++, wt};
        auto enc = nf::encode_fix44(order.view(), h, tx);
        if (!enc.ok() || !tcp_send_all(fd, tx.data(), enc.value())) { ::close(fd); return false; }
        nf::FrameView fv{};
        if (!tcp_recv_frame(fd, rx, head, tail, fv)) { ::close(fd); return false; }
    }

    ready_guard.signaled = true;
    ready_count.fetch_add(1, std::memory_order_release);
    while (!go_flag.load(std::memory_order_acquire)) std::this_thread::yield();

    rtt_out.clear();
    rtt_out.reserve(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        nf::SessionHeader h{sender_comp, "SELL", seq++, wt};
        auto enc = nf::encode_fix44(order.view(), h, tx);
        if (!enc.ok()) { ::close(fd); return false; }
        const std::uint64_t t0 = now_ns();
        if (!tcp_send_all(fd, tx.data(), enc.value())) { ::close(fd); return false; }
        nf::FrameView fv{};
        if (!tcp_recv_frame(fd, rx, head, tail, fv)) { ::close(fd); return false; }
        rtt_out.push_back(now_ns() - t0);
    }

    auto logout = nf::message<"5">();
    nf::SessionHeader lh{sender_comp, "SELL", seq++, wt};
    if (auto enc = nf::encode_fix44(logout, lh, tx); enc.ok()) {
        if (tcp_send_all(fd, tx.data(), enc.value())) {
            nf::FrameView lfv{};
            (void)tcp_recv_frame(fd, rx, head, tail, lfv);
        }
    }
    ::close(fd);
    return true;
}

bool run_engine_acceptor_tcp(std::size_t session_count, std::size_t warmup, std::size_t samples,
                             std::string_view label, LatencyStats& stats_out, double& agg_rate_out) {
    char pat[] = "/tmp/nimblefix-acceptor-tcp-XXXXXX";
    char* dir_raw = ::mkdtemp(pat);
    if (!dir_raw) return false;
    std::string dir{dir_raw};

    const std::uint16_t port = pick_free_port();
    if (port == 0) { std::filesystem::remove_all(dir); return false; }

    nf::EngineConfig cfg{};
    cfg.storage.directory = dir;
    cfg.storage.journal_capacity_bytes = 32U * 1024U * 1024U;
    cfg.storage.segment_bytes = 4U * 1024U * 1024U;
    cfg.storage.journal_max_records = 131'072;
    cfg.heartbeat_ms = 60'000;
    cfg.max_sessions = std::max<std::size_t>(16, session_count + 4);

    auto created = nf::Engine<fix::Schema, nf::Inline>::create(cfg);
    if (!created.ok()) {
        std::cerr << label << " engine create failed: " << created.status().message() << '\n';
        std::filesystem::remove_all(dir);
        return false;
    }
    auto engine = std::move(created).value();
    if (!engine.add_listener({"main", {"127.0.0.1", port}}).ok()) {
        std::cerr << label << " add_listener failed\n";
        std::filesystem::remove_all(dir);
        return false;
    }

    for (std::size_t s = 0; s < session_count; ++s) {
        nf::AcceptorConfig ac{};
        ac.name = "session-" + std::to_string(s);
        ac.listener = "main";
        ac.local_comp_id = "SELL";
        ac.remote_comp_id = "BUY-" + std::to_string(s);
        if (!engine.add_acceptor(ac).ok()) {
            std::cerr << label << " add_acceptor failed\n";
            std::filesystem::remove_all(dir);
            return false;
        }
    }

    if (!engine.on_error([label](const nf::ErrorEvent& ev) {
            std::cerr << "[" << label << "] error: " << ev.format() << '\n';
        }).ok()) {
        std::filesystem::remove_all(dir);
        return false;
    }

    fix::ExecutionReportFixed<ErShape, 128> er;
    if (!engine.on<fix::NewOrderSingle>([&](nf::SessionContext& ctx, const fix::NewOrderSingleView& req) noexcept {
            const auto cid = req.cl_ord_id();
            const std::string_view id = cid.ok() ? cid.value() : std::string_view{"ORD-000001"};
            if (const auto st = populate_er(er, id); !st.ok()) return st;
            return ctx.send(er).status();
        }).ok()) {
        std::filesystem::remove_all(dir);
        return false;
    }

    std::thread server_thread([&] { (void)engine.run(); });

    std::atomic<int> ready_count{0};
    std::atomic<bool> go_flag{false};
    std::vector<std::vector<std::uint64_t>> per_client_rtt(session_count);
    std::vector<std::atomic<bool>> client_ok(session_count);
    std::vector<std::thread> clients;
    clients.reserve(session_count);
    for (std::size_t s = 0; s < session_count; ++s) {
        client_ok[s].store(false);
        clients.emplace_back([&, s] {
            const bool ok = run_tcp_client_session(port, "BUY-" + std::to_string(s),
                warmup, samples, per_client_rtt[s], ready_count, go_flag);
            client_ok[s].store(ok, std::memory_order_release);
        });
    }

    while (ready_count.load(std::memory_order_acquire) < static_cast<int>(session_count)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const std::uint64_t t_wall_start = now_ns();
    go_flag.store(true, std::memory_order_release);

    bool all_ok = true;
    for (std::size_t s = 0; s < session_count; ++s) {
        clients[s].join();
        if (!client_ok[s].load(std::memory_order_acquire)) all_ok = false;
    }
    const std::uint64_t t_wall_ns = now_ns() - t_wall_start;

    engine.request_stop();
    server_thread.join();
    std::filesystem::remove_all(dir);
    if (!all_ok) {
        std::cerr << label << " client failed\n";
        return false;
    }

    std::vector<std::uint64_t> merged;
    merged.reserve(session_count * samples);
    for (const auto& v : per_client_rtt) merged.insert(merged.end(), v.begin(), v.end());
    stats_out = compute_stats(merged, 0);
    agg_rate_out = (static_cast<double>(merged.size()) * 1e9) / static_cast<double>(t_wall_ns);

    std::cout << "\n=== " << label << " (" << session_count << " session(s), "
              << samples << " orders/session, warmup " << warmup << ") ===\n"
              << std::fixed << std::setprecision(3)
              << "  p50  = " << stats_out.p50_ns / 1000.0 << " us\n"
              << "  p95  = " << stats_out.p95_ns / 1000.0 << " us\n"
              << "  p99  = " << stats_out.p99_ns / 1000.0 << " us\n"
              << "  p99.9= " << stats_out.p999_ns / 1000.0 << " us\n"
              << "  max  = " << stats_out.max_ns / 1000.0 << " us\n"
              << "  agg  = " << static_cast<std::uint64_t>(agg_rate_out) << " orders/sec (1 epoll thread)\n";
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t warmup = argc > 1 ? std::stoull(argv[1]) : 2'000;
    const std::size_t samples = argc > 2 ? std::stoull(argv[2]) : 20'000;
    const std::size_t multi_sessions = argc > 3 ? std::stoull(argv[3]) : 8;

    LatencyStats pre_tcp{};
    if (!run_pre_tcp_acceptor(warmup, samples, pre_tcp)) return 1;

    LatencyStats tcp_1{};
    double rate_1 = 0.0;
    if (!run_engine_acceptor_tcp(1, warmup, samples,
            "Tier 2: Engine<fix44::Schema, Inline> Acceptor Single-Session TCP Loopback",
            tcp_1, rate_1)) return 2;

    LatencyStats tcp_multi{};
    double rate_multi = 0.0;
    const std::size_t multi_samples = std::max<std::size_t>(2'000, samples / 2);
    if (!run_engine_acceptor_tcp(multi_sessions, std::min<std::size_t>(warmup, 1'000), multi_samples,
            "Tier 3: Engine<fix44::Schema, Inline> Acceptor Multi-Session Concurrent TCP",
            tcp_multi, rate_multi)) return 3;

    const double fix_share_pct = tcp_1.p50_ns > 0.0 ? (pre_tcp.p50_ns * 100.0 / tcp_1.p50_ns) : 0.0;
    std::cout << "\n=== Single-Session Breakdown (p50) ===\n"
              << std::fixed << std::setprecision(3)
              << "  Pure Acceptor FIX engine (pre-TCP) : " << pre_tcp.p50_ns / 1000.0 << " us ("
              << std::setprecision(1) << pre_tcp.p50_ns << " ns, " << fix_share_pct << "% of TCP RTT)\n"
              << std::setprecision(3)
              << "  epoll + Kernel TCP + Client        : " << (tcp_1.p50_ns - pre_tcp.p50_ns) / 1000.0 << " us ("
              << std::setprecision(1) << (100.0 - fix_share_pct) << "% of TCP RTT)\n";
    return 0;
}
