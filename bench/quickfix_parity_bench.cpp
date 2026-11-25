#include <nimblefix/codec.hpp>
#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/direct_queue.hpp>
#include <nimblefix/engine.hpp>
#include <nimblefix/fix44.hpp>
#include <nimblefix/group.hpp>
#include <nimblefix/message.hpp>

#include <quickfix/Application.h>
#include <quickfix/DataDictionary.h>
#include <quickfix/FileStore.h>
#include <quickfix/Message.h>
#include <quickfix/Session.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/SocketAcceptor.h>
#include <quickfix/SocketInitiator.h>
#include <quickfix/fix44/ExecutionReport.h>
#include <quickfix/fix44/NewOrderSingle.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
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
#include <sstream>
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

// Global allocation counters via -Wl,--wrap=malloc + operator new/delete.
// Shared identically by BOTH NimbleFIX and statically-linked QuickFIX.
namespace {
std::atomic<bool> g_track_alloc{false};
std::atomic<std::uint64_t> g_alloc_calls{0};
} // namespace

extern "C" void* __real_malloc(std::size_t);
extern "C" void* __wrap_malloc(std::size_t size) {
    if (g_track_alloc.load(std::memory_order_relaxed)) {
        g_alloc_calls.fetch_add(1, std::memory_order_relaxed);
    }
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

[[nodiscard]] inline std::uint64_t now_ns() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

[[nodiscard]] std::string current_utc_timestamp() {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    std::tm utc{};
    ::gmtime_r(&ts.tv_sec, &utc);
    char buf[64];
    const auto ms = static_cast<unsigned>(ts.tv_nsec / 1'000'000L);
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d.%03u",
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
        utc.tm_hour, utc.tm_min, utc.tm_sec, ms);
    return std::string{buf};
}

[[nodiscard]] std::string_view format_ord_id(char* buf, std::size_t cap, std::size_t seq) noexcept {
    if (cap < 16) return {};
    std::memcpy(buf, "ORD-", 4);
    auto r = std::to_chars(buf + 4, buf + cap, seq);
    if (r.ec != std::errc{}) return {};
    return {buf, static_cast<std::size_t>(r.ptr - buf)};
}

[[nodiscard]] std::string_view format_burst_id(char* buf, std::size_t cap,
                                               std::uint64_t t_burst,
                                               std::uint64_t t_send) noexcept {
    auto r1 = std::to_chars(buf, buf + cap, t_burst);
    if (r1.ec != std::errc{} || r1.ptr >= buf + cap - 2) return {};
    *r1.ptr++ = '-';
    auto r2 = std::to_chars(r1.ptr, buf + cap, t_send);
    if (r2.ec != std::errc{}) return {};
    return {buf, static_cast<std::size_t>(r2.ptr - buf)};
}

[[nodiscard]] bool parse_burst_id(std::string_view text,
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

struct TempDir {
    std::string path;
    explicit TempDir(const char* prefix) {
        std::string pattern = std::string{"/tmp/"} + prefix + "XXXXXX";
        std::vector<char> buf(pattern.begin(), pattern.end());
        buf.push_back('\0');
        if (char* p = ::mkdtemp(buf.data())) path = p;
    }
    ~TempDir() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
};

struct Stats {
    double p50_ns{};
    double p95_ns{};
    double p99_ns{};
    double p999_ns{};
    double max_ns{};
    double avg_ns{};
    double rate{};
    double allocs_per_op{};
};

[[nodiscard]] Stats compute_stats(std::vector<std::uint64_t> samples,
                                  std::uint64_t total_allocs,
                                  std::uint64_t wall_ns = 0) {
    Stats s{};
    if (samples.empty()) return s;
    std::sort(samples.begin(), samples.end());
    auto pct = [&](double q) -> double {
        const auto idx = static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1));
        return static_cast<double>(samples[idx]);
    };
    std::uint64_t sum = 0;
    for (auto v : samples) sum += v;
    s.p50_ns = pct(0.50);
    s.p95_ns = pct(0.95);
    s.p99_ns = pct(0.99);
    s.p999_ns = pct(0.999);
    s.max_ns = static_cast<double>(samples.back());
    s.avg_ns = static_cast<double>(sum) / static_cast<double>(samples.size());
    if (wall_ns != 0) {
        s.rate = static_cast<double>(samples.size()) * 1e9 / static_cast<double>(wall_ns);
    } else if (s.avg_ns > 0.0) {
        s.rate = 1e9 / s.avg_ns;
    }
    s.allocs_per_op = static_cast<double>(total_allocs) / static_cast<double>(samples.size());
    return s;
}

void print_row_ns(const char* label, const Stats& s, const Stats* baseline = nullptr) {
    std::cout << "  " << std::left << std::setw(42) << label
              << std::right << std::fixed
              << " p50=" << std::setprecision(1) << std::setw(8) << s.p50_ns << " ns"
              << " | p99=" << std::setw(8) << s.p99_ns << " ns"
              << " | rate=" << std::setw(9) << static_cast<std::uint64_t>(s.rate) << "/s"
              << " | alloc/op=" << std::setprecision(2) << std::setw(5) << s.allocs_per_op;
    if (baseline != nullptr && s.p50_ns > 0.0) {
        std::cout << " | " << std::setprecision(2) << (baseline->p50_ns / s.p50_ns) << "x";
    }
    std::cout << '\n';
}

void print_row_us(const char* label, const Stats& s, const Stats* baseline = nullptr) {
    std::cout << "  " << std::left << std::setw(42) << label
              << std::right << std::fixed
              << " p50=" << std::setprecision(2) << std::setw(7) << (s.p50_ns / 1000.0) << " us"
              << " | p99=" << std::setw(7) << (s.p99_ns / 1000.0) << " us"
              << " | rate=" << std::setw(9) << static_cast<std::uint64_t>(s.rate) << "/s"
              << " | alloc/op=" << std::setprecision(2) << std::setw(5) << s.allocs_per_op;
    if (baseline != nullptr && s.p50_ns > 0.0) {
        std::cout << " | " << std::setprecision(2) << (baseline->p50_ns / s.p50_ns) << "x";
    }
    std::cout << '\n';
}

[[nodiscard]] std::uint16_t reserve_loopback_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    std::uint16_t port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        socklen_t len = sizeof(addr);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            port = ntohs(addr.sin_port);
        }
    }
    ::close(fd);
    return port;
}

bool send_all(int fd, std::string_view bytes) noexcept {
    std::size_t off = 0;
    while (off < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// ============================================================================
// Shared FIX 4.4 Parity Payloads:
//   NewOrderSingle (35=D): 7 scalar fields (11, 55, 54, 60, 38, 40, 44)
//                        + NoPartyIDs (453)=1 repeating group (448, 447, 452)
//   ExecutionReport (35=8): 11 business fields (37, 11, 17, 150, 39, 54, 38, 151, 14, 6, 55)
// ============================================================================
constexpr std::string_view kFixedUtc{"20261123-00:00:00.000"};

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

    [[nodiscard]] nf::Status populate(std::string_view cl_ord_id,
                                      std::string_view transact_time = kFixedUtc) noexcept {
        body.reset();
        parties.reset();
        if (const auto s = body.cl_ord_id(cl_ord_id); !s.ok()) return s;
        if (const auto s = body.symbol("AAPL"); !s.ok()) return s;
        if (const auto s = body.side('1'); !s.ok()) return s;
        if (const auto s = body.transact_time(transact_time); !s.ok()) return s;
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

inline nf::Status populate_nf_er(fix::ExecutionReportFixed<ErShape, 128>& er,
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

inline FIX44::NewOrderSingle build_qf_order(std::string_view cl_ord_id,
                                            const FIX::TransactTime& transact_time) {
    FIX44::NewOrderSingle order;
    order.set(FIX::ClOrdID(std::string{cl_ord_id}));
    order.set(FIX::Symbol("AAPL"));
    order.set(FIX::Side('1'));
    order.set(transact_time);
    order.set(FIX::OrderQty(100));
    order.set(FIX::OrdType('2'));
    order.set(FIX::Price(185.25));
    FIX44::NewOrderSingle::NoPartyIDs party;
    party.set(FIX::PartyID("CLIENT1"));
    party.set(FIX::PartyIDSource('D'));
    party.set(FIX::PartyRole(1));
    order.addGroup(party);
    return order;
}

inline FIX44::ExecutionReport build_qf_er(const std::string& cl_ord_id) {
    FIX44::ExecutionReport er(
        FIX::OrderID("ORDER-1"),
        FIX::ExecID("EXEC-1"),
        FIX::ExecType('0'),
        FIX::OrdStatus('0'),
        FIX::Side('1'),
        FIX::LeavesQty(100),
        FIX::CumQty(0),
        FIX::AvgPx(0));
    er.set(FIX::ClOrdID(cl_ord_id));
    er.set(FIX::OrderQty(100));
    er.set(FIX::Symbol("AAPL"));
    return er;
}

// ============================================================================
// Neutral Raw-TCP Counterparty Peer for Initiator Ping-Pong & Burst Suites.
// Identical peer implementation used for BOTH NimbleFIX and QuickFIX.
// ============================================================================
class NeutralEchoPeer {
public:
    explicit NeutralEchoPeer(bool echo_er) : echo_er_(echo_er) {}
    ~NeutralEchoPeer() { stop(); }

    bool start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) return false;
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (::listen(listen_fd_, 4) != 0) return false;
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
        port_ = ntohs(addr.sin_port);
        worker_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (worker_.joinable()) worker_.join();
    }

    void begin_capture(std::size_t reserve_n) {
        burst_lats_.clear();
        burst_lats_.reserve(reserve_n);
        send_lats_.clear();
        send_lats_.reserve(reserve_n);
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
        fix::ExecutionReportFixed<ErShape, 128> er;
        std::uint64_t out_seq = 1;
        const std::string session_utc = current_utc_timestamp();

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
                const auto st = nf::inspect_frame(
                    std::string_view{rx.data() + rx_head, rx.size() - rx_head}, fv);
                if (st.code == nf::ErrorCode::need_more) break;
                if (!st.ok()) { rx_head = rx.size(); break; }
                rx_head += fv.consumed;

                nf::FlatMessageView msg{};
                if (!nf::parse_flat_verified(fv, fields, msg).ok()) continue;
                const auto mt = msg.msg_type();

                if (mt == "A") {
                    const std::string utc = current_utc_timestamp();
                    auto ack = nf::message<"A">().set<98>(0).set<108>(60);
                    const auto enc = nf::encode_fix44(
                        ack, nf::SessionHeader{"SELL", "BUY", out_seq++, utc}, tx_buf);
                    if (enc.ok()) (void)send_all(fd, {tx_buf.data(), enc.value()});
                } else if (mt == "5") {
                    const std::string utc = current_utc_timestamp();
                    auto ack = nf::message<"5">();
                    const auto enc = nf::encode_fix44(
                        ack, nf::SessionHeader{"SELL", "BUY", out_seq++, utc}, tx_buf);
                    if (enc.ok()) (void)send_all(fd, {tx_buf.data(), enc.value()});
                    ::close(fd);
                    return;
                } else if (mt == "D") {
                    const auto f11 = msg.find_unique(11);
                    if (capture_.load(std::memory_order_acquire) && f11.ok()) {
                        std::uint64_t t_burst = 0, t_send = 0;
                        if (parse_burst_id(f11.value().value, t_burst, t_send)) {
                            burst_lats_.push_back(t_recv >= t_burst ? (t_recv - t_burst) : 0);
                            send_lats_.push_back(t_recv >= t_send ? (t_recv - t_send) : 0);
                        }
                    }
                    received_orders_.fetch_add(1, std::memory_order_release);

                    if (echo_er_) {
                        const auto cid = f11.ok() ? f11.value().value : std::string_view{"ORD-0"};
                        if (populate_nf_er(er, cid).ok()) {
                            const auto enc = nf::encode_fix44(
                                er, nf::SessionHeader{"SELL", "BUY", out_seq++, session_utc}, tx_buf);
                            if (enc.ok()) (void)send_all(fd, {tx_buf.data(), enc.value()});
                        }
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

    bool echo_er_{true};
    int listen_fd_{-1};
    std::uint16_t port_{};
    std::atomic<bool> stop_{false};
    std::atomic<bool> capture_{false};
    std::atomic<std::uint64_t> received_orders_{0};
    std::vector<std::uint64_t> burst_lats_;
    std::vector<std::uint64_t> send_lats_;
    std::thread worker_;
};

// ============================================================================
// Suite 1: Codec Encode (Construct + Complete FIX 4.4 Wire Frame)
// ============================================================================
bool run_suite1_encode(std::size_t warmup, std::size_t samples,
                       const FIX::DataDictionary& dict) {
    std::cout << "\n=== Suite 1: Codec Encode (FIX 4.4 NewOrderSingle + NoPartyIDs[1] + Session Header/CheckSum, "
              << samples << " samples) ===\n";

    const FIX::UtcTimeStamp qf_ts(0, 0, 0, 23, 11, 2026);
    const FIX::TransactTime qf_transact(qf_ts);
    const FIX::SendingTime qf_sending(qf_ts);

    // Cross-validation gate before timing: verify mutual wire interoperability.
    {
        ReusablePartyOrder nf_order;
        if (!nf_order.populate("VERIFY-1").ok()) return false;
        std::array<char, 1024> wire{};
        const auto enc = nf::encode_fix44(nf_order.view(), nf::SessionHeader{"BUY", "SELL", 2, kFixedUtc}, wire);
        if (!enc.ok()) { std::cerr << "NimbleFIX verify encode failed\n"; return false; }
        const std::string nf_bytes(wire.data(), enc.value());
        FIX::Message qf_parsed(nf_bytes, dict, true);
        dict.validate(qf_parsed);
        if (qf_parsed.getField(11) != "VERIFY-1") return false;

        auto qf_order = build_qf_order("VERIFY-2", qf_transact);
        qf_order.getHeader().setField(FIX::SenderCompID("BUY"));
        qf_order.getHeader().setField(FIX::TargetCompID("SELL"));
        qf_order.getHeader().setField(FIX::MsgSeqNum(2));
        qf_order.getHeader().setField(qf_sending);
        std::string qf_bytes;
        qf_order.toString(qf_bytes);
        nf::FrameView fv{};
        if (!nf::inspect_frame(qf_bytes, fv, fix::Schema::wire_hints()).ok()) return false;
        std::array<nf::FieldView, 64> scratch{};
        nf::FlatMessageView mv{};
        if (!nf::parse_flat_verified(fv, scratch, mv, fix::Schema::wire_hints()).ok()) return false;
        if (!nf::validate_schema_message(fix::Schema::descriptor(), mv, nf::ValidationLevel::strict).ok()) return false;
        if (!mv.is<fix::NewOrderSingle>() || mv.as<fix::NewOrderSingle>().cl_ord_id().value() != "VERIFY-2") return false;
    }

    volatile std::uint64_t sink = 0;

    // 1A. NimbleFIX Fixed (zero-alloc)
    Stats nf_fixed{};
    {
        ReusablePartyOrder order;
        std::array<char, 1024> wire{};
        char id_buf[32];
        for (std::size_t i = 0; i < warmup; ++i) {
            const auto cid = format_ord_id(id_buf, sizeof(id_buf), i);
            (void)order.populate(cid);
            const auto r = nf::encode_fix44(order.view(), nf::SessionHeader{"BUY", "SELL", i + 2, kFixedUtc}, wire);
            if (!r.ok()) return false;
            sink += static_cast<unsigned char>(wire[r.value() - 1]);
        }
        std::vector<std::uint64_t> lats;
        lats.reserve(samples);
        g_alloc_calls.store(0, std::memory_order_relaxed);
        g_track_alloc.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < samples; ++i) {
            const auto cid = format_ord_id(id_buf, sizeof(id_buf), i);
            const nf::SessionHeader hdr{"BUY", "SELL", i + 2, kFixedUtc};
            const std::uint64_t t0 = now_ns();
            (void)order.populate(cid);
            const auto r = nf::encode_fix44(order.view(), hdr, wire);
            const std::uint64_t t1 = now_ns();
            if (!r.ok()) { g_track_alloc.store(false); return false; }
            sink += static_cast<unsigned char>(wire[r.value() - 1]);
            lats.push_back(t1 - t0);
        }
        g_track_alloc.store(false, std::memory_order_release);
        nf_fixed = compute_stats(std::move(lats), g_alloc_calls.load());
    }

    // 1B. NimbleFIX StreamWriter (0-alloc, zero intermediate objects, dynamic groups/if)
    Stats nf_stream{};
    {
        std::array<char, 1024> wire{};
        char id_buf[32];
        auto run_one = [&](std::size_t i) -> std::size_t {
            const auto cid = format_ord_id(id_buf, sizeof(id_buf), i);
            const auto r = nf::encode_fix44_stream<fix::NewOrderSingle>(
                nf::SessionHeader{"BUY", "SELL", i + 2, kFixedUtc}, wire, [&](auto& w) {
                    w.cl_ord_id(cid).symbol("AAPL").side('1').transact_time(kFixedUtc)
                     .order_qty(nf::Decimal{100, 0}).ord_type('2').price(nf::Decimal{18525, 2})
                     .no_party_i_ds(1, [](auto& g) {
                         g.entry().party_id("CLIENT1").party_id_source('D').party_role(1);
                     });
                });
            return r.ok() ? r.value() : 0;
        };
        for (std::size_t i = 0; i < warmup; ++i) sink += run_one(i);
        std::vector<std::uint64_t> lats;
        lats.reserve(samples);
        g_alloc_calls.store(0, std::memory_order_relaxed);
        g_track_alloc.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < samples; ++i) {
            const std::uint64_t t0 = now_ns();
            const auto n = run_one(i);
            const std::uint64_t t1 = now_ns();
            if (n == 0) { g_track_alloc.store(false); return false; }
            sink += static_cast<unsigned char>(wire[n - 1]);
            lats.push_back(t1 - t0);
        }
        g_track_alloc.store(false, std::memory_order_release);
        nf_stream = compute_stats(std::move(lats), g_alloc_calls.load());
    }

    // 1C. QuickFIX FIX44::NewOrderSingle + NoPartyIDs + toString()
    Stats qf_stats{};
    {
        std::string wire;
        wire.reserve(1024);
        char id_buf[32];
        const FIX::SenderCompID sender("BUY");
        const FIX::TargetCompID target("SELL");
        auto run_one = [&](std::size_t i) -> std::size_t {
            const auto cid = format_ord_id(id_buf, sizeof(id_buf), i);
            auto order = build_qf_order(cid, qf_transact);
            auto& hdr = order.getHeader();
            hdr.setField(sender);
            hdr.setField(target);
            hdr.setField(FIX::MsgSeqNum(static_cast<int>(i + 2)));
            hdr.setField(qf_sending);
            wire.clear();
            order.toString(wire);
            return wire.size();
        };
        for (std::size_t i = 0; i < warmup; ++i) sink += run_one(i);
        std::vector<std::uint64_t> lats;
        lats.reserve(samples);
        g_alloc_calls.store(0, std::memory_order_relaxed);
        g_track_alloc.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < samples; ++i) {
            const std::uint64_t t0 = now_ns();
            const auto n = run_one(i);
            const std::uint64_t t1 = now_ns();
            if (n == 0) { g_track_alloc.store(false); return false; }
            sink += static_cast<unsigned char>(wire[n - 1]);
            lats.push_back(t1 - t0);
        }
        g_track_alloc.store(false, std::memory_order_release);
        qf_stats = compute_stats(std::move(lats), g_alloc_calls.load());
    }

    (void)static_cast<std::uint64_t>(sink);
    print_row_ns("NimbleFIX StreamWriter + PartyGroup (0-alloc)", nf_stream, &qf_stats);
    print_row_ns("NimbleFIX Fixed + PartyGroup (0-alloc)", nf_fixed, &qf_stats);
    print_row_ns("QuickFIX FIX44::NewOrderSingle::toString", qf_stats);
    return true;
}

// ============================================================================
// Suite 2: Codec Parse + Full 912-Field FIX 4.4 Validation + Typed Field Read
// ============================================================================
bool run_suite2_parse(std::size_t warmup, std::size_t samples,
                      const FIX::DataDictionary& dict) {
    std::cout << "\n=== Suite 2: Parse + Full FIX44.xml Strict Validation + Typed Read (NewOrderSingle + NoPartyIDs[1], "
              << samples << " samples) ===\n";

    // Build one canonical FIX 4.4 frame with QuickFIX so both parse identical bytes.
    const FIX::UtcTimeStamp qf_ts(0, 0, 0, 23, 11, 2026);
    auto sample_order = build_qf_order("ORD-123456", FIX::TransactTime(qf_ts));
    sample_order.getHeader().setField(FIX::SenderCompID("BUY"));
    sample_order.getHeader().setField(FIX::TargetCompID("SELL"));
    sample_order.getHeader().setField(FIX::MsgSeqNum(42));
    sample_order.getHeader().setField(FIX::SendingTime(qf_ts));
    std::string wire;
    sample_order.toString(wire);

    volatile std::uint64_t sink = 0;

    // 2A. NimbleFIX inspect + parse_flat_verified + strict 912-field validation + typed view read
    Stats nf_stats{};
    {
        std::array<nf::FieldView, 64> scratch{};
        nf::detail::ParsedFieldIndex parse_index{};
        const auto hints = fix::Schema::wire_hints();
        auto run_one = [&]() -> bool {
            nf::FrameView fv{};
            if (!nf::inspect_frame(wire, fv, hints).ok()) return false;
            nf::FlatMessageView mv{};
            if (!nf::parse_flat_verified(fv, scratch, mv, hints).ok()) return false;
            mv.bind_index(&parse_index);
            if (!nf::validate_schema_message(fix::Schema::descriptor(), mv, nf::ValidationLevel::strict).ok()) return false;
            const auto nos = mv.as<fix::NewOrderSingle>();
            const auto cid = nos.cl_ord_id();
            const auto sym = nos.symbol();
            const auto side = nos.side();
            const auto qty = nos.order_qty();
            const auto px = nos.price();
            const auto grp = nos.no_party_i_ds();
            if (!cid.ok() || !sym.ok() || !side.ok() || !qty.ok() || !px.ok() || !grp.ok()) return false;
            std::size_t p_len = 0;
            for (const auto& entry : grp.value()) {
                const auto pid = entry.get<448>();
                if (pid.ok()) p_len += pid.value().size();
            }
            sink += cid.value().size() + sym.value().size() + static_cast<std::uint64_t>(side.value()) +
                    static_cast<std::uint64_t>(qty.value().coefficient()) +
                    static_cast<std::uint64_t>(px.value().coefficient()) + p_len;
            return p_len != 0;
        };
        for (std::size_t i = 0; i < warmup; ++i) { if (!run_one()) return false; }
        std::vector<std::uint64_t> lats;
        lats.reserve(samples);
        g_alloc_calls.store(0, std::memory_order_relaxed);
        g_track_alloc.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < samples; ++i) {
            const std::uint64_t t0 = now_ns();
            const bool ok = run_one();
            const std::uint64_t t1 = now_ns();
            if (!ok) { g_track_alloc.store(false); return false; }
            lats.push_back(t1 - t0);
        }
        g_track_alloc.store(false, std::memory_order_release);
        nf_stats = compute_stats(std::move(lats), g_alloc_calls.load());
    }

    // 2B. QuickFIX FIX::Message(wire, dict, true) + dict.validate + typed get() + getGroup()
    Stats qf_stats{};
    {
        auto run_one = [&]() -> bool {
            FIX44::NewOrderSingle msg;
            msg.setString(wire, true, &dict);
            dict.validate(msg);
            FIX::ClOrdID cid;
            FIX::Symbol sym;
            FIX::Side side;
            FIX::OrderQty qty;
            FIX::Price px;
            msg.get(cid);
            msg.get(sym);
            msg.get(side);
            msg.get(qty);
            msg.get(px);
            FIX44::NewOrderSingle::NoPartyIDs party;
            msg.getGroup(1, party);
            FIX::PartyID pid;
            party.get(pid);
            sink += cid.getValue().size() + sym.getValue().size() + static_cast<std::uint64_t>(side.getValue()) +
                    static_cast<std::uint64_t>(qty.getValue()) + static_cast<std::uint64_t>(px.getValue()) +
                    pid.getValue().size();
            return !pid.getValue().empty();
        };
        for (std::size_t i = 0; i < warmup; ++i) { if (!run_one()) return false; }
        std::vector<std::uint64_t> lats;
        lats.reserve(samples);
        g_alloc_calls.store(0, std::memory_order_relaxed);
        g_track_alloc.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < samples; ++i) {
            const std::uint64_t t0 = now_ns();
            const bool ok = run_one();
            const std::uint64_t t1 = now_ns();
            if (!ok) { g_track_alloc.store(false); return false; }
            lats.push_back(t1 - t0);
        }
        g_track_alloc.store(false, std::memory_order_release);
        qf_stats = compute_stats(std::move(lats), g_alloc_calls.load());
    }

    (void)static_cast<std::uint64_t>(sink);
    print_row_ns("NimbleFIX parse + FIX44 strict + view", nf_stats, &qf_stats);
    print_row_ns("QuickFIX setString + FIX44 validate + get", qf_stats);
    return true;
}

// ============================================================================
// Suite 3: Real Loopback TCP Initiator Ping-Pong RTT
//   Identical NeutralEchoPeer, identical FIX 4.4 dictionary validation,
//   real on-disk persistence (v2 mmap Journal vs FileStore), 0 logging.
// ============================================================================
struct DirectPingHandler {
    std::uint64_t received{};
    std::string_view expected_id{};
    bool mismatch{};
    [[nodiscard]] nf::Status on_message(const nf::FlatMessageView& msg, const nf::DeliveryInfo&) noexcept {
        if (msg.is<fix::ExecutionReport>()) {
            const auto cid = msg.as<fix::ExecutionReport>().cl_ord_id();
            if (!cid.ok() || cid.value() != expected_id) mismatch = true;
            ++received;
        }
        return nf::Status::success();
    }
};

bool run_nf_direct_initiator_rtt(std::size_t warmup, std::size_t samples, Stats& out) {
    TempDir dir("nf-qf-parity-direct-");
    NeutralEchoPeer peer(true);
    if (!peer.start()) return false;

    nf::DirectInitiatorConfig cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = peer.port();
    cfg.storage_directory = dir.path;
    cfg.session_key = "FIX.4.4/BUY/SELL";
    cfg.local_comp_id = "BUY";
    cfg.remote_comp_id = "SELL";
    cfg.heartbeat_ms = 60'000;
    cfg.journal_capacity_bytes = 64U * 1024U * 1024U;
    cfg.journal_segment_bytes = 4U * 1024U * 1024U;
    cfg.journal_max_records = 262'144;

    auto opened = nf::DirectInitiator<fix::Schema, DirectPingHandler>::open(std::move(cfg));
    if (!opened.ok()) return false;
    auto init = std::move(opened).value();
    if (!init.connect().ok()) return false;
    while (!init.is_active()) {
        if (!init.poll_once().ok()) return false;
    }

    char id_buf[32];
    auto write_order = [&](std::string_view cid) noexcept {
        return [cid](auto& w) noexcept {
            w.cl_ord_id(cid).symbol("AAPL").side('1').transact_time(kFixedUtc)
             .order_qty(nf::Decimal{100, 0}).ord_type('2').price(nf::Decimal{18525, 2})
             .no_party_i_ds(1, [](auto& g) noexcept {
                 g.entry().party_id("CLIENT1").party_id_source('D').party_role(1);
             });
        };
    };
    for (std::size_t i = 0; i < warmup; ++i) {
        const auto cid = format_ord_id(id_buf, sizeof(id_buf), i);
        init.handler().expected_id = cid;
        const auto target = init.handler().received + 1;
        if (!init.send_stream<fix::NewOrderSingle>(write_order(cid)).ok()) return false;
        while (init.handler().received < target) {
            if (!init.poll_once().ok()) return false;
        }
        if (init.needs_maintenance()) (void)init.poll_maintenance();
    }

    std::vector<std::uint64_t> lats;
    lats.reserve(samples);
    g_alloc_calls.store(0, std::memory_order_relaxed);
    const std::uint64_t wall0 = now_ns();
    for (std::size_t i = 0; i < samples; ++i) {
        const auto cid = format_ord_id(id_buf, sizeof(id_buf), warmup + i);
        init.handler().expected_id = cid;
        const auto target = init.handler().received + 1;
        g_track_alloc.store(true, std::memory_order_release);
        const std::uint64_t t0 = now_ns();
        if (!init.send_stream<fix::NewOrderSingle>(write_order(cid)).ok()) { g_track_alloc.store(false); return false; }
        while (init.handler().received < target) {
            if (!init.poll_once().ok()) { g_track_alloc.store(false); return false; }
        }
        const std::uint64_t t1 = now_ns();
        g_track_alloc.store(false, std::memory_order_release);
        if (init.handler().mismatch) return false;
        lats.push_back(t1 - t0);
        if (init.needs_maintenance()) (void)init.poll_maintenance();
    }
    const std::uint64_t wall_ns = now_ns() - wall0;

    (void)init.logout("done");
    for (int i = 0; i < 200 && init.valid_transport(); ++i) (void)init.poll_once();
    peer.stop();
    out = compute_stats(std::move(lats), g_alloc_calls.load(), wall_ns);
    return true;
}

bool run_nf_engine_initiator_rtt(nf::IoBackendConfig backend_cfg,
                                 std::size_t warmup, std::size_t samples, Stats& out) {
    TempDir dir("nf-qf-parity-eng-init-");
    NeutralEchoPeer peer(true);
    if (!peer.start()) return false;

    nf::EngineConfig ecfg{};
    ecfg.storage.directory = dir.path;
    ecfg.storage.journal_capacity_bytes = 64U * 1024U * 1024U;
    ecfg.storage.segment_bytes = 4U * 1024U * 1024U;
    ecfg.storage.journal_max_records = 262'144;
    ecfg.heartbeat_ms = 60'000;
    ecfg.io_backend = backend_cfg;

    auto created = nf::Engine<fix::Schema, nf::Inline>::create(std::move(ecfg));
    if (!created.ok()) return false;
    auto engine = std::move(created).value();

    std::atomic<bool> active{false};
    std::atomic<std::uint64_t> received{0};
    (void)engine.on_session_active([&](nf::SessionContext&) {
        active.store(true, std::memory_order_release);
        return nf::Status::success();
    });
    (void)engine.on<fix::ExecutionReport>([&](nf::SessionContext&, const fix::ExecutionReportView& er) {
        if (!er.cl_ord_id().ok()) return nf::Status{nf::ErrorCode::invalid_value};
        received.fetch_add(1, std::memory_order_release);
        return nf::Status::success();
    });

    (void)engine.on_error([](const nf::ErrorEvent& ev) {
        std::cerr << "NimbleFIX initiator error: " << ev.format() << '\n';
    });

    auto handle = engine.add_initiator({
        .name = "buy",
        .sender_comp_id = "BUY",
        .target_comp_id = "SELL",
        .endpoint = {"127.0.0.1", peer.port()},
    });
    if (!handle.ok()) return false;
    auto lane_res = handle.value().producer_lane<fix::Schema>({.capacity_messages = 64, .capacity_bytes = 65536});
    if (!lane_res.ok()) return false;
    auto lane = std::move(lane_res).value();

    std::thread io_thr([&] { (void)engine.run(); });
    const auto active_deadline = now_ns() + 5'000'000'000ULL;
    while (!active.load(std::memory_order_acquire)) {
        if (now_ns() > active_deadline) { engine.request_stop(); io_thr.join(); return false; }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    ReusablePartyOrder order;
    char id_buf[32];
    for (std::size_t i = 0; i < warmup; ++i) {
        const auto cid = format_ord_id(id_buf, sizeof(id_buf), i);
        const auto target = received.load(std::memory_order_acquire) + 1;
        if (!order.populate(cid).ok()) { engine.request_stop(); io_thr.join(); return false; }
        const auto deadline = now_ns() + 5'000'000'000ULL;
        while (!lane.send(order).ok()) {
            if (now_ns() > deadline) { engine.request_stop(); io_thr.join(); return false; }
        }
        while (received.load(std::memory_order_acquire) < target) {
            if (now_ns() > deadline) { engine.request_stop(); io_thr.join(); return false; }
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }
    }

    std::vector<std::uint64_t> lats;
    lats.reserve(samples);
    g_alloc_calls.store(0, std::memory_order_relaxed);
    const std::uint64_t wall0 = now_ns();
    for (std::size_t i = 0; i < samples; ++i) {
        const auto cid = format_ord_id(id_buf, sizeof(id_buf), warmup + i);
        const auto target = received.load(std::memory_order_acquire) + 1;
        g_track_alloc.store(true, std::memory_order_release);
        const std::uint64_t t0 = now_ns();
        const std::uint64_t deadline = t0 + 10'000'000'000ULL;
        if (!order.populate(cid).ok()) { g_track_alloc.store(false); engine.request_stop(); io_thr.join(); return false; }
        while (!lane.send(order).ok()) {
            if (now_ns() > deadline) { g_track_alloc.store(false); engine.request_stop(); io_thr.join(); return false; }
        }
        while (received.load(std::memory_order_acquire) < target) {
            if (now_ns() > deadline) { g_track_alloc.store(false); engine.request_stop(); io_thr.join(); return false; }
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }
        const std::uint64_t t1 = now_ns();
        g_track_alloc.store(false, std::memory_order_release);
        lats.push_back(t1 - t0);
    }
    const std::uint64_t wall_ns = now_ns() - wall0;

    engine.request_stop();
    io_thr.join();
    peer.stop();
    out = compute_stats(std::move(lats), g_alloc_calls.load(), wall_ns);
    return true;
}

class QfInitiatorApp final : public FIX::Application {
public:
    std::atomic<bool> logged_on{false};
    std::atomic<std::uint64_t> received{0};
    FIX::SessionID session_id{};

    void onCreate(const FIX::SessionID& id) override { session_id = id; }
    void onLogon(const FIX::SessionID&) override { logged_on.store(true, std::memory_order_release); }
    void onLogout(const FIX::SessionID&) override { logged_on.store(false, std::memory_order_release); }
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message& msg, const FIX::SessionID&) override {
        if (msg.getHeader().getField(FIX::FIELD::MsgType) == "3") {
            std::cerr << "QuickFIX received Reject: " << msg.toString() << '\n';
        }
    }
    void fromApp(const FIX::Message& msg, const FIX::SessionID&) override {
        if (msg.getHeader().getField(FIX::FIELD::MsgType) == "8") {
            FIX::ClOrdID cid;
            msg.getField(cid);
            if (!cid.getValue().empty()) {
                received.fetch_add(1, std::memory_order_release);
            }
        }
    }
};

bool run_qf_initiator_rtt(const std::string& dict_path,
                          std::size_t warmup, std::size_t samples, Stats& out) {
    TempDir dir("nf-qf-parity-qf-init-");
    NeutralEchoPeer peer(true);
    if (!peer.start()) return false;

    std::ostringstream cfg;
    cfg << "[DEFAULT]\n"
        << "ConnectionType=initiator\n"
        << "StartTime=00:00:00\n"
        << "EndTime=00:00:00\n"
        << "ReconnectInterval=1\n"
        << "HeartBtInt=60\n"
        << "SocketConnectHost=127.0.0.1\n"
        << "SocketConnectPort=" << peer.port() << "\n"
        << "SocketNodelay=Y\n"
        << "UseDataDictionary=Y\n"
        << "DataDictionary=" << dict_path << "\n"
        << "ValidateFieldsOutOfOrder=Y\n"
        << "ValidateFieldsHaveValues=Y\n"
        << "ValidateUserDefinedFields=Y\n"
        << "CheckLatency=Y\n"
        << "MaxLatency=120\n"
        << "PersistMessages=Y\n"
        << "ResetOnLogon=N\n"
        << "ResetOnLogout=N\n"
        << "ResetOnDisconnect=N\n"
        << "FileStorePath=" << dir.path << "\n"
        << "[SESSION]\n"
        << "BeginString=FIX.4.4\n"
        << "SenderCompID=BUY\n"
        << "TargetCompID=SELL\n";

    std::istringstream input{cfg.str()};
    FIX::SessionSettings settings{input};
    FIX::FileStoreFactory store_factory{settings};
    QfInitiatorApp app;
    FIX::SocketInitiator initiator{app, store_factory, settings};
    initiator.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!app.logged_on.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!app.logged_on.load(std::memory_order_acquire)) {
        initiator.stop(true);
        return false;
    }

    const FIX::UtcTimeStamp qf_ts(0, 0, 0, 23, 11, 2026);
    const FIX::TransactTime qf_transact(qf_ts);
    char id_buf[32];

    for (std::size_t i = 0; i < warmup; ++i) {
        const auto cid = format_ord_id(id_buf, sizeof(id_buf), i);
        const auto target = app.received.load(std::memory_order_acquire) + 1;
        auto order = build_qf_order(cid, qf_transact);
        if (!FIX::Session::sendToTarget(order, app.session_id)) { initiator.stop(true); return false; }
        const auto wait_deadline = now_ns() + 5'000'000'000ULL;
        while (app.received.load(std::memory_order_acquire) < target) {
            if (now_ns() > wait_deadline) {
                std::cerr << "QuickFIX initiator warmup timed out at i=" << i
                          << " peer_orders=" << peer.received_orders() << '\n';
                initiator.stop(true);
                return false;
            }
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }
    }

    std::vector<std::uint64_t> lats;
    lats.reserve(samples);
    g_alloc_calls.store(0, std::memory_order_relaxed);
    const std::uint64_t wall0 = now_ns();
    for (std::size_t i = 0; i < samples; ++i) {
        const auto cid = format_ord_id(id_buf, sizeof(id_buf), warmup + i);
        const auto target = app.received.load(std::memory_order_acquire) + 1;
        g_track_alloc.store(true, std::memory_order_release);
        const std::uint64_t t0 = now_ns();
        const std::uint64_t wait_deadline = t0 + 10'000'000'000ULL;
        auto order = build_qf_order(cid, qf_transact);
        if (!FIX::Session::sendToTarget(order, app.session_id)) {
            g_track_alloc.store(false);
            initiator.stop(true);
            return false;
        }
        while (app.received.load(std::memory_order_acquire) < target) {
            if (now_ns() > wait_deadline) {
                g_track_alloc.store(false);
                initiator.stop(true);
                return false;
            }
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }
        const std::uint64_t t1 = now_ns();
        g_track_alloc.store(false, std::memory_order_release);
        lats.push_back(t1 - t0);
    }
    const std::uint64_t wall_ns = now_ns() - wall0;

    initiator.stop();
    peer.stop();
    out = compute_stats(std::move(lats), g_alloc_calls.load(), wall_ns);
    return true;
}

// ============================================================================
// Suite 4: Real Loopback TCP Acceptor Turnaround RTT
//   Identical neutral raw-TCP ClientPeer drives BOTH Acceptors.
// ============================================================================
bool drive_acceptor_client(std::uint16_t port, std::size_t warmup, std::size_t samples,
                           Stats& out) {
    int fd = -1;
    const auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < connect_deadline) {
        fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) break;
        ::close(fd);
        fd = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (fd < 0) return false;
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    std::array<char, 2048> tx_buf{};
    std::array<char, 8192> rx_buf{};
    std::size_t rx_head = 0, rx_tail = 0;
    std::array<nf::FieldView, 64> fields{};
    std::uint64_t out_seq = 1;

    auto recv_frame = [&](std::string_view& out_type, std::string_view& out_cid) -> bool {
        while (true) {
            if (rx_head < rx_tail) {
                nf::FrameView fv{};
                const auto st = nf::inspect_frame({rx_buf.data() + rx_head, rx_tail - rx_head}, fv);
                if (st.ok()) {
                    rx_head += fv.consumed;
                    nf::FlatMessageView mv{};
                    if (!nf::parse_flat_verified(fv, fields, mv).ok()) return false;
                    out_type = mv.msg_type();
                    const auto cid = mv.find_unique(11);
                    out_cid = cid.ok() ? cid.value().value : std::string_view{};
                    if (rx_head == rx_tail) rx_head = rx_tail = 0;
                    return true;
                }
                if (st.code != nf::ErrorCode::need_more) return false;
            }
            if (rx_tail == rx_buf.size()) {
                if (rx_head == 0) return false;
                const std::size_t rem = rx_tail - rx_head;
                std::memmove(rx_buf.data(), rx_buf.data() + rx_head, rem);
                rx_head = 0;
                rx_tail = rem;
            }
            const ssize_t n = ::recv(fd, rx_buf.data() + rx_tail, rx_buf.size() - rx_tail, 0);
            if (n <= 0) return false;
            rx_tail += static_cast<std::size_t>(n);
        }
    };

    // FIX 4.4 Logon handshake
    const std::string session_utc = current_utc_timestamp();
    {
        auto logon = nf::message<"A">().set<98>(0).set<108>(60);
        const auto enc = nf::encode_fix44(logon, nf::SessionHeader{"BUY", "SELL", out_seq++, session_utc}, tx_buf);
        if (!enc.ok() || !send_all(fd, {tx_buf.data(), enc.value()})) { ::close(fd); return false; }
        std::string_view mt, cid;
        if (!recv_frame(mt, cid) || mt != "A") { ::close(fd); return false; }
    }

    // Pre-encode frames outside client timing so client overhead is minimal & identical.
    ReusablePartyOrder order;
    char id_buf[32];

    for (std::size_t i = 0; i < warmup; ++i) {
        const auto cid = format_ord_id(id_buf, sizeof(id_buf), i);
        if (!order.populate(cid, session_utc).ok()) { ::close(fd); return false; }
        const auto enc = nf::encode_fix44(order.view(), nf::SessionHeader{"BUY", "SELL", out_seq++, session_utc}, tx_buf);
        if (!enc.ok() || !send_all(fd, {tx_buf.data(), enc.value()})) { ::close(fd); return false; }
        std::string_view mt, er_cid;
        if (!recv_frame(mt, er_cid) || mt != "8" || er_cid != cid) { ::close(fd); return false; }
    }

    std::vector<std::uint64_t> lats;
    lats.reserve(samples);
    g_alloc_calls.store(0, std::memory_order_relaxed);
    const std::uint64_t wall0 = now_ns();
    for (std::size_t i = 0; i < samples; ++i) {
        const auto cid = format_ord_id(id_buf, sizeof(id_buf), warmup + i);
        if (!order.populate(cid, session_utc).ok()) { ::close(fd); return false; }
        const auto enc = nf::encode_fix44(order.view(), nf::SessionHeader{"BUY", "SELL", out_seq++, session_utc}, tx_buf);
        if (!enc.ok()) { ::close(fd); return false; }

        g_track_alloc.store(true, std::memory_order_release);
        const std::uint64_t t0 = now_ns();
        if (!send_all(fd, {tx_buf.data(), enc.value()})) { g_track_alloc.store(false); ::close(fd); return false; }
        std::string_view mt, er_cid;
        const bool got = recv_frame(mt, er_cid);
        const std::uint64_t t1 = now_ns();
        g_track_alloc.store(false, std::memory_order_release);
        if (!got || mt != "8" || er_cid != cid) { ::close(fd); return false; }
        lats.push_back(t1 - t0);
    }
    const std::uint64_t wall_ns = now_ns() - wall0;

    // Logout
    {
        const std::string utc = current_utc_timestamp();
        auto logout = nf::message<"5">();
        const auto enc = nf::encode_fix44(logout, nf::SessionHeader{"BUY", "SELL", out_seq++, utc}, tx_buf);
        if (enc.ok()) (void)send_all(fd, {tx_buf.data(), enc.value()});
    }
    ::close(fd);
    out = compute_stats(std::move(lats), g_alloc_calls.load(), wall_ns);
    return true;
}

bool run_nf_acceptor_rtt(nf::IoBackendConfig backend_cfg,
                         std::size_t warmup, std::size_t samples, Stats& out) {
    TempDir dir("nf-qf-parity-nf-acc-");

    nf::EngineConfig ecfg{};
    ecfg.storage.directory = dir.path;
    ecfg.storage.journal_capacity_bytes = 64U * 1024U * 1024U;
    ecfg.storage.segment_bytes = 4U * 1024U * 1024U;
    ecfg.storage.journal_max_records = 262'144;
    ecfg.heartbeat_ms = 60'000;
    ecfg.io_backend = backend_cfg;

    auto created = nf::Engine<fix::Schema, nf::Inline>::create(std::move(ecfg));
    if (!created.ok()) return false;
    auto engine = std::move(created).value();

    fix::ExecutionReportFixed<ErShape, 128> er;
    (void)engine.on<fix::NewOrderSingle>([&](nf::SessionContext& ctx, const fix::NewOrderSingleView& order) {
        const auto cid = order.cl_ord_id();
        if (!cid.ok()) return nf::Status{nf::ErrorCode::invalid_value};
        if (const auto st = populate_nf_er(er, cid.value()); !st.ok()) return st;
        return ctx.send(er).status();
    });
    if (!engine.add_listener({.name = "main", .endpoint = {"127.0.0.1", 0}}).ok()) return false;
    if (!engine.add_acceptor({
            .name = "sell",
            .listener = "main",
            .local_comp_id = "SELL",
            .remote_comp_id = "BUY",
        }).ok()) return false;

    std::atomic<std::uint16_t> bound_port{0};
    std::thread io_thr([&] { (void)engine.run(); });
    for (int i = 0; i < 500; ++i) {
        const auto ep = engine.listener_endpoint("main");
        if (ep.ok() && ep.value().port != 0) {
            bound_port.store(ep.value().port, std::memory_order_release);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const std::uint16_t port = bound_port.load(std::memory_order_acquire);
    if (port == 0) { engine.request_stop(); io_thr.join(); return false; }

    const bool ok = drive_acceptor_client(port, warmup, samples, out);
    engine.request_stop();
    io_thr.join();
    return ok;
}

class QfAcceptorApp final : public FIX::Application {
public:
    void onCreate(const FIX::SessionID&) override {}
    void onLogon(const FIX::SessionID&) override {}
    void onLogout(const FIX::SessionID&) override {}
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message&, const FIX::SessionID&) override {}
    void fromApp(const FIX::Message& msg, const FIX::SessionID& session_id) override {
        if (msg.getHeader().getField(FIX::FIELD::MsgType) != "D") return;
        FIX::ClOrdID cid;
        msg.getField(cid);
        auto er = build_qf_er(cid.getValue());
        FIX::Session::sendToTarget(er, session_id);
    }
};

bool run_qf_acceptor_rtt(const std::string& dict_path,
                         std::size_t warmup, std::size_t samples, Stats& out) {
    TempDir dir("nf-qf-parity-qf-acc-");
    const std::uint16_t port = reserve_loopback_port();
    if (port == 0) return false;

    std::ostringstream cfg;
    cfg << "[DEFAULT]\n"
        << "ConnectionType=acceptor\n"
        << "StartTime=00:00:00\n"
        << "EndTime=00:00:00\n"
        << "HeartBtInt=60\n"
        << "SocketAcceptPort=" << port << "\n"
        << "SocketNodelay=Y\n"
        << "UseDataDictionary=Y\n"
        << "DataDictionary=" << dict_path << "\n"
        << "ValidateFieldsOutOfOrder=Y\n"
        << "ValidateFieldsHaveValues=Y\n"
        << "ValidateUserDefinedFields=Y\n"
        << "CheckLatency=Y\n"
        << "MaxLatency=120\n"
        << "PersistMessages=Y\n"
        << "ResetOnLogon=N\n"
        << "ResetOnLogout=N\n"
        << "ResetOnDisconnect=N\n"
        << "FileStorePath=" << dir.path << "\n"
        << "[SESSION]\n"
        << "BeginString=FIX.4.4\n"
        << "SenderCompID=SELL\n"
        << "TargetCompID=BUY\n";

    std::istringstream input{cfg.str()};
    FIX::SessionSettings settings{input};
    FIX::FileStoreFactory store_factory{settings};
    QfAcceptorApp app;
    FIX::SocketAcceptor acceptor{app, store_factory, settings};
    acceptor.start();

    const bool ok = drive_acceptor_client(port, warmup, samples, out);
    acceptor.stop();
    return ok;
}

// ============================================================================
// Suite 5: Open-Loop Dense Burst (Initiator -> Neutral Peer, CO-Free 1-Way)
// ============================================================================
bool run_nf_direct_burst(bool batched, std::size_t warmup_bursts,
                         std::size_t bursts, std::size_t burst_size, Stats& out) {
    TempDir dir("nf-qf-parity-nf-burst-");
    NeutralEchoPeer peer(false);
    if (!peer.start()) return false;

    nf::DirectInitiatorConfig cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = peer.port();
    cfg.storage_directory = dir.path;
    cfg.session_key = "FIX.4.4/BUY/SELL";
    cfg.local_comp_id = "BUY";
    cfg.remote_comp_id = "SELL";
    cfg.heartbeat_ms = 60'000;
    cfg.max_pending_messages = 256;
    cfg.journal_capacity_bytes = 64U * 1024U * 1024U;
    cfg.journal_segment_bytes = 4U * 1024U * 1024U;
    cfg.journal_max_records = 262'144;

    auto opened = nf::DirectInitiator<fix::Schema>::open(std::move(cfg));
    if (!opened.ok()) return false;
    auto init = std::move(opened).value();
    if (!init.connect().ok()) return false;
    while (!init.is_active()) {
        if (!init.poll_once().ok()) return false;
    }

    char id_buf[64];

    auto send_burst = [&](std::uint64_t t_burst) -> bool {
        for (std::size_t j = 0; j < burst_size; ++j) {
            const std::uint64_t t_send = now_ns();
            const auto cid = format_burst_id(id_buf, sizeof(id_buf), t_burst, t_send);
            auto write_order = [cid](auto& w) noexcept {
                w.cl_ord_id(cid).symbol("AAPL").side('1').transact_time(kFixedUtc)
                 .order_qty(nf::Decimal{100, 0}).ord_type('2').price(nf::Decimal{18525, 2})
                 .no_party_i_ds(1, [](auto& g) noexcept {
                     g.entry().party_id("CLIENT1").party_id_source('D').party_role(1);
                 });
            };
            if (batched) {
                if (!init.enqueue_stream<fix::NewOrderSingle>(write_order).ok()) return false;
            } else {
                if (!init.send_stream<fix::NewOrderSingle>(write_order).ok()) return false;
            }
        }
        if (batched) {
            if (!init.flush().ok()) return false;
        }
        while (init.wants_write()) {
            if (!init.poll_once().ok()) return false;
        }
        return true;
    };

    for (std::size_t b = 0; b < warmup_bursts; ++b) {
        const auto target = peer.received_orders() + burst_size;
        if (!send_burst(now_ns())) return false;
        while (peer.received_orders() < target) {
            (void)init.poll_once();
        }
        if (init.needs_maintenance()) (void)init.poll_maintenance();
    }

    const std::size_t total_orders = bursts * burst_size;
    peer.begin_capture(total_orders);
    g_alloc_calls.store(0, std::memory_order_relaxed);
    std::uint64_t active_ns = 0;
    for (std::size_t b = 0; b < bursts; ++b) {
        const auto target = peer.received_orders() + burst_size;
        g_track_alloc.store(true, std::memory_order_release);
        const std::uint64_t t_burst = now_ns();
        const std::uint64_t burst_deadline = t_burst + 10'000'000'000ULL;
        if (!send_burst(t_burst)) { g_track_alloc.store(false); return false; }
        g_track_alloc.store(false, std::memory_order_release);
        while (peer.received_orders() < target) {
            if (now_ns() > burst_deadline) return false;
            (void)init.poll_once();
        }
        active_ns += now_ns() - t_burst;
        if (init.needs_maintenance()) (void)init.poll_maintenance();
    }
    peer.end_capture();

    (void)init.logout("done");
    for (int i = 0; i < 200 && init.valid_transport(); ++i) (void)init.poll_once();
    peer.stop();

    out = compute_stats(peer.burst_latencies(), g_alloc_calls.load(), active_ns);
    return peer.burst_latencies().size() == total_orders;
}

bool run_qf_burst(const std::string& dict_path, std::size_t warmup_bursts,
                  std::size_t bursts, std::size_t burst_size, Stats& out) {
    TempDir dir("nf-qf-parity-qf-burst-");
    NeutralEchoPeer peer(false);
    if (!peer.start()) return false;

    std::ostringstream cfg;
    cfg << "[DEFAULT]\n"
        << "ConnectionType=initiator\n"
        << "StartTime=00:00:00\n"
        << "EndTime=00:00:00\n"
        << "ReconnectInterval=1\n"
        << "HeartBtInt=60\n"
        << "SocketConnectHost=127.0.0.1\n"
        << "SocketConnectPort=" << peer.port() << "\n"
        << "SocketNodelay=Y\n"
        << "UseDataDictionary=Y\n"
        << "DataDictionary=" << dict_path << "\n"
        << "ValidateFieldsOutOfOrder=Y\n"
        << "ValidateFieldsHaveValues=Y\n"
        << "ValidateUserDefinedFields=Y\n"
        << "CheckLatency=Y\n"
        << "MaxLatency=120\n"
        << "PersistMessages=Y\n"
        << "ResetOnLogon=N\n"
        << "ResetOnLogout=N\n"
        << "ResetOnDisconnect=N\n"
        << "FileStorePath=" << dir.path << "\n"
        << "[SESSION]\n"
        << "BeginString=FIX.4.4\n"
        << "SenderCompID=BUY\n"
        << "TargetCompID=SELL\n";

    std::istringstream input{cfg.str()};
    FIX::SessionSettings settings{input};
    FIX::FileStoreFactory store_factory{settings};
    QfInitiatorApp app;
    FIX::SocketInitiator initiator{app, store_factory, settings};
    initiator.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!app.logged_on.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!app.logged_on.load(std::memory_order_acquire)) {
        initiator.stop(true);
        return false;
    }

    const FIX::UtcTimeStamp qf_ts(0, 0, 0, 23, 11, 2026);
    const FIX::TransactTime qf_transact(qf_ts);
    char id_buf[64];

    for (std::size_t b = 0; b < warmup_bursts; ++b) {
        const auto target = peer.received_orders() + burst_size;
        const std::uint64_t t_burst = now_ns();
        for (std::size_t j = 0; j < burst_size; ++j) {
            const auto cid = format_burst_id(id_buf, sizeof(id_buf), t_burst, now_ns());
            auto order = build_qf_order(cid, qf_transact);
            if (!FIX::Session::sendToTarget(order, app.session_id)) { initiator.stop(true); return false; }
        }
        while (peer.received_orders() < target) {
            std::this_thread::yield();
        }
    }

    const std::size_t total_orders = bursts * burst_size;
    peer.begin_capture(total_orders);
    g_alloc_calls.store(0, std::memory_order_relaxed);
    std::uint64_t active_ns = 0;
    for (std::size_t b = 0; b < bursts; ++b) {
        const auto target = peer.received_orders() + burst_size;
        g_track_alloc.store(true, std::memory_order_release);
        const std::uint64_t t_burst = now_ns();
        const std::uint64_t burst_deadline = t_burst + 10'000'000'000ULL;
        for (std::size_t j = 0; j < burst_size; ++j) {
            const auto cid = format_burst_id(id_buf, sizeof(id_buf), t_burst, now_ns());
            auto order = build_qf_order(cid, qf_transact);
            if (!FIX::Session::sendToTarget(order, app.session_id)) {
                g_track_alloc.store(false);
                initiator.stop(true);
                return false;
            }
        }
        g_track_alloc.store(false, std::memory_order_release);
        while (peer.received_orders() < target) {
            if (now_ns() > burst_deadline) { initiator.stop(true); return false; }
            std::this_thread::yield();
        }
        active_ns += now_ns() - t_burst;
    }
    peer.end_capture();

    initiator.stop();
    peer.stop();
    out = compute_stats(peer.burst_latencies(), g_alloc_calls.load(), active_ns);
    return peer.burst_latencies().size() == total_orders;
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t warmup = argc > 1 ? std::stoull(argv[1]) : 1'000;
    const std::size_t samples = argc > 2 ? std::stoull(argv[2]) : 5'000;
    const std::string dict_path = argc > 3 ? argv[3] : "schemas/standard/FIX44.xml";

    if (!std::filesystem::is_regular_file(dict_path)) {
        std::cerr << "FIX 4.4 dictionary not found: " << dict_path << '\n';
        return 2;
    }

    std::cout << "====================================================================================\n"
              << "NimbleFIX 0.3 vs QuickFIX 1.16.0 — Strictly Same-Methodology (Apples-to-Apples) Bench\n"
              << "  Dictionary : " << dict_path << " (Full 912-field FIX 4.4, strict validation ON)\n"
              << "  Order      : NewOrderSingle (7 scalars + NoPartyIDs[1]: 448/447/452), dynamic ClOrdID\n"
              << "  Ack        : ExecutionReport (11 business fields, populated in callback)\n"
              << "  Store      : NimbleFIX v2 mmap Journal vs QuickFIX FileStore (OS page-cache, no fsync)\n"
              << "  Logging    : Disabled on both engines | TCP: 127.0.0.1 TCP_NODELAY=1 neutral peer\n"
              << "  Allocators : Unified -Wl,--wrap=malloc + operator new/delete across both engines\n"
              << "====================================================================================\n";

    try {
        FIX::DataDictionary dict(dict_path);

        if (!run_suite1_encode(warmup, samples, dict)) {
            std::cerr << "Suite 1 (Encode) failed\n";
            return 1;
        }
        if (!run_suite2_parse(warmup, samples, dict)) {
            std::cerr << "Suite 2 (Parse + Validate) failed\n";
            return 1;
        }

        // Suite 3: TCP Initiator Ping-Pong RTT
        std::cout << "\n=== Suite 3: Real Loopback TCP Initiator Ping-Pong RTT ("
                  << samples << " orders, warmup " << warmup << ") ===\n" << std::flush;
        Stats init_direct{}, init_busy{}, init_epoll{}, init_qf{};
        if (!run_nf_direct_initiator_rtt(warmup, samples, init_direct)) {
            std::cerr << "Suite 3 (DirectInitiator) failed\n"; return 1;
        }
        if (!run_nf_engine_initiator_rtt(nf::IoBackendConfig::busy_poll(), warmup, samples, init_busy)) {
            std::cerr << "Suite 3 (Engine busy_poll) failed\n"; return 1;
        }
        if (!run_nf_engine_initiator_rtt(nf::IoBackendConfig::epoll(), warmup, samples, init_epoll)) {
            std::cerr << "Suite 3 (Engine epoll) failed\n"; return 1;
        }
        if (!run_qf_initiator_rtt(dict_path, warmup, samples, init_qf)) {
            std::cerr << "Suite 3 (QuickFIX SocketInitiator) failed\n"; return 1;
        }
        print_row_us("NimbleFIX DirectInitiator (Layer 0)", init_direct, &init_qf);
        print_row_us("NimbleFIX Engine (busy_poll)", init_busy, &init_qf);
        print_row_us("NimbleFIX Engine (epoll)", init_epoll, &init_qf);
        print_row_us("QuickFIX SocketInitiator (FileStore)", init_qf);

        // Suite 4: TCP Acceptor Turnaround RTT
        std::cout << "\n=== Suite 4: Real Loopback TCP Acceptor Turnaround RTT ("
                  << samples << " orders, warmup " << warmup << ") ===\n";
        Stats acc_busy{}, acc_epoll{}, acc_qf{};
        if (!run_nf_acceptor_rtt(nf::IoBackendConfig::busy_poll(), warmup, samples, acc_busy) ||
            !run_nf_acceptor_rtt(nf::IoBackendConfig::epoll(), warmup, samples, acc_epoll) ||
            !run_qf_acceptor_rtt(dict_path, warmup, samples, acc_qf)) {
            std::cerr << "Suite 4 (TCP Acceptor Turnaround) failed\n";
            return 1;
        }
        print_row_us("NimbleFIX Engine Acceptor (busy_poll)", acc_busy, &acc_qf);
        print_row_us("NimbleFIX Engine Acceptor (epoll)", acc_epoll, &acc_qf);
        print_row_us("QuickFIX SocketAcceptor (FileStore)", acc_qf);

        // Suite 5: Open-Loop Dense Burst (CO-Free One-Way Latency & Throughput)
        const std::size_t burst_size = 32;
        const std::size_t warmup_bursts = std::max<std::size_t>(2, warmup / burst_size);
        const std::size_t bursts = std::max<std::size_t>(5, samples / burst_size);
        std::cout << "\n=== Suite 5: Open-Loop Dense Burst (" << bursts << " bursts x " << burst_size
                  << " orders, CO-Free 1-way) ===\n";
        Stats burst_direct{}, burst_batch{}, burst_qf{};
        if (!run_nf_direct_burst(false, warmup_bursts, bursts, burst_size, burst_direct) ||
            !run_nf_direct_burst(true, warmup_bursts, bursts, burst_size, burst_batch) ||
            !run_qf_burst(dict_path, warmup_bursts, bursts, burst_size, burst_qf)) {
            std::cerr << "Suite 5 (Open-Loop Dense Burst) failed\n";
            return 1;
        }
        print_row_us("NimbleFIX DirectInitiator::send()", burst_direct, &burst_qf);
        print_row_us("NimbleFIX DirectInitiator batch sendmsg", burst_batch, &burst_qf);
        print_row_us("QuickFIX SocketInitiator sendToTarget", burst_qf);
    } catch (const std::exception& ex) {
        std::cerr << "quickfix-parity-bench exception: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
