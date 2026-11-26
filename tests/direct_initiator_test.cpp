#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/fix44_sample.hpp>
#include <nimblefix/message.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

// Linker --wrap counters to enforce zero malloc, zero mutex lock, and zero fsync
// on DirectInitiator's steady-state send() and poll_once() hot path!
namespace {
std::atomic<bool> g_audit_hot_path{false};
std::atomic<std::uint64_t> g_malloc_calls{0};
std::atomic<std::uint64_t> g_mutex_calls{0};
std::atomic<std::uint64_t> g_fsync_calls{0};
} // namespace

#ifdef NIMBLEFIX_AUDIT_WRAPS
extern "C" {
void* __real_malloc(std::size_t n);
void* __wrap_malloc(std::size_t n) {
    if (g_audit_hot_path.load(std::memory_order_relaxed))
        g_malloc_calls.fetch_add(1, std::memory_order_relaxed);
    return __real_malloc(n);
}
int __real_pthread_mutex_lock(void* m);
int __wrap_pthread_mutex_lock(void* m) {
    if (g_audit_hot_path.load(std::memory_order_relaxed))
        g_mutex_calls.fetch_add(1, std::memory_order_relaxed);
    return __real_pthread_mutex_lock(m);
}
int __real_fsync(int fd);
int __wrap_fsync(int fd) {
    if (g_audit_hot_path.load(std::memory_order_relaxed))
        g_fsync_calls.fetch_add(1, std::memory_order_relaxed);
    return __real_fsync(fd);
}
int __real_fdatasync(int fd);
int __wrap_fdatasync(int fd) {
    if (g_audit_hot_path.load(std::memory_order_relaxed))
        g_fsync_calls.fetch_add(1, std::memory_order_relaxed);
    return __real_fdatasync(fd);
}
int __real_msync(void* addr, std::size_t len, int flags);
int __wrap_msync(void* addr, std::size_t len, int flags) {
    if (g_audit_hot_path.load(std::memory_order_relaxed))
        g_fsync_calls.fetch_add(1, std::memory_order_relaxed);
    return __real_msync(addr, len, flags);
}
} // extern "C"
#endif

namespace nf = nimblefix;
namespace fix = nimblefix::fix44_sample;

namespace {

int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #cond << '\n'; \
        ++failures; \
    } \
} while (false)

constexpr std::string_view t0{"20261123-00:00:00.000"};
constexpr std::string_view t1{"20261123-00:00:01.000"};
constexpr std::string_view t2{"20261123-00:00:02.000"};
constexpr std::string_view t3{"20261123-00:00:03.000"};

struct TempDir {
    std::string path;
    TempDir() {
        char pat[] = "/tmp/nimblefix-direct-XXXXXX";
        if (char* p = ::mkdtemp(pat)) path = p;
    }
    ~TempDir() {
        if (!path.empty()) std::filesystem::remove_all(path);
    }
};

// Zero-overhead in-memory loopback transport satisfying DirectTransport concept.
struct LoopbackTransport {
    std::string rx_wire;
    std::size_t rx_pos{};
    std::string tx_wire;
    bool open_state{};
    bool peer_eof{};
    bool block_writes{};
    std::size_t write_batch_calls{};
    std::size_t write_batch_chunks{};

    [[nodiscard]] nf::Status connect(std::string_view, std::uint16_t) noexcept {
        open_state = true;
        peer_eof = false;
        return nf::Status::success();
    }
    [[nodiscard]] nf::Status finish_connect() noexcept { return nf::Status::success(); }
    [[nodiscard]] nf::TransportIoResult read(std::span<char> out) noexcept {
        if (!open_state) return {0, false, true, nf::Status::success()};
        const std::size_t avail = rx_wire.size() - rx_pos;
        if (avail == 0) {
            if (peer_eof) return {0, false, true, nf::Status::success()};
            return {0, true, false, nf::Status::success()};
        }
        const std::size_t n = std::min(avail, out.size());
        std::memcpy(out.data(), rx_wire.data() + rx_pos, n);
        rx_pos += n;
        return {n, false, false, nf::Status::success()};
    }
    [[nodiscard]] nf::TransportIoResult write(std::span<const char> in) noexcept {
        if (!open_state) return {0, false, true, nf::Status{nf::ErrorCode::io_error}};
        if (block_writes) return {0, true, false, nf::Status::success()};
        tx_wire.append(in.data(), in.size());
        return {in.size(), false, false, nf::Status::success()};
    }
    [[nodiscard]] nf::TransportIoResult write_batch(std::span<const std::span<const char>> chunks) noexcept {
        if (!open_state) return {0, false, true, nf::Status{nf::ErrorCode::io_error}};
        if (block_writes) return {0, true, false, nf::Status::success()};
        ++write_batch_calls;
        write_batch_chunks += chunks.size();
        std::size_t total = 0;
        for (const auto& c : chunks) {
            tx_wire.append(c.data(), c.size());
            total += c.size();
        }
        return {total, false, false, nf::Status::success()};
    }
    void close() noexcept { open_state = false; }
    [[nodiscard]] int native_fd() const noexcept { return open_state ? 1 : -1; }
    [[nodiscard]] bool valid() const noexcept { return open_state; }
    [[nodiscard]] bool is_connecting() const noexcept { return false; }

    void push_from_peer(std::string_view frame) { rx_wire.append(frame.data(), frame.size()); }
    bool pop_sent_frame(std::string& out) {
        nf::FrameView fv{};
        if (!nf::inspect_frame(tx_wire, fv).ok()) return false;
        out.assign(fv.bytes);
        tx_wire.erase(0, fv.consumed);
        return true;
    }
};
static_assert(nf::DirectTransport<LoopbackTransport>);

template <class Message>
std::string make_frame(const Message& msg, std::string_view sender, std::string_view target,
                       std::uint64_t seq, std::string_view wt, bool poss_dup = false,
                       std::string_view orig_time = {}) {
    std::array<char, 4096> buf{};
    nf::SessionHeader h{sender, target, seq, wt, poss_dup, orig_time};
    const auto n = nf::encode_fix(msg, h, buf);
    CHECK(n.ok());
    return n.ok() ? std::string{buf.data(), n.value()} : std::string{};
}

template <class Message, class Fn>
std::string make_stream_frame(Fn&& fn, std::string_view sender, std::string_view target,
                              std::uint64_t seq, std::string_view wt, bool poss_dup = false,
                              std::string_view orig_time = {}) {
    std::array<char, 4096> buf{};
    nf::SessionHeader h{sender, target, seq, wt, poss_dup, orig_time};
    const auto n = nf::encode_stream<Message>(h, buf, std::forward<Fn>(fn));
    CHECK(n.ok());
    return n.ok() ? std::string{buf.data(), n.value()} : std::string{};
}

std::string field(std::string_view frame, std::uint32_t tag) {
    std::array<nf::FieldView, 256> scratch{};
    nf::FrameView fv{};
    if (!nf::inspect_frame(frame, fv).ok()) return {};
    nf::FlatMessageView msg{};
    if (!nf::parse_flat(fv, scratch, msg).ok()) return {};
    const auto f = msg.find_unique(tag);
    return f.ok() ? std::string{f.value().value} : std::string{};
}

struct RecordingHandler {
    std::uint64_t active_count{};
    std::uint64_t msg_count{};
    std::uint64_t recovery_count{};
    std::uint64_t last_seq{};
    std::string last_exec_id;

    [[nodiscard]] nf::Status on_active() noexcept {
        ++active_count;
        return nf::Status::success();
    }

    [[nodiscard]] nf::Status on_message(const nf::FlatMessageView& msg,
                                        const nf::DeliveryInfo& info) noexcept {
        ++msg_count;
        if (info.is_recovery) ++recovery_count;
        last_seq = info.msg_seq_num;
        const auto eid = msg.find_unique(17);
        if (eid.ok() && !g_audit_hot_path.load(std::memory_order_relaxed)) {
            last_exec_id.assign(eid.value().value);
        }
        return nf::Status::success();
    }
};

nf::DirectInitiatorConfig make_config(const TempDir& dir) {
    nf::DirectInitiatorConfig cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = 9999;
    cfg.storage_directory = dir.path;
    cfg.session_key = "FIX.4.4/BUY/SELL";
    cfg.local_comp_id = "BUY";
    cfg.remote_comp_id = "SELL";
    cfg.heartbeat_ms = 30'000;
    cfg.journal_capacity_bytes = 4U * 1024U * 1024U;
    cfg.journal_segment_bytes = 256U * 1024U;
    cfg.journal_max_segments = 0; // Auto-derived from capacity minus audit budget.
    cfg.journal_max_records = 16'384;
    return cfg;
}

void test_zero_alloc_zero_lock_zero_fsync_steady_state() {
    TempDir dir;
    auto opened = nf::DirectInitiator<fix::Schema, RecordingHandler, LoopbackTransport>::open(
        make_config(dir));
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto init = std::move(opened).value();
    init.transport().tx_wire.reserve(1U << 20);
    init.transport().rx_wire.reserve(1U << 20);

    CHECK(init.connect(0, t0).ok());
    std::string logon_out;
    CHECK(init.transport().pop_sent_frame(logon_out));
    CHECK(field(logon_out, 35) == "A" && field(logon_out, 34) == "1");

    const auto peer_logon = make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 1, t0);
    init.transport().push_from_peer(peer_logon);
    CHECK(init.poll_once(1, t0).ok());
    CHECK(init.is_active());
    CHECK(init.handler().active_count == 1);

    // Pre-build 200 peer ExecutionReports into rx_wire before enabling audit.
    std::vector<std::string> peer_reports;
    peer_reports.reserve(200);
    for (std::uint64_t i = 0; i < 200; ++i) {
        peer_reports.push_back(make_stream_frame<fix::ExecutionReport>([](auto& w) {
            w.order_id("O1").exec_id("E1").cl_ord_id("ORD-HOT").exec_type('0')
             .ord_status('0').symbol("AAPL").side('1')
             .leaves_qty(nf::Decimal{100, 0})
             .cum_qty(nf::Decimal{0, 0})
             .avg_px(nf::Decimal{0, 0});
        }, "SELL", "BUY", 2 + i, t1));
    }

    using OrderShape = nf::Fields<
        nf::Field<11, nf::FieldKind::string>,
        nf::Field<55, nf::FieldKind::string>,
        nf::Field<54, nf::FieldKind::character>,
        nf::Field<60, nf::FieldKind::string>,
        nf::Field<38, nf::FieldKind::decimal>,
        nf::Field<40, nf::FieldKind::character>>;
    fix::NewOrderSingleFixed<OrderShape, 128> order;
    CHECK(order.cl_ord_id("ORD-HOT").ok());
    CHECK(order.symbol("AAPL").ok());
    CHECK(order.side('1').ok());
    CHECK(order.transact_time(t1).ok());
    CHECK(order.order_qty(nf::Decimal{100, 0}).ok());
    CHECK(order.ord_type('1').ok());

    // Enable strict zero-malloc / zero-mutex / zero-fsync audit across 200 full round-trips!
    g_malloc_calls.store(0, std::memory_order_relaxed);
    g_mutex_calls.store(0, std::memory_order_relaxed);
    g_fsync_calls.store(0, std::memory_order_relaxed);
    g_audit_hot_path.store(true, std::memory_order_release);

    for (std::uint64_t i = 0; i < 200; ++i) {
        const auto seq = init.send(order, 10 + i, t1);
        CHECK(seq.ok() && seq.value() == 2 + i);
        init.transport().tx_wire.clear();
        init.transport().rx_wire.assign(peer_reports[i]);
        init.transport().rx_pos = 0;
        CHECK(init.poll_once(10 + i, t1).ok());
    }

    g_audit_hot_path.store(false, std::memory_order_release);
    CHECK(init.handler().msg_count == 200);
    CHECK(g_malloc_calls.load() == 0);
    CHECK(g_mutex_calls.load() == 0);
    CHECK(g_fsync_calls.load() == 0);
}

void test_restart_recovery_resend_replay_and_gapfill() {
    TempDir dir;
    const auto cfg = make_config(dir);

    // Session 1: Logon (seq 1), send 2 orders (seq 2, 3), receive 1 ExecReport (seq 2), then close.
    {
        auto opened = nf::DirectInitiator<fix::Schema, RecordingHandler, LoopbackTransport>::open(cfg);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto init = std::move(opened).value();
        CHECK(init.connect(0, t0).ok());
        std::string frame;
        CHECK(init.transport().pop_sent_frame(frame) && field(frame, 35) == "A");
        init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 1, t0));
        CHECK(init.poll_once(1, t0).ok());
        CHECK(init.is_active());

        using OrderShape = nf::Fields<
            nf::Field<11, nf::FieldKind::string>,
            nf::Field<55, nf::FieldKind::string>,
            nf::Field<54, nf::FieldKind::character>,
            nf::Field<60, nf::FieldKind::string>,
            nf::Field<38, nf::FieldKind::decimal>,
            nf::Field<40, nf::FieldKind::character>>;
        fix::NewOrderSingleFixed<OrderShape, 128> o1;
        CHECK(o1.cl_ord_id("ORD-1").ok()); CHECK(o1.symbol("AAPL").ok()); CHECK(o1.side('1').ok());
        CHECK(o1.transact_time(t1).ok()); CHECK(o1.order_qty(nf::Decimal{10, 0}).ok());
        CHECK(o1.ord_type('1').ok());
        CHECK(init.send(o1, 2, t1).value() == 2);

        o1.reset();
        CHECK(o1.cl_ord_id("ORD-2").ok()); CHECK(o1.symbol("MSFT").ok()); CHECK(o1.side('2').ok());
        CHECK(o1.transact_time(t1).ok()); CHECK(o1.order_qty(nf::Decimal{20, 0}).ok());
        CHECK(o1.ord_type('1').ok());
        CHECK(init.send(o1, 3, t1).value() == 3);

        init.transport().push_from_peer(make_stream_frame<fix::ExecutionReport>([](auto& w) {
            w.order_id("O1").exec_id("E1").cl_ord_id("ORD-1").exec_type('0')
             .ord_status('0').symbol("AAPL").side('1')
             .leaves_qty(nf::Decimal{10, 0}).cum_qty(nf::Decimal{0, 0})
             .avg_px(nf::Decimal{0, 0});
        }, "SELL", "BUY", 2, t1));
        CHECK(init.poll_once(4, t1).ok());
        CHECK(init.handler().msg_count == 1);
        CHECK(init.next_outbound_seq() == 4);
        CHECK(init.next_inbound_seq() == 3);
    }

    // Session 2 (restart from disk):
    // - Resumes at next_outbound_seq == 4, next_inbound_seq == 3.
    // - Peer sends high-sequence Logon (seq 5) -> DirectInitiator sends ResendRequest(7=3, 16=4)!
    // - Peer fills gap with seq 3 (ExecReport PossDup=Y) + seq 4 (GapFill 36=5) -> activates & delivers!
    // - Peer then sends ResendRequest(7=1, 16=3) -> DirectInitiator sends GapFill(34=1, 36=2) +
    //   replayed ORD-1 (34=2, 43=Y) + replayed ORD-2 (34=3, 43=Y)!
    {
        auto reopened = nf::DirectInitiator<fix::Schema, RecordingHandler, LoopbackTransport>::open(cfg);
        CHECK(reopened.ok());
        if (!reopened.ok()) return;
        auto init = std::move(reopened).value();
        CHECK(init.next_outbound_seq() == 4);
        CHECK(init.next_inbound_seq() == 3);

        CHECK(init.connect(10, t2).ok());
        std::string logon_out;
        CHECK(init.transport().pop_sent_frame(logon_out));
        CHECK(field(logon_out, 35) == "A" && field(logon_out, 34) == "4");

        // Peer sends high-sequence Logon (seq 5; we expected seq 3).
        init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 5, t2));
        CHECK(init.poll_once(11, t2).ok());
        CHECK(!init.is_active());
        std::string rr_out;
        CHECK(init.transport().pop_sent_frame(rr_out));
        CHECK(field(rr_out, 35) == "2" && field(rr_out, 7) == "3" && field(rr_out, 16) == "4");

        // Peer replays seq 3 (business ExecReport with PossDup=Y) and seq 4 (GapFill 123=Y, 36=5).
        init.transport().push_from_peer(make_stream_frame<fix::ExecutionReport>([](auto& w) {
            w.order_id("O2").exec_id("E-GAP").cl_ord_id("ORD-2").exec_type('0')
             .ord_status('0').symbol("MSFT").side('2')
             .leaves_qty(nf::Decimal{20, 0}).cum_qty(nf::Decimal{0, 0})
             .avg_px(nf::Decimal{0, 0});
        }, "SELL", "BUY", 3, t2, true, t1));
        init.transport().push_from_peer(make_frame(nf::message<"4">().set<123>('Y').set<36>(5), "SELL", "BUY", 4, t2, true, t1));
        CHECK(init.poll_once(12, t2).ok());
        CHECK(init.is_active());
        CHECK(init.next_inbound_seq() == 6);
        CHECK(init.handler().msg_count == 1);
        CHECK(init.handler().recovery_count == 1);
        CHECK(init.handler().last_exec_id == "E-GAP");

        // Now peer requests replay of our seqs 1..3 (1=Logon -> GapFill, 2=ORD-1, 3=ORD-2).
        init.transport().push_from_peer(make_frame(nf::message<"2">().set<7>(1).set<16>(3), "SELL", "BUY", 6, t3));
        CHECK(init.poll_once(13, t3).ok());

        std::string f1, f2, f3;
        CHECK(init.transport().pop_sent_frame(f1));
        CHECK(init.transport().pop_sent_frame(f2));
        CHECK(init.transport().pop_sent_frame(f3));
        CHECK(field(f1, 35) == "4" && field(f1, 34) == "1" && field(f1, 123) == "Y" && field(f1, 36) == "2");
        CHECK(field(f2, 35) == "D" && field(f2, 34) == "2" && field(f2, 43) == "Y" && field(f2, 11) == "ORD-1");
        CHECK(field(f3, 35) == "D" && field(f3, 34) == "3" && field(f3, 43) == "Y" && field(f3, 11) == "ORD-2");
    }
}

void test_sigkill_crash_recovery_redelivers_uncommitted_delivery() {
    TempDir dir;
    struct KillCtx { bool armed{}; };
    auto hook_fn = [](nf::JournalFaultPoint pt, void* ctx) noexcept {
        if (static_cast<KillCtx*>(ctx)->armed && pt == nf::JournalFaultPoint::postcommit) {
            (void)::kill(::getpid(), SIGKILL);
            ::_exit(137);
        }
    };

    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        KillCtx kctx{};
        auto cfg = make_config(dir);
        cfg.journal_test_hook = {hook_fn, &kctx};
        auto opened = nf::DirectInitiator<fix::Schema, RecordingHandler, LoopbackTransport>::open(cfg);
        if (!opened.ok()) ::_exit(10);
        auto init = std::move(opened).value();
        if (!init.connect(0, t0).ok()) ::_exit(11);
        init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 1, t0));
        if (!init.poll_once(1, t0).ok()) ::_exit(12);
        // Arm SIGKILL right after inbound seq 2 is committed (before delivery record!).
        kctx.armed = true;
        init.transport().push_from_peer(make_stream_frame<fix::ExecutionReport>([](auto& w) {
            w.order_id("O-CRASH").exec_id("E-CRASH").cl_ord_id("ORD-C").exec_type('0')
             .ord_status('0').symbol("NVDA").side('1')
             .leaves_qty(nf::Decimal{50, 0}).cum_qty(nf::Decimal{0, 0})
             .avg_px(nf::Decimal{0, 0});
        }, "SELL", "BUY", 2, t1));
        (void)init.poll_once(2, t1);
        ::_exit(13);
    }
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

    // Reopen after SIGKILL: seq 2 is persisted without delivery, so upon reconnecting
    // DirectInitiator automatically redelivers E-CRASH with is_recovery = true!
    auto reopened = nf::DirectInitiator<fix::Schema, RecordingHandler, LoopbackTransport>::open(
        make_config(dir));
    CHECK(reopened.ok());
    if (!reopened.ok()) return;
    auto init = std::move(reopened).value();
    CHECK(init.next_inbound_seq() == 3);
    CHECK(init.connect(10, t2).ok());
    init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 3, t2));
    CHECK(init.poll_once(11, t2).ok());
    CHECK(init.is_active());
    CHECK(init.handler().msg_count == 1);
    CHECK(init.handler().recovery_count == 1);
    CHECK(init.handler().last_exec_id == "E-CRASH");
}

void test_default_session_key_auto_derivation() {
    TempDir dir;
    nf::DirectInitiatorConfig cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = 9876;
    cfg.storage_directory = dir.path;
    cfg.local_comp_id = "BUY";
    cfg.remote_comp_id = "SELL";
    // session_key intentionally omitted: should auto-derive as FIX.4.4/BUY/SELL.
    auto opened = nf::DirectInitiator<fix::Schema>::open(cfg);
    CHECK(opened.ok());
}

void test_named_fixed_enqueue_batch_and_spsc_queue() {
    TempDir dir;
    nf::DirectInitiatorConfig cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = 9876;
    cfg.storage_directory = dir.path;
    cfg.local_comp_id = "BUY";
    cfg.remote_comp_id = "SELL";

    struct TypedHandler {
        std::uint64_t er_count{};
        std::string last_exec_id;
        [[nodiscard]] nf::Status on_message(const nf::FlatMessageView& msg, const nf::DeliveryInfo&) noexcept {
            if (msg.is<fix::ExecutionReport>()) {
                const auto er = msg.as<fix::ExecutionReport>();
                ++er_count;
                if (const auto id = er.exec_id(); id.ok()) last_exec_id.assign(id.value());
            }
            return nf::Status::success();
        }
    };

    auto opened = nf::DirectInitiator<fix::Schema, TypedHandler, LoopbackTransport>::open(cfg);
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto init = std::move(opened).value();
    init.transport().tx_wire.reserve(256U * 1024U);
    CHECK(init.connect(1, t0).ok());

    std::string wire;
    CHECK(init.transport().pop_sent_frame(wire));
    CHECK(field(wire, 35) == "A");
    init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 1, t0));
    CHECK(init.poll_once(2, t0).ok());
    CHECK(init.is_active());

    // 1. Named zero-alloc Fixed<ByteCapacity, fix::field::...> + enqueue() + write_batch coalescing.
    using FastOrder = fix::NewOrderSingle::Fixed<128,
        fix::field::ClOrdID,
        fix::field::Symbol,
        fix::field::Side,
        fix::field::TransactTime,
        fix::field::OrderQty,
        fix::field::OrdType>;

    FastOrder order;
    g_malloc_calls.store(0);
    g_mutex_calls.store(0);
    g_fsync_calls.store(0);
    g_audit_hot_path.store(true);
    for (int i = 0; i < 10; ++i) {
        order.reset();
        char cid[16] = "BATCH-00";
        cid[6] = static_cast<char>('0' + (i / 10));
        cid[7] = static_cast<char>('0' + (i % 10));
        CHECK(order.cl_ord_id(std::string_view{cid, 8}).ok());
        CHECK(order.symbol("AAPL").ok());
        CHECK(order.side('1').ok());
        CHECK(order.transact_time(t1).ok());
        CHECK(order.order_qty(nf::Decimal{100, 0}).ok());
        CHECK(order.ord_type('1').ok());
        const auto seq = init.enqueue(order, 10, t1);
        CHECK(seq.ok() && seq.value() == static_cast<std::uint64_t>(2 + i));
    }
    CHECK(init.transport().tx_wire.empty()); // Enqueued only; no socket write yet.
    CHECK(init.flush(10).ok());
    g_audit_hot_path.store(false);

    CHECK(init.transport().write_batch_calls == 1);
    CHECK(init.transport().write_batch_chunks == 10);
    CHECK(g_malloc_calls.load() == 0);
    CHECK(g_mutex_calls.load() == 0);
    CHECK(g_fsync_calls.load() == 0);
    for (int i = 0; i < 10; ++i) {
        CHECK(init.transport().pop_sent_frame(wire));
        CHECK(field(wire, 35) == "D");
        CHECK(field(wire, 34) == std::to_string(2 + i));
    }

    // 2. Verify msg.is<ExecutionReport>() / msg.as<ExecutionReport>() typed handler dispatch.
    init.transport().push_from_peer(make_stream_frame<fix::ExecutionReport>([](auto& w) {
        w.order_id("O1").exec_id("EX-TYPED-1").cl_ord_id("BATCH-00")
         .exec_type('0').ord_status('0').symbol("AAPL").side('1')
         .leaves_qty(nf::Decimal{100, 0}).cum_qty(nf::Decimal{0, 0}).avg_px(nf::Decimal{0, 0});
    }, "SELL", "BUY", 2, t1));
    CHECK(init.poll_once(11, t1).ok());
    CHECK(init.handler().er_count == 1);
    CHECK(init.handler().last_exec_id == "EX-TYPED-1");

    // 3. 2-thread lock-free SPSC DirectSendQueue: producer thread pushes 100 orders, I/O thread drains.
    nf::DirectSendQueue queue{64, 512};
    constexpr int total_queued = 100;
    std::atomic<bool> producer_done{false};
    std::thread producer([&] {
        FastOrder local_order;
        for (int i = 0; i < total_queued; ++i) {
            local_order.reset();
            char cid[16] = "SPSC-000";
            cid[5] = static_cast<char>('0' + (i / 100));
            cid[6] = static_cast<char>('0' + ((i / 10) % 10));
            cid[7] = static_cast<char>('0' + (i % 10));
            (void)local_order.cl_ord_id(std::string_view{cid, 8});
            (void)local_order.symbol("MSFT");
            (void)local_order.side('2');
            (void)local_order.transact_time(t2);
            (void)local_order.order_qty(nf::Decimal{50, 0});
            (void)local_order.ord_type('1');
            while (true) {
                const auto st = queue.try_push(local_order);
                if (st.ok()) break;
                if (st.code == nf::ErrorCode::queue_full) {
                    std::this_thread::yield();
                    continue;
                }
                CHECK(st.ok());
                break;
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::size_t drained_total = 0;
    while (drained_total < static_cast<std::size_t>(total_queued)) {
        const auto d = init.drain(queue, 32, 20, t2);
        CHECK(d.ok());
        if (d.ok()) drained_total += d.value();
        if (d.ok() && d.value() == 0) std::this_thread::yield();
    }
    producer.join();
    CHECK(producer_done.load());
    CHECK(queue.empty());

    for (int i = 0; i < total_queued; ++i) {
        CHECK(init.transport().pop_sent_frame(wire));
        CHECK(field(wire, 35) == "D");
        CHECK(field(wire, 34) == std::to_string(12 + i));
        CHECK(field(wire, 55) == "MSFT");
    }

    // 4. Zero-intermediate-copy StreamWriter (send_stream / enqueue_stream / try_push_stream)
    //    with hard 0-malloc / 0-mutex / 0-fsync audit, std::optional, dynamic w.header(), and >320B fields!
    const std::string long_id(320, 'X');
    g_malloc_calls.store(0);
    g_mutex_calls.store(0);
    g_fsync_calls.store(0);
    g_audit_hot_path.store(true);
    {
        const std::optional<nf::Decimal> no_price = std::nullopt;
        const std::optional<nf::Decimal> limit_price = nf::Decimal{15025, 2};
        CHECK(init.send_stream<fix::NewOrderSingle>([&](auto& w) {
            w.cl_ord_id("OPT-MKT").symbol("AAPL").side('1')
             .transact_time(t2).order_qty(nf::Decimal{10, 0}).ord_type('1')
             .price(no_price);
        }, 30, t2).ok());
        CHECK(init.send_stream<fix::NewOrderSingle>([&](auto& w) {
            w.header().sender_sub_id("DESK-A").target_sub_id("EXCH-1");
            w.cl_ord_id("OPT-LMT").symbol("AAPL").side('2')
             .transact_time(t2).order_qty(nf::Decimal{20, 0}).ord_type('2')
             .price(limit_price);
        }, 30, t2).ok());

        for (int k = 0; k < 2; ++k) {
            CHECK(init.enqueue_stream<fix::NewOrderSingle>([&](auto& w) {
                w.cl_ord_id(k == 0 ? "STR-MKT" : "STR-LMT")
                 .symbol("NVDA")
                 .side('1')
                 .transact_time(t2)
                 .order_qty(nf::Decimal{30, 0})
                 .ord_type(k == 1 ? '2' : '1');
                if (k == 1) w.price(nf::Decimal{9950, 2});
            }, 31, t2).ok());
        }
        CHECK(init.flush(31).ok());

        // Large >320B string streams straight into mmap Journal with 0 mallocs!
        CHECK(init.send_stream<fix::NewOrderSingle>([&](auto& w) {
            w.cl_ord_id(long_id).symbol("AAPL").side('1')
             .transact_time(t2).order_qty(nf::Decimal{1, 0}).ord_type('1');
        }, 32, t2).ok());

        // DirectSendQueue::try_push_stream + drain: 0 mallocs!
        CHECK(queue.try_push_stream<fix::NewOrderSingle>([&](auto& w) {
            w.cl_ord_id("Q-STREAM").symbol("MSFT").side('1')
             .transact_time(t2).order_qty(nf::Decimal{5, 0}).ord_type('1');
        }).ok());
        const auto drained_one = init.drain(queue, 8, 33, t2);
        CHECK(drained_one.ok() && drained_one.value() == 1);
    }
    g_audit_hot_path.store(false);
    CHECK(g_malloc_calls.load() == 0);
    CHECK(g_mutex_calls.load() == 0);
    CHECK(g_fsync_calls.load() == 0);

    // Verify optional absent/present, dynamic header (50/57), and large field on the wire.
    CHECK(init.transport().pop_sent_frame(wire));
    CHECK(field(wire, 11) == "OPT-MKT" && field(wire, 44).empty());
    CHECK(init.transport().pop_sent_frame(wire));
    CHECK(field(wire, 11) == "OPT-LMT" && field(wire, 44) == "150.25" &&
          field(wire, 50) == "DESK-A" && field(wire, 57) == "EXCH-1");
    CHECK(init.transport().pop_sent_frame(wire));
    CHECK(field(wire, 11) == "STR-MKT" && field(wire, 44).empty());
    CHECK(init.transport().pop_sent_frame(wire));
    CHECK(field(wire, 11) == "STR-LMT" && field(wire, 44) == "99.50");
    CHECK(init.transport().pop_sent_frame(wire));
    CHECK(field(wire, 11) == long_id);
    CHECK(init.transport().pop_sent_frame(wire));
    CHECK(field(wire, 11) == "Q-STREAM" && field(wire, 55) == "MSFT");
}

void test_subms_timestamps_and_optional_checksum_skip() {
    // 1. FastUtcClock millisecond, microsecond, nanosecond precision formats
    nf::detail::FastUtcClock clk{};
    const auto ts_ms = clk.wall_time(nf::TimestampPrecision::milliseconds);
    CHECK(ts_ms.size() == 21 && nf::detail::is_valid_utc_timestamp(ts_ms));
    const auto ts_us = clk.wall_time(nf::TimestampPrecision::microseconds);
    CHECK(ts_us.size() == 24 && nf::detail::is_valid_utc_timestamp(ts_us));
    const auto ts_ns = clk.wall_time(nf::TimestampPrecision::nanoseconds);
    CHECK(ts_ns.size() == 27 && nf::detail::is_valid_utc_timestamp(ts_ns));

    // 2. DirectInitiator configured with nanosecond timestamps and verify_checksum = false
    TempDir dir;
    nf::DirectInitiatorConfig cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = 9999;
    cfg.storage_directory = dir.path;
    cfg.local_comp_id = "BUY";
    cfg.remote_comp_id = "SELL";
    cfg.heartbeat_ms = 30'000;
    cfg.timestamp_precision = nf::TimestampPrecision::nanoseconds;
    cfg.verify_checksum = false;

    auto opened = nf::DirectInitiator<fix::Schema, nf::NullInitiatorHandler, LoopbackTransport>::open(cfg);
    CHECK(opened.ok());
    auto init = std::move(opened).value();
    CHECK(init.connect().ok());

    std::string logon_wire;
    CHECK(init.transport().pop_sent_frame(logon_wire));
    const auto logon_ts = field(logon_wire, 52);
    CHECK(logon_ts.size() == 27 && nf::detail::is_valid_utc_timestamp(logon_ts));

    // Feed peer Logon with intentionally wrong (but syntactically valid 000..255) checksum
    std::string peer_logon = make_frame(nf::message<"A">().set<98>(0).set<108>(30),
                                        "SELL", "BUY", 1, init.now_wall_time());
    const std::size_t cpos = peer_logon.size() - 4;
    peer_logon[cpos] = '0';
    peer_logon[cpos + 1] = '0';
    peer_logon[cpos + 2] = (peer_logon[cpos + 2] == '0') ? '1' : '0';
    init.transport().push_from_peer(peer_logon);
    CHECK(init.poll_once().ok());
    CHECK(init.is_active());

    // Send a NewOrderSingle using the default clock (27-byte nanosecond 52= and 60=)
    const auto wt = init.now_wall_time();
    CHECK(wt.size() == 27);
    const auto sent = init.send_stream<fix::NewOrderSingle>([&](auto& w) {
        w.cl_ord_id("NS-ORD-1").symbol("AAPL").side('1')
         .transact_time(wt).order_qty(nf::Decimal{10, 0}).ord_type('1');
    });
    CHECK(sent.ok());
    std::string ord_wire;
    CHECK(init.transport().pop_sent_frame(ord_wire));
    CHECK(field(ord_wire, 52).size() == 27);
    CHECK(field(ord_wire, 60).size() == 27);
}

// Transport that simulates selective endpoint reachability for multi-endpoint failover tests.
struct FailoverMockTransport {
    std::vector<std::uint16_t> allowed_ports;
    std::vector<std::uint16_t> attempted_ports;
    LoopbackTransport inner;

    [[nodiscard]] nf::Status connect(std::string_view host, std::uint16_t port) noexcept {
        attempted_ports.push_back(port);
        for (const auto p : allowed_ports) {
            if (p == port) return inner.connect(host, port);
        }
        return nf::Status{nf::ErrorCode::io_error};
    }
    [[nodiscard]] nf::Status finish_connect() noexcept { return inner.finish_connect(); }
    [[nodiscard]] nf::TransportIoResult read(std::span<char> out) noexcept { return inner.read(out); }
    [[nodiscard]] nf::TransportIoResult write(std::span<const char> in) noexcept { return inner.write(in); }
    [[nodiscard]] nf::TransportIoResult write_batch(std::span<const std::span<const char>> chunks) noexcept {
        return inner.write_batch(chunks);
    }
    void close() noexcept { inner.close(); }
    [[nodiscard]] int native_fd() const noexcept { return inner.native_fd(); }
    [[nodiscard]] bool valid() const noexcept { return inner.valid(); }
    [[nodiscard]] bool is_connecting() const noexcept { return inner.is_connecting(); }
};
static_assert(nf::DirectTransport<FailoverMockTransport>);

void test_multi_endpoint_failover_and_backoff() {
    // 1. FailoverController exponential backoff, ordered reset-to-primary, round_robin, and max_retries
    {
        nf::detail::FailoverController fc;
        fc.configure({{"127.0.0.1", 9001}, {"127.0.0.1", 9002}},
                     nf::ConnectionStrategy::ordered(100, 500, 2, 0, true, true), 250);
        CHECK(fc.current_index() == 0);
        // Primary fails -> immediate 0 ms failover to standby (index 1)
        CHECK(fc.on_failure_or_disconnect(1000, false) == 0);
        CHECK(fc.current_index() == 1 && fc.next_connect_at_ms() == 1000);
        // Standby fails -> wrap back to primary (index 0) with 100 ms backoff
        CHECK(fc.on_failure_or_disconnect(1000, false) == 100);
        CHECK(fc.current_index() == 0 && fc.next_connect_at_ms() == 1100);
        // Second cycle: primary fails (0 ms -> idx 1), standby fails (200 ms -> idx 0)
        CHECK(fc.on_failure_or_disconnect(1100, false) == 0);
        CHECK(fc.current_index() == 1);
        CHECK(fc.on_failure_or_disconnect(1100, false) == 200);
        CHECK(fc.current_index() == 0 && fc.next_connect_at_ms() == 1300);
        // Third cycle: cap at 500 ms (400 ms then 500 ms)
        CHECK(fc.on_failure_or_disconnect(1300, false) == 0);
        CHECK(fc.on_failure_or_disconnect(1300, false) == 400);
        CHECK(fc.on_failure_or_disconnect(1700, false) == 0);
        CHECK(fc.on_failure_or_disconnect(1700, false) == 500);

        // Succeed on standby (index 1), then disconnect -> reset_to_primary returns to index 0 with base backoff
        CHECK(fc.on_failure_or_disconnect(2200, false) == 0); // now at index 1
        fc.on_session_active();
        CHECK(fc.consecutive_failures() == 0);
        CHECK(fc.on_failure_or_disconnect(3000, true) == 100);
        CHECK(fc.current_index() == 0);
    }

    // Round-robin cycling + max_retries exhaustion
    {
        nf::detail::FailoverController fc;
        fc.configure({{"127.0.0.1", 9001}, {"127.0.0.1", 9002}, {"127.0.0.1", 9003}},
                     nf::ConnectionStrategy::round_robin(50, 500, 2, 3, true), 250);
        fc.on_session_active();
        // Disconnect from active on index 0 -> advances to index 1 with base backoff
        CHECK(fc.on_failure_or_disconnect(100, true) == 50);
        CHECK(fc.current_index() == 1);
        // 3 consecutive failures -> exhausted on 3rd
        CHECK(fc.on_failure_or_disconnect(150, false) == 0);
        CHECK(fc.current_index() == 2 && !fc.exhausted());
        CHECK(fc.on_failure_or_disconnect(150, false) == 0);
        CHECK(fc.current_index() == 0 && !fc.exhausted());
        (void)fc.on_failure_or_disconnect(150, false);
        CHECK(fc.exhausted());
    }

    // 2. DirectInitiator end-to-end ordered failover: primary (9001) down -> immediate switch to standby (9002)
    TempDir dir;
    nf::DirectInitiatorConfig cfg{};
    cfg.storage_directory = dir.path;
    cfg.local_comp_id = "BUY";
    cfg.remote_comp_id = "SELL";
    cfg.heartbeat_ms = 30'000;
    cfg.endpoints = {{"127.0.0.1", 9001}, {"127.0.0.1", 9002}};
    cfg.connection_strategy = nf::ConnectionStrategy::ordered(200, 2000, 2, 0, true, true);

    FailoverMockTransport mock{};
    mock.allowed_ports = {9002};
    auto opened = nf::DirectInitiator<fix::Schema, nf::NullInitiatorHandler, FailoverMockTransport>::open(
        cfg, nf::NullInitiatorHandler{}, std::move(mock));
    CHECK(opened.ok());
    auto init = std::move(opened).value();

    // connect() tries 9001 (fails) and immediately fails over to 9002 (succeeds)!
    CHECK(init.connect(100, t0).ok());
    CHECK(init.valid_transport());
    CHECK(init.active_endpoint_index() == 1);
    CHECK(init.active_endpoint().port == 9002);
    CHECK(init.transport().attempted_ports.size() == 2);
    CHECK(init.transport().attempted_ports[0] == 9001 && init.transport().attempted_ports[1] == 9002);

    // Complete Logon on standby 9002
    std::string logon_wire;
    CHECK(init.transport().inner.pop_sent_frame(logon_wire));
    init.transport().inner.push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30),
                                                     "SELL", "BUY", 1, t0));
    CHECK(init.poll_once(101, t0).ok());
    CHECK(init.is_active());

    // Now primary 9001 recovers, standby 9002 drops -> ordered reset_to_primary switches back to 9001 after backoff!
    init.transport().allowed_ports = {9001};
    init.disconnect();
    CHECK(!init.valid_transport());
    CHECK(init.active_endpoint_index() == 0);
    // Before backoff elapses, reconnect_if_due returns need_more
    const auto due_at = init.next_reconnect_at_ms();
    CHECK(init.reconnect_if_due(due_at - 1, t1).code == nf::ErrorCode::need_more);
    // Once backoff elapses, reconnect_if_due reconnects to primary 9001!
    CHECK(init.reconnect_if_due(due_at, t1).ok());
    CHECK(init.valid_transport());
    CHECK(init.active_endpoint_index() == 0);
    CHECK(init.active_endpoint().port == 9001);
}

void test_zero_overhead_mmap_telemetry_and_stage_histograms() {
    // 1. Histogram bucket accuracy and quantile monotonicity
    nf::TelemetryHistogram hist{};
    for (std::uint64_t ns = 50; ns <= 10'000; ns += 50) {
        hist.record(ns);
    }
    const auto sum = hist.summarize();
    CHECK(sum.count == 200);
    CHECK(sum.min_ns == 50);
    CHECK(sum.max_ns == 10'000);
    CHECK(sum.p50_ns >= 4'800 && sum.p50_ns <= 5'500);
    CHECK(sum.p90_ns >= 8'800 && sum.p90_ns <= 9'600);
    CHECK(sum.p99_ns >= 9'800 && sum.p99_ns <= 10'000);
    CHECK(sum.p50_ns <= sum.p90_ns && sum.p90_ns <= sum.p99_ns &&
          sum.p99_ns <= sum.p999_ns && sum.p999_ns <= sum.p9999_ns &&
          sum.p9999_ns <= sum.max_ns);

    // 2. DirectInitiator with mmap telemetry + all 5 stage probes under 0-malloc / 0-mutex / 0-fsync audit
    TempDir dir;
    const std::filesystem::path tel_path = std::filesystem::path{dir.path} / "direct.nfxt";
    nf::DirectInitiatorConfig cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = 9876;
    cfg.storage_directory = dir.path;
    cfg.local_comp_id = "BUY";
    cfg.remote_comp_id = "SELL";
    cfg.telemetry_path = tel_path;
    cfg.telemetry_stage_probes = true;

    struct CountHandler {
        int delivered{};
        [[nodiscard]] nf::Status on_message(const nf::FlatMessageView&, const nf::DeliveryInfo&) noexcept {
            ++delivered;
            return nf::Status::success();
        }
    };

    auto opened = nf::DirectInitiator<fix::Schema, CountHandler, LoopbackTransport>::open(cfg);
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto init = std::move(opened).value();
    init.transport().tx_wire.reserve(256U * 1024U);
    init.transport().rx_wire.reserve(256U * 1024U);

    CHECK(init.connect(1, t0).ok());
    std::string wire;
    CHECK(init.transport().pop_sent_frame(wire));
    init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 1, t0));
    CHECK(init.poll_once(2, t0).ok());
    CHECK(init.is_active());

    // Pre-build 20 inbound ExecutionReport frames to feed during audited hot path
    std::vector<std::string> ers;
    ers.reserve(20);
    for (int i = 0; i < 20; ++i) {
        auto er = nf::message<"8">()
            .set<37>("ORD-" + std::to_string(i))
            .set<17>("EX-" + std::to_string(i))
            .set<11>("TEL-" + std::to_string(i))
            .set<150>('0').set<39>('0')
            .set<55>("AAPL").set<54>('1')
            .set<151>(nf::Decimal{100, 0})
            .set<14>(nf::Decimal{0, 0})
            .set<6>(nf::Decimal{0, 0});
        ers.push_back(make_frame(er, "SELL", "BUY", static_cast<std::uint64_t>(2 + i), t1));
    }

    g_malloc_calls.store(0);
    g_mutex_calls.store(0);
    g_fsync_calls.store(0);
    g_audit_hot_path.store(true);
    for (int i = 0; i < 20; ++i) {
        char cid[16] = "TEL-00";
        cid[4] = static_cast<char>('0' + (i / 10));
        cid[5] = static_cast<char>('0' + (i % 10));
        const auto seq = init.send_stream<fix::NewOrderSingle>([&](auto& w) {
            w.cl_ord_id(std::string_view{cid, 6})
             .symbol("AAPL").side('1').transact_time(t1)
             .order_qty(nf::Decimal{100, 0}).ord_type('1');
        }, 10, t1);
        CHECK(seq.ok());
        init.transport().rx_wire.append(ers[static_cast<std::size_t>(i)]);
        CHECK(init.poll_once(10, t1).ok());
    }
    g_audit_hot_path.store(false);

    CHECK(g_malloc_calls.load() == 0);
    CHECK(g_mutex_calls.load() == 0);
    CHECK(g_fsync_calls.load() == 0);
    CHECK(init.handler().delivered == 20);

    // 3. Out-of-band read-only sidecar inspection via TelemetryReader
    auto reader_res = nf::TelemetryReader::open(tel_path);
    CHECK(reader_res.ok());
    if (!reader_res.ok()) return;
    const auto& reader = reader_res.value();
    CHECK(reader.writer_pid() > 0);
    const auto snaps = reader.read_all();
    CHECK(snaps.size() == 1);
    if (snaps.size() == 1) {
        const auto& s = snaps[0];
        CHECK(s.local_comp_id == "BUY" && s.remote_comp_id == "SELL");
        CHECK(s.state == nf::SessionState::active);
        CHECK(s.inbound_frames == 21);   // Logon + 20 ERs
        CHECK(s.outbound_frames == 21);  // Logon + 20 NOS
        CHECK(s.business_deliveries == 20);
        CHECK(s.error_count == 0);
        CHECK(s.stages[static_cast<std::size_t>(nf::TelemetryStage::read)].count >= 21);
        CHECK(s.stages[static_cast<std::size_t>(nf::TelemetryStage::parse)].count >= 21);
        CHECK(s.stages[static_cast<std::size_t>(nf::TelemetryStage::validate)].count == 20);
        CHECK(s.stages[static_cast<std::size_t>(nf::TelemetryStage::journal)].count >= 40);
        CHECK(s.stages[static_cast<std::size_t>(nf::TelemetryStage::write)].count >= 21);
    }
    const auto prom = reader.format_prometheus();
    CHECK(prom.find("nimblefix_session_inbound_frames_total") != std::string::npos);
    CHECK(prom.find("stage=\"validate\",quantile=\"0.99\"") != std::string::npos);
}

// Regression test for R02: DirectInitiator recovery reuses previously committed
// dependent business replies instead of duplicating orders with new MsgSeqNums.
void test_recovery_reuses_dependent_business_reply() {
    TempDir dir;
    const auto cfg = make_config(dir);

    struct ReplyOnExecHandler {
        nf::DirectInitiator<fix::Schema, ReplyOnExecHandler, LoopbackTransport>* init{};
        bool fail_after_send{false};
        bool send_different_reply{false};
        bool omit_reply{false};
        int calls{0};
        int recovery_calls{0};
        nf::Result<std::uint64_t> last_send{nf::Status::success()};

        [[nodiscard]] nf::Status on_message(const nf::FlatMessageView& msg,
                                            const nf::DeliveryInfo& info) noexcept {
            ++calls;
            if (info.is_recovery) ++recovery_calls;
            if (!msg.is<fix::ExecutionReport>() || init == nullptr) return nf::Status::success();
            if (omit_reply) return nf::Status::success();
            const std::string_view cid = send_different_reply ? "WRONG-REPLY" : "DEP-REPLY-1";
            last_send = init->send_stream<fix::NewOrderSingle>([cid](auto& w) noexcept {
                w.cl_ord_id(cid).symbol("AAPL").side('1')
                 .transact_time(t1).order_qty(nf::Decimal{25, 0}).ord_type('1');
            }, 10, t1);
            if (!last_send.ok()) return last_send.status();
            return fail_after_send ? nf::Status{nf::ErrorCode::callback_failed} : nf::Status::success();
        }
    };

    std::uint64_t orig_reply_seq = 0;
    nf::RecordId inbound_er_id = 0;

    // 1. First session: receive ER seq 2, handler commits dependent reply (seq 2) then crashes/fails
    //    BEFORE delivery record is written.
    {
        auto opened = nf::DirectInitiator<fix::Schema, ReplyOnExecHandler, LoopbackTransport>::open(cfg);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto init = std::move(opened).value();
        init.handler().init = &init;
        init.handler().fail_after_send = true;

        CHECK(init.connect(0, t0).ok());
        std::string wire;
        CHECK(init.transport().pop_sent_frame(wire));
        init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 1, t0));
        CHECK(init.poll_once(1, t0).ok());
        CHECK(init.is_active());

        init.transport().push_from_peer(make_stream_frame<fix::ExecutionReport>([](auto& w) {
            w.order_id("O1").exec_id("E1").cl_ord_id("TRIG-1").exec_type('0')
             .ord_status('0').symbol("AAPL").side('1')
             .leaves_qty(nf::Decimal{10, 0}).cum_qty(nf::Decimal{0, 0})
             .avg_px(nf::Decimal{0, 0});
        }, "SELL", "BUY", 2, t1));
        const auto st = init.poll_once(2, t1);
        CHECK(st.code == nf::ErrorCode::callback_failed);
        CHECK(init.handler().calls == 1);
        CHECK(init.handler().last_send.ok());
        orig_reply_seq = init.handler().last_send.value();
        CHECK(orig_reply_seq == 2);

        // Verify the reply record carries non-zero dependency pointing to inbound ER!
        int dep_replies = 0;
        for (const auto& rec : init.journal().records()) {
            if (rec.kind == nf::RecordKind::inbound && rec.seq == 2) inbound_er_id = rec.id;
            if (rec.kind == nf::RecordKind::outbound && rec.receipt != 0 && rec.seq == orig_reply_seq) {
                CHECK(rec.dependency != 0 && rec.dependency == inbound_er_id);
                ++dep_replies;
            }
        }
        CHECK(inbound_er_id != 0 && dep_replies == 1);
    }

    // 2. Reopen and verify mismatching recovery reply is rejected with invalid_value!
    {
        auto opened = nf::DirectInitiator<fix::Schema, ReplyOnExecHandler, LoopbackTransport>::open(cfg);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto init = std::move(opened).value();
        init.handler().init = &init;
        init.handler().send_different_reply = true;

        CHECK(init.connect(10, t2).ok());
        std::string wire;
        CHECK(init.transport().pop_sent_frame(wire));
        init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 3, t2));
        const auto st = init.poll_once(11, t2);
        CHECK(!st.ok() && st.code == nf::ErrorCode::callback_failed);
        CHECK(!init.handler().last_send.ok() && init.handler().last_send.status().code == nf::ErrorCode::invalid_value);
        CHECK(init.handler().recovery_calls == 1);
    }

    // 3. Reopen and verify omitting previously committed reply during recovery is rejected!
    {
        auto opened = nf::DirectInitiator<fix::Schema, ReplyOnExecHandler, LoopbackTransport>::open(cfg);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto init = std::move(opened).value();
        init.handler().init = &init;
        init.handler().omit_reply = true;

        CHECK(init.connect(20, t2).ok());
        std::string wire;
        CHECK(init.transport().pop_sent_frame(wire));
        init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 4, t2));
        const auto st = init.poll_once(21, t2);
        CHECK(!st.ok() && st.code == nf::ErrorCode::callback_failed);
        CHECK(init.handler().recovery_calls == 1);
    }

    // 4. Reopen with matching reply -> reuses original seq 2, does NOT append a duplicate order!
    {
        auto opened = nf::DirectInitiator<fix::Schema, ReplyOnExecHandler, LoopbackTransport>::open(cfg);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto init = std::move(opened).value();
        init.handler().init = &init;

        const auto out_seq_before = init.next_outbound_seq();
        CHECK(init.connect(30, t3).ok()); // Logon consumes out_seq_before
        std::string wire;
        CHECK(init.transport().pop_sent_frame(wire));
        init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 5, t3));
        CHECK(init.poll_once(31, t3).ok());
        CHECK(init.is_active());
        CHECK(init.handler().recovery_calls == 1);
        CHECK(init.handler().last_send.ok() && init.handler().last_send.value() == orig_reply_seq);
        // Outbound seq only advanced by 1 for Logon, NOT for the reused business reply!
        CHECK(init.next_outbound_seq() == out_seq_before + 1);

        int business_outbounds = 0;
        for (const auto& rec : init.journal().records()) {
            if (rec.kind == nf::RecordKind::outbound && rec.receipt != 0) ++business_outbounds;
        }
        CHECK(business_outbounds == 1);
    }
}

// Controllable mock transport for R03 connect, TLS-handshake, and write-stall timeout tests.
struct TimeoutMockTransport {
    LoopbackTransport inner;
    std::vector<std::uint16_t> attempted_ports;
    std::uint16_t stall_connect_port{0};
    bool report_handshaking{false};
    bool block_writes{false};
    bool connecting_state{false};

    [[nodiscard]] nf::Status connect(std::string_view host, std::uint16_t port) noexcept {
        attempted_ports.push_back(port);
        if (port == stall_connect_port) {
            inner.open_state = true;
            connecting_state = true;
            return nf::Status::success();
        }
        connecting_state = false;
        return inner.connect(host, port);
    }
    [[nodiscard]] nf::Status finish_connect() noexcept {
        if (connecting_state) return nf::Status{nf::ErrorCode::need_more};
        return nf::Status::success();
    }
    [[nodiscard]] nf::TransportIoResult read(std::span<char> out) noexcept { return inner.read(out); }
    [[nodiscard]] nf::TransportIoResult write(std::span<const char> in) noexcept {
        if (block_writes) return {0, true, false, nf::Status::success()};
        return inner.write(in);
    }
    [[nodiscard]] nf::TransportIoResult write_batch(std::span<const std::span<const char>> chunks) noexcept {
        if (block_writes) return {0, true, false, nf::Status::success()};
        return inner.write_batch(chunks);
    }
    void close() noexcept {
        connecting_state = false;
        inner.close();
    }
    [[nodiscard]] int native_fd() const noexcept { return inner.native_fd(); }
    [[nodiscard]] bool valid() const noexcept { return inner.valid(); }
    [[nodiscard]] bool is_connecting() const noexcept { return connecting_state; }
    [[nodiscard]] bool is_handshaking() const noexcept { return connecting_state && report_handshaking; }
};
static_assert(nf::DirectTransport<TimeoutMockTransport>);

// Regression test for R03: connect timeout, TLS handshake timeout, and write-stall timeout.
void test_connect_tls_and_write_stall_timeouts() {
    struct ErrorRecorder {
        nf::ErrorCode last_code{nf::Status::success().code};
        int error_count{0};
        [[nodiscard]] nf::Status on_message(const nf::FlatMessageView&, const nf::DeliveryInfo&) noexcept {
            return nf::Status::success();
        }
        void on_error(const nf::Status& st) noexcept {
            last_code = st.code;
            ++error_count;
        }
    };

    // 1. Connect timeout triggers io_error + failover to standby endpoint!
    {
        TempDir dir;
        auto cfg = make_config(dir);
        cfg.endpoints = {{"127.0.0.1", 9101}, {"127.0.0.1", 9102}};
        cfg.connection_strategy = nf::ConnectionStrategy::ordered(100, 1000, 2, 0, true, true);
        cfg.connect_timeout_ms = 500;

        TimeoutMockTransport mock{};
        mock.stall_connect_port = 9101;
        auto opened = nf::DirectInitiator<fix::Schema, ErrorRecorder, TimeoutMockTransport>::open(
            cfg, ErrorRecorder{}, std::move(mock));
        CHECK(opened.ok());
        auto init = std::move(opened).value();

        CHECK(init.connect(1000, t0).ok());
        CHECK(init.valid_transport() && init.transport().is_connecting());
        CHECK(init.next_deadline_ms() == 1500);
        // Before deadline: stays connecting
        CHECK(init.poll_once(1499, t0).ok());
        CHECK(init.valid_transport() && init.handler().error_count == 0);
        // At deadline: times out, calls on_error, closes transport, advances failover to 9102!
        const auto st = init.poll_once(1500, t0);
        CHECK(st.code == nf::ErrorCode::io_error);
        CHECK(!init.valid_transport());
        CHECK(init.handler().last_code == nf::ErrorCode::io_error);
        CHECK(init.active_endpoint().port == 9102);
        CHECK(init.reconnect_if_due(1500, t0).ok());
        CHECK(init.valid_transport() && !init.transport().is_connecting());
    }

    // 2. TLS handshake timeout reports ErrorCode::tls_handshake_timeout!
    {
        TempDir dir;
        auto cfg = make_config(dir);
        cfg.endpoints = {{"127.0.0.1", 9201}};
        cfg.connect_timeout_ms = 300;

        TimeoutMockTransport mock{};
        mock.stall_connect_port = 9201;
        mock.report_handshaking = true;
        auto opened = nf::DirectInitiator<fix::Schema, ErrorRecorder, TimeoutMockTransport>::open(
            cfg, ErrorRecorder{}, std::move(mock));
        CHECK(opened.ok());
        auto init = std::move(opened).value();

        CHECK(init.connect(2000, t0).ok());
        CHECK(init.poll_once(2299, t0).ok());
        const auto st = init.poll_once(2300, t0);
        CHECK(st.code == nf::ErrorCode::tls_handshake_timeout);
        CHECK(!init.valid_transport());
        CHECK(init.handler().last_code == nf::ErrorCode::tls_handshake_timeout);
    }

    // 3. Write-stall timeout forces Logout then closes with ErrorCode::write_stalled!
    {
        TempDir dir;
        auto cfg = make_config(dir);
        cfg.endpoints = {{"127.0.0.1", 9301}, {"127.0.0.1", 9302}};
        cfg.connection_strategy = nf::ConnectionStrategy::ordered(100, 1000, 2, 0, true, true);
        cfg.heartbeat_ms = 30'000;
        cfg.write_stall_ms = 500;
        cfg.logout_timeout_ms = 200;

        auto opened = nf::DirectInitiator<fix::Schema, ErrorRecorder, TimeoutMockTransport>::open(cfg);
        CHECK(opened.ok());
        auto init = std::move(opened).value();

        CHECK(init.connect(1000, t0).ok());
        std::string wire;
        CHECK(init.transport().inner.pop_sent_frame(wire));
        init.transport().inner.push_from_peer(
            make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 1, t0));
        CHECK(init.poll_once(1001, t0).ok());
        CHECK(init.is_active());

        // Block all socket writes and send an order -> write timer starts at t=2000!
        init.transport().block_writes = true;
        const auto sent = init.send_stream<fix::NewOrderSingle>([](auto& w) noexcept {
            w.cl_ord_id("STALL-1").symbol("AAPL").side('1')
             .transact_time(t1).order_qty(nf::Decimal{10, 0}).ord_type('1');
        }, 2000, t1);
        CHECK(sent.ok());
        CHECK(init.wants_write());
        CHECK(init.next_deadline_ms() == 2500);

        // Before write_stall_ms: still active
        CHECK(init.poll_once(2499, t1).ok());
        CHECK(init.is_active());

        // At write_stall_ms (2500): initiates Logout (state leaves active), arms logout deadline (2700)
        CHECK(init.poll_once(2500, t1).ok());
        CHECK(init.state() == nf::SessionState::logout_pending);
        CHECK(init.valid_transport());
        CHECK(init.next_deadline_ms() == 2700);

        // At 2700: still stalled -> closes with ErrorCode::write_stalled & notifies on_error!
        const auto stalled = init.poll_once(2700, t1);
        CHECK(stalled.code == nf::ErrorCode::write_stalled);
        CHECK(!init.valid_transport());
        CHECK(init.handler().last_code == nf::ErrorCode::write_stalled);
    }
}

// Regression test for R04: DirectInitiator redacts BOTH outbound and inbound Logon credentials
// (tags 553/554) before persisting to the journal.
void test_inbound_and_outbound_logon_redacts_secrets() {
    TempDir dir;
    auto cfg = make_config(dir);
    cfg.username = "OUT-SECRET-USER";
    cfg.password = "OUT-SECRET-PASS";

    auto opened = nf::DirectInitiator<fix::Schema, RecordingHandler, LoopbackTransport>::open(cfg);
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto init = std::move(opened).value();

    CHECK(init.connect(0, t0).ok());
    std::string out_logon;
    CHECK(init.transport().pop_sent_frame(out_logon));
    // Outbound wire frame DOES carry credentials to the counterparty
    CHECK(field(out_logon, 553) == "OUT-SECRET-USER");
    CHECK(field(out_logon, 554) == "OUT-SECRET-PASS");

    // Counterparty replies with an inbound Logon that ALSO contains 553/554 secrets!
    const auto peer_logon = make_frame(
        nf::message<"A">().set<98>(0).set<108>(30)
            .set<553>("IN-SECRET-USER").set<554>("IN-SECRET-PASS"),
        "SELL", "BUY", 1, t0);
    init.transport().push_from_peer(peer_logon);
    CHECK(init.poll_once(1, t0).ok());
    CHECK(init.is_active());

    bool saw_in_logon = false;
    bool saw_out_logon = false;
    for (const auto& rec : init.journal().records()) {
        CHECK(rec.payload.find("OUT-SECRET-USER") == std::string_view::npos);
        CHECK(rec.payload.find("OUT-SECRET-PASS") == std::string_view::npos);
        CHECK(rec.payload.find("IN-SECRET-USER") == std::string_view::npos);
        CHECK(rec.payload.find("IN-SECRET-PASS") == std::string_view::npos);
        CHECK(rec.payload.find("\x01" "553=") == std::string_view::npos);
        CHECK(rec.payload.find("\x01" "554=") == std::string_view::npos);
        if (rec.kind == nf::RecordKind::outbound && rec.seq == 1) {
            saw_out_logon = true;
            CHECK(field(rec.payload, 35) == "A");
            CHECK(field(rec.payload, 49) == "BUY" && field(rec.payload, 56) == "SELL");
        }
        if (rec.kind == nf::RecordKind::inbound && rec.seq == 1) {
            saw_in_logon = true;
            CHECK(field(rec.payload, 35) == "A");
            // Preserves inbound CompID direction!
            CHECK(field(rec.payload, 49) == "SELL" && field(rec.payload, 56) == "BUY");
            CHECK(field(rec.payload, 108) == "30");
        }
    }
    CHECK(saw_out_logon && saw_in_logon);
}

// Regression test for R08: large send_stream payloads spilling out of the fast reserve
// buffer MUST invoke the user writer lambda at most once!
void test_send_stream_large_spill_invokes_lambda_once() {
    // 1. DirectInitiator::send_stream > 4 KiB (spills from journal reserve into encode_buf_)
    TempDir dir;
    auto opened = nf::DirectInitiator<fix::Schema, RecordingHandler, LoopbackTransport>::open(
        make_config(dir));
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto init = std::move(opened).value();
    CHECK(init.connect(0, t0).ok());
    std::string wire;
    CHECK(init.transport().pop_sent_frame(wire));
    init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 1, t0));
    CHECK(init.poll_once(1, t0).ok());
    CHECK(init.is_active());

    const std::string large_id(5200, 'X');
    int lambda_calls = 0;
    const auto sent = init.send_stream<fix::NewOrderSingle>([&](auto& w) {
        ++lambda_calls;
        w.cl_ord_id(large_id)
         .symbol("AAPL")
         .side('1')
         .transact_time(t1)
         .order_qty(nf::Decimal{100, 0})
         .ord_type('1');
    }, 2, t1);
    CHECK(sent.ok());
    CHECK(lambda_calls == 1);

    std::string large_wire;
    CHECK(init.transport().pop_sent_frame(large_wire));
    CHECK(field(large_wire, 11) == large_id);

    // 2. stream_body_and_invoke > 4 KiB (spills from stack buffer to vector in a single call)
    int body_calls = 0;
    std::string captured_body;
    const auto body_st = nf::detail::stream_body_and_invoke<fix::NewOrderSingle>(
        [&](auto& w) {
            ++body_calls;
            w.cl_ord_id(large_id).symbol("AAPL").side('1')
             .transact_time(t1).order_qty(nf::Decimal{100, 0}).ord_type('1');
        },
        [&](nf::detail::MessageView view) {
            CHECK(view.prebuilt_body);
            captured_body.assign(view.value_bytes, view.value_byte_count);
            return nf::Status::success();
        });
    CHECK(body_st.ok());
    CHECK(body_calls == 1);
    CHECK(captured_body.find(large_id) != std::string::npos);
}

struct DrainRecoveryHandler {
    bool fail_after{};
    int send_count{2};
    std::string reply_prefix{"DRAIN-R"};
    bool view_ok_before{};
    bool view_ok_after{};
    int recoveries{};

    template <class Init>
    [[nodiscard]] nf::Status on_message(Init& init, const nf::FlatMessageView& msg,
                                        const nf::DeliveryInfo& info) noexcept {
        if (info.is_recovery) ++recoveries;
        const auto before = msg.find_unique(17);
        view_ok_before = before.ok() && before.value().value == "E-F01";
        nf::DirectSendQueue q(8, 512);
        for (int i = 0; i < send_count; ++i) {
            const std::string cid = reply_prefix + std::to_string(i);
            const auto ps = q.try_push_stream<fix::NewOrderSingle>([&](auto& w) {
                w.cl_ord_id(cid).symbol("AAPL").side('1').transact_time(t1)
                 .order_qty(nf::Decimal{10, 0}).ord_type('1');
            });
            if (!ps.ok()) return ps;
        }
        if (send_count > 0) {
            const auto dr = init.drain(q, 8, 10, t1);
            if (!dr.ok()) return dr.status();
        }
        const auto after = msg.find_unique(17);
        const auto typed = msg.as<fix::ExecutionReport>().exec_id();
        view_ok_after = after.ok() && after.value().value == "E-F01" &&
                        typed.ok() && typed.value() == "E-F01";
        return fail_after ? nf::Status{nf::ErrorCode::callback_failed} : nf::Status::success();
    }
};

// Regression tests for F01 (DirectSendQueue::drain recovery reuse), F02 (369=0 recovery
// consistency), F03 (inbound FlatMessageView preserved across recovery reply reuse),
// and F04 (Logon / Heartbeat / Logout timeouts close transport and advance failover).
void test_followup_audit_recovery_and_timeouts() {
    // 1. F01 + F03: DirectSendQueue::drain() & send_stream() inside recovery callbacks
    //    preserve inbound FlatMessageView AND reuse/validate multi-reply sequences!
    {

        TempDir dir;
        const auto cfg = make_config(dir);
        {
            auto opened = nf::DirectInitiator<fix::Schema, DrainRecoveryHandler, LoopbackTransport>::open(
                cfg, DrainRecoveryHandler{true, 2, "DRAIN-R"});
            CHECK(opened.ok());
            auto init = std::move(opened).value();
            CHECK(init.connect(0, t0).ok());
            init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 1, t0));
            CHECK(init.poll_once(1, t0).ok());
            init.transport().push_from_peer(make_stream_frame<fix::ExecutionReport>([](auto& w) {
                w.order_id("O1").exec_id("E-F01").cl_ord_id("ORD-1").exec_type('0')
                 .ord_status('0').symbol("AAPL").side('1').leaves_qty(nf::Decimal{1, 0})
                 .cum_qty(nf::Decimal{0, 0}).avg_px(nf::Decimal{0, 0});
            }, "SELL", "BUY", 2, t1));
            CHECK(init.poll_once(2, t1).code == nf::ErrorCode::callback_failed);
            CHECK(!init.valid_transport()); // F04: fail() must close transport!
        }

        // Recovery attempt with modified second reply -> rejected without duplicating!
        {
            auto opened = nf::DirectInitiator<fix::Schema, DrainRecoveryHandler, LoopbackTransport>::open(
                cfg, DrainRecoveryHandler{false, 2, "CHANGED-R"});
            CHECK(opened.ok());
            auto init = std::move(opened).value();
            CHECK(init.connect(10, t2).ok());
            init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 3, t2));
            CHECK(init.poll_once(11, t2).code == nf::ErrorCode::callback_failed);
        }

        // Recovery attempt with fewer replies (1 instead of 2) -> rejected!
        {
            auto opened = nf::DirectInitiator<fix::Schema, DrainRecoveryHandler, LoopbackTransport>::open(
                cfg, DrainRecoveryHandler{false, 1, "DRAIN-R"});
            CHECK(opened.ok());
            auto init = std::move(opened).value();
            CHECK(init.connect(20, t2).ok());
            init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 4, t2));
            CHECK(init.poll_once(21, t2).code == nf::ErrorCode::callback_failed);
        }

        // Exact matching recovery via drain() -> reuses both replies, preserves view, commits delivery!
        {
            auto opened = nf::DirectInitiator<fix::Schema, DrainRecoveryHandler, LoopbackTransport>::open(
                cfg, DrainRecoveryHandler{false, 2, "DRAIN-R"});
            CHECK(opened.ok());
            auto init = std::move(opened).value();
            CHECK(init.connect(30, t2).ok());
            init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 5, t2));
            CHECK(init.poll_once(31, t2).ok());
            CHECK(init.handler().recoveries == 1);
            CHECK(init.handler().view_ok_before && init.handler().view_ok_after);
            int replies = 0, dependent = 0;
            for (const auto& rec : init.journal().records()) {
                if (rec.kind == nf::RecordKind::outbound && field(rec.payload, 35) == "D") {
                    ++replies;
                    if (rec.dependency != 0) ++dependent;
                }
            }
            CHECK(replies == 2 && dependent == 2);
        }
    }

    // 2. F02: Inbound message with 369=0 accepted on initial receipt and cleanly redelivered on recovery
    {
        struct AckZeroHandler {
            bool fail_after{};
            int calls{};
            int recovery_calls{};
            [[nodiscard]] nf::Status on_message(const nf::FlatMessageView& msg,
                                                const nf::DeliveryInfo& info) noexcept {
                ++calls;
                if (info.is_recovery) ++recovery_calls;
                const auto eid = msg.as<fix::ExecutionReport>().exec_id();
                if (!eid.ok() || eid.value() != "ACK-ZERO") return nf::Status{nf::ErrorCode::invalid_value};
                return fail_after ? nf::Status{nf::ErrorCode::callback_failed} : nf::Status::success();
            }
        };
        TempDir dir;
        const auto cfg = make_config(dir);
        nf::RecordId er_id{};
        {
            auto opened = nf::DirectInitiator<fix::Schema, AckZeroHandler, LoopbackTransport>::open(
                cfg, AckZeroHandler{true});
            CHECK(opened.ok());
            auto init = std::move(opened).value();
            CHECK(init.connect(0, t0).ok());
            init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 1, t0));
            CHECK(init.poll_once(1, t0).ok());
            std::array<char, 2048> bytes{};
            nf::SessionHeader hdr{"SELL", "BUY", 2, t1};
            hdr.last_msg_seq_num_processed = 0;
            const auto enc = nf::encode_stream<fix::ExecutionReport>(hdr, bytes, [](auto& w) {
                w.order_id("O1").exec_id("ACK-ZERO").cl_ord_id("ORD-1").exec_type('0')
                 .ord_status('0').symbol("AAPL").side('1').leaves_qty(nf::Decimal{1, 0})
                 .cum_qty(nf::Decimal{0, 0}).avg_px(nf::Decimal{0, 0});
            });
            CHECK(enc.ok());
            init.transport().push_from_peer({bytes.data(), enc.value()});
            CHECK(init.poll_once(2, t1).code == nf::ErrorCode::callback_failed);
            CHECK(init.handler().calls == 1);
            for (const auto& rec : init.journal().records()) {
                if (rec.kind == nf::RecordKind::inbound && rec.seq == 2) er_id = rec.id;
            }
            CHECK(er_id != 0 && !init.journal().has_delivery(er_id));
        }
        {
            auto opened = nf::DirectInitiator<fix::Schema, AckZeroHandler, LoopbackTransport>::open(
                cfg, AckZeroHandler{false});
            CHECK(opened.ok());
            auto init = std::move(opened).value();
            CHECK(init.connect(10, t2).ok());
            init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 3, t2));
            CHECK(init.poll_once(11, t2).ok());
            CHECK(init.handler().calls == 1 && init.handler().recovery_calls == 1);
            CHECK(init.journal().has_delivery(er_id));
        }
    }

    // 3. F04: Logon timeout, Heartbeat timeout, and Logout timeout close transport & advance failover
    {
        struct ErrTrackHandler {
            nf::ErrorCode last_err{nf::Status::success().code};
            int disconnects{};
            void on_error(nf::Status st) noexcept { last_err = st.code; }
            void on_disconnected(nf::Status) noexcept { ++disconnects; }
        };
        TempDir dir;
        auto cfg = make_config(dir);
        cfg.logon_timeout_ms = 50;
        cfg.logout_timeout_ms = 50;
        cfg.heartbeat_ms = 100;
        cfg.endpoints = {{"127.0.0.1", 9901}, {"127.0.0.1", 9902}};
        cfg.connection_strategy = nf::ConnectionStrategy::ordered(100, 1000, 2, 0, true, true);

        auto opened = nf::DirectInitiator<fix::Schema, ErrTrackHandler, LoopbackTransport>::open(cfg);
        CHECK(opened.ok());
        auto init = std::move(opened).value();

        // 3a. Logon timeout closes transport, advances failover to standby (index 1), allows reconnect!
        CHECK(init.connect(0, t0).ok());
        CHECK(init.active_endpoint_index() == 0);
        const auto lt = init.poll_once(100, t0);
        CHECK(lt.code == nf::ErrorCode::logon_timeout);
        CHECK(!init.valid_transport());
        CHECK(init.state() == nf::SessionState::disconnected);
        CHECK(init.consecutive_connect_failures() == 1);
        CHECK(init.active_endpoint_index() == 1);
        CHECK(init.handler().last_err == nf::ErrorCode::logon_timeout);
        CHECK(init.handler().disconnects == 1);

        // Reconnect immediately succeeds on standby endpoint 1 and completes Logon!
        CHECK(init.reconnect_if_due(100, t0).ok());
        CHECK(init.valid_transport() && init.active_endpoint_index() == 1);
        init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(1), "SELL", "BUY", 1, t0));
        CHECK(init.poll_once(101, t0).ok());
        CHECK(init.is_active());

        // 3b. Heartbeat / TestRequest timeout -> sends TestRequest, then Logout, then closes cleanly!
        // Peer HeartBtInt=1s -> test interval = 1200 ms.
        CHECK(init.poll_once(1'400, t1).ok()); // emits TestRequest (35=1)
        std::string tr_wire;
        bool saw_tr = false;
        while (init.transport().pop_sent_frame(tr_wire)) {
            if (field(tr_wire, 35) == "1") saw_tr = true;
        }
        CHECK(saw_tr);
        // Advance past TestRequest response deadline (1400 + 1200 = 2600) -> enters logout_pending
        CHECK(init.poll_once(2'700, t1).ok());
        CHECK(init.state() == nf::SessionState::logout_pending);
        // Advance past logout_timeout_ms (50 ms) -> heartbeat_timeout closes transport & disconnects!
        const auto hb = init.poll_once(2'800, t1);
        CHECK(hb.code == nf::ErrorCode::heartbeat_timeout);
        CHECK(!init.valid_transport());
        CHECK(init.state() == nf::SessionState::disconnected);
        CHECK(init.handler().last_err == nf::ErrorCode::heartbeat_timeout);

        // 3c. Reconnect again (reset_to_primary -> index 0), then local logout() times out cleanly!
        const auto next_at = init.next_reconnect_at_ms();
        CHECK(init.reconnect_if_due(next_at, t2).ok());
        CHECK(init.valid_transport());
        init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 2, t2));
        CHECK(init.poll_once(next_at + 1, t2).ok());
        CHECK(init.is_active());
        CHECK(init.logout("bye", next_at + 10, t2).ok());
        CHECK(init.state() == nf::SessionState::logout_pending);
        const auto lo = init.poll_once(next_at + 100, t2);
        CHECK(lo.code == nf::ErrorCode::logout_timeout);
        CHECK(!init.valid_transport());
        CHECK(init.state() == nf::SessionState::disconnected);
        CHECK(init.handler().last_err == nf::ErrorCode::logout_timeout);
    }
}

// Regression tests for code review N01 (retention vs unflushed TX), N02 (SequenceReset),
// N03 (DirectSendQueue constructor integer overflow), and N05 (overlapping ResendRequest dedup).
void test_code_review_n01_n02_n03_n05_regressions() {
    // N03: DirectSendQueue constructor rejects integer overflow / oversized arguments cleanly.
    {
        auto expect_invalid = [](std::size_t cap, std::size_t body) {
            bool threw = false;
            try {
                nf::DirectSendQueue q(cap, body);
                (void)q.capacity();
            } catch (const std::invalid_argument&) {
                threw = true;
            }
            CHECK(threw);
        };
        expect_invalid(256, std::numeric_limits<std::size_t>::max());
        expect_invalid(256, std::numeric_limits<std::size_t>::max() - 16U);
        expect_invalid(256, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1U);
        expect_invalid(std::numeric_limits<std::size_t>::max(), 512);
        expect_invalid((std::size_t{1} << 30U) + 1U, 512);
        // Normal valid construction still works identically.
        nf::DirectSendQueue ok_q(16, 128);
        CHECK(ok_q.capacity() == 16 && ok_q.max_body_bytes() == 128);
    }

    // N01: DirectInitiator retention must never prune unflushed outbound frames even with high peer 369.
    {
        TempDir dir;
        auto cfg = make_config(dir);
        cfg.journal_capacity_bytes = 4U * 1024U * 1024U;
        cfg.journal_segment_bytes = 8'192;
        cfg.journal_max_segments = 256;
        cfg.max_pending_messages = 256;
        cfg.retain_outbound_messages = 32; // Smaller than queued backlog (120)
        cfg.write_stall_ms = 60'000;

        struct NoopH {
            int errors{};
            void on_error(nf::Status) noexcept { ++errors; }
        };
        auto opened = nf::DirectInitiator<fix::Schema, NoopH, LoopbackTransport>::open(cfg);
        CHECK(opened.ok());
        auto init = std::move(opened).value();
        CHECK(init.connect(0, t0).ok());
        init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30),
                                                   "SELL", "BUY", 1, t0));
        CHECK(init.poll_once(1, t0).ok());
        CHECK(init.is_active());
        init.transport().tx_wire.clear();

        // Block writes and enqueue 120 orders spanning multiple 8 KiB segments.
        init.transport().block_writes = true;
        const std::string pad(64, 'K');
        for (int i = 0; i < 120; ++i) {
            const std::string cid = "DN01-" + std::to_string(i) + "-" + pad;
            CHECK(init.enqueue_stream<fix::NewOrderSingle>([&](auto& w) {
                w.cl_ord_id(cid).symbol("AAPL").side('1').transact_time(t1)
                 .order_qty(nf::Decimal{10, 0}).ord_type('1');
            }, static_cast<std::uint64_t>(2 + i), t1).ok());
        }
        CHECK(init.pending_outbound() == 120);

        // Peer sends Heartbeat claiming 369=121 while all 120 orders are still unflushed!
        std::array<char, 512> hbuf{};
        nf::SessionHeader hhdr{"SELL", "BUY", 2, t1};
        hhdr.last_msg_seq_num_processed = 121;
        const auto henc = nf::encode_fix44(nf::message<"0">(), hhdr, hbuf);
        CHECK(henc.ok());
        init.transport().push_from_peer({hbuf.data(), henc.value()});
        CHECK(init.poll_once(200, t1).ok());
        CHECK(init.is_active());

        // Unblock transport and flush: all 120 frames must drain intact and retention must catch up!
        init.transport().block_writes = false;
        CHECK(init.flush(201).ok());
        CHECK(!init.wants_write() && init.is_active() && init.handler().errors == 0);
        int sent_count = 0;
        std::string wire;
        while (init.transport().pop_sent_frame(wire)) {
            if (field(wire, 35) == "D") {
                CHECK(field(wire, 34) == std::to_string(2 + sent_count));
                ++sent_count;
            }
        }
        CHECK(sent_count == 120);
        CHECK(init.journal().lowest_retained_outbound_seq() > 1);
    }

    // N02: DirectInitiator SequenceReset-Reset mode (no-op, PossDup replay, live lower -> Reject).
    {
        TempDir dir;
        auto cfg = make_config(dir);
        struct ErrH {
            int errors{};
            nf::Status last{};
            void on_error(nf::Status s) noexcept { ++errors; last = s; }
        };
        auto opened = nf::DirectInitiator<fix::Schema, ErrH, LoopbackTransport>::open(cfg);
        CHECK(opened.ok());
        auto init = std::move(opened).value();
        CHECK(init.connect(0, t0).ok());
        init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30),
                                                   "SELL", "BUY", 1, t0));
        CHECK(init.poll_once(1, t0).ok());
        init.transport().tx_wire.clear();

        // Forward Reset -> 5, then no-op Reset -> 5, then PossDup replay -> 5.
        init.transport().push_from_peer(make_frame(nf::message<"4">().set<36>(5), "SELL", "BUY", 2, t0));
        CHECK(init.poll_once(2, t0).ok());
        CHECK(init.is_active() && init.next_inbound_seq() == 5);

        init.transport().push_from_peer(make_frame(nf::message<"4">().set<36>(5), "SELL", "BUY", 5, t1));
        init.transport().push_from_peer(make_frame(nf::message<"4">().set<36>(4), "SELL", "BUY", 2, t1, true, t0));
        CHECK(init.poll_once(3, t1).ok());
        CHECK(init.is_active() && init.next_inbound_seq() == 5 && init.handler().errors == 0);
        CHECK(init.transport().tx_wire.empty());

        // Live backward Reset (NewSeqNo=3 < 5): emits Reject (35=3, 371=36, 372=4, 373=5) and stays active.
        init.transport().push_from_peer(make_frame(nf::message<"4">().set<123>('N').set<36>(3), "SELL", "BUY", 5, t2));
        CHECK(init.poll_once(4, t2).ok());
        CHECK(init.is_active() && init.next_inbound_seq() == 5);
        CHECK(init.handler().errors == 1 && init.handler().last.code == nf::ErrorCode::invalid_value &&
              init.handler().last.tag == 36);
        std::string rej;
        CHECK(init.transport().pop_sent_frame(rej));
        CHECK(field(rej, 35) == "3" && field(rej, 45) == "5" &&
              field(rej, 371) == "36" && field(rej, 372) == "4" && field(rej, 373) == "5");

        // Normal message at seq 5 still accepted cleanly.
        init.transport().push_from_peer(make_frame(nf::message<"0">(), "SELL", "BUY", 5, t2));
        CHECK(init.poll_once(5, t2).ok());
        CHECK(init.is_active() && init.next_inbound_seq() == 6);
    }

    // N05: DirectInitiator merges overlapping ResendRequests across distinct inbound dependencies.
    {
        TempDir dir;
        auto cfg = make_config(dir);
        cfg.max_pending_messages = 8; // Small replay_ring_ (capacity 8)
        cfg.write_stall_ms = 60'000;
        struct NoopH {
            int errors{};
            void on_error(nf::Status) noexcept { ++errors; }
        };
        auto opened = nf::DirectInitiator<fix::Schema, NoopH, LoopbackTransport>::open(cfg);
        CHECK(opened.ok());
        auto init = std::move(opened).value();
        CHECK(init.connect(0, t0).ok());
        init.transport().push_from_peer(make_frame(nf::message<"A">().set<98>(0).set<108>(30),
                                                   "SELL", "BUY", 1, t0));
        CHECK(init.poll_once(1, t0).ok());

        for (int i = 0; i < 10; ++i) {
            CHECK(init.send_stream<fix::NewOrderSingle>([&](auto& w) {
                w.cl_ord_id("DR-" + std::to_string(i)).symbol("AAPL").side('1').transact_time(t1)
                 .order_qty(nf::Decimal{1, 0}).ord_type('1');
            }, static_cast<std::uint64_t>(2 + i), t1).ok());
        }
        init.transport().tx_wire.clear();

        // Block writes and flood 20 overlapping ResendRequests (> replay_ring_.size() == 8!).
        init.transport().block_writes = true;
        for (int i = 0; i < 20; ++i) {
            const std::uint64_t end_seq = static_cast<std::uint64_t>(7 + (i % 5)); // up to 11
            init.transport().push_from_peer(make_frame(nf::message<"2">().set<7>(2).set<16>(end_seq),
                                                       "SELL", "BUY", static_cast<std::uint64_t>(2 + i), t2));
        }
        CHECK(init.poll_once(30, t2).ok());
        CHECK(init.is_active() && init.handler().errors == 0);

        // Unblock and flush: exact 10 frames (seq 2..11) replayed once, all 20 ResendRequests delivered!
        init.transport().block_writes = false;
        CHECK(init.flush(31).ok());
        CHECK(!init.wants_write() && init.is_active());
        std::vector<std::string> seqs;
        std::string wire;
        while (init.transport().pop_sent_frame(wire)) {
            if (field(wire, 35) == "D") {
                CHECK(field(wire, 43) == "Y");
                seqs.push_back(field(wire, 34));
            }
        }
        CHECK(seqs.size() == 10);
        for (std::size_t i = 0; i < seqs.size(); ++i) CHECK(seqs[i] == std::to_string(2 + i));
        CHECK(init.journal().completed_inbound_seq() == 21);
    }
}

} // namespace

int main() {
    test_default_session_key_auto_derivation();
    test_named_fixed_enqueue_batch_and_spsc_queue();
    test_zero_alloc_zero_lock_zero_fsync_steady_state();
    test_restart_recovery_resend_replay_and_gapfill();
    test_sigkill_crash_recovery_redelivers_uncommitted_delivery();
    test_subms_timestamps_and_optional_checksum_skip();
    test_multi_endpoint_failover_and_backoff();
    test_zero_overhead_mmap_telemetry_and_stage_histograms();
    test_recovery_reuses_dependent_business_reply();
    test_connect_tls_and_write_stall_timeouts();
    test_inbound_and_outbound_logon_redacts_secrets();
    test_send_stream_large_spill_invokes_lambda_once();
    test_followup_audit_recovery_and_timeouts();
    test_code_review_n01_n02_n03_n05_regressions();
    if (failures != 0) {
        std::cerr << failures << " direct initiator test(s) failed\n";
        return 1;
    }
    std::cout << "DirectInitiator zero-overhead, restart recovery, gap-fill and SIGKILL tests passed\n";
    return 0;
}
