#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/fix44_sample.hpp>
#include <nimblefix/message.hpp>

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
        tx_wire.append(in.data(), in.size());
        return {in.size(), false, false, nf::Status::success()};
    }
    [[nodiscard]] nf::TransportIoResult write_batch(std::span<const std::span<const char>> chunks) noexcept {
        if (!open_state) return {0, false, true, nf::Status{nf::ErrorCode::io_error}};
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
        auto er = fix::ExecutionReport::make()
            .order_id("O1").exec_id("E1").cl_ord_id("ORD-HOT").exec_type('0')
            .ord_status('0').symbol("AAPL").side('1')
            .leaves_qty(nf::Decimal{100, 0})
            .cum_qty(nf::Decimal{0, 0})
            .avg_px(nf::Decimal{0, 0});
        peer_reports.push_back(make_frame(er, "SELL", "BUY", 2 + i, t1));
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

        auto er = fix::ExecutionReport::make()
            .order_id("O1").exec_id("E1").cl_ord_id("ORD-1").exec_type('0')
            .ord_status('0').symbol("AAPL").side('1')
            .leaves_qty(nf::Decimal{10, 0}).cum_qty(nf::Decimal{0, 0})
            .avg_px(nf::Decimal{0, 0});
        init.transport().push_from_peer(make_frame(er, "SELL", "BUY", 2, t1));
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
        auto er3 = fix::ExecutionReport::make()
            .order_id("O2").exec_id("E-GAP").cl_ord_id("ORD-2").exec_type('0')
            .ord_status('0').symbol("MSFT").side('2')
            .leaves_qty(nf::Decimal{20, 0}).cum_qty(nf::Decimal{0, 0})
            .avg_px(nf::Decimal{0, 0});
        init.transport().push_from_peer(make_frame(er3, "SELL", "BUY", 3, t2, true, t1));
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
        auto er = fix::ExecutionReport::make()
            .order_id("O-CRASH").exec_id("E-CRASH").cl_ord_id("ORD-C").exec_type('0')
            .ord_status('0').symbol("NVDA").side('1')
            .leaves_qty(nf::Decimal{50, 0}).cum_qty(nf::Decimal{0, 0})
            .avg_px(nf::Decimal{0, 0});
        init.transport().push_from_peer(make_frame(er, "SELL", "BUY", 2, t1));
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
    auto er = fix::ExecutionReport::make()
        .order_id("O1").exec_id("EX-TYPED-1").cl_ord_id("BATCH-00")
        .exec_type('0').ord_status('0').symbol("AAPL").side('1')
        .leaves_qty(nf::Decimal{100, 0}).cum_qty(nf::Decimal{0, 0}).avg_px(nf::Decimal{0, 0});
    init.transport().push_from_peer(make_frame(er, "SELL", "BUY", 2, t1));
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

    // 4. Scheme A (SSO make() + std::optional) & Scheme B (SSO Draft + native if + reset)
    //    with hard 0-malloc / 0-mutex / 0-fsync audit AND SSO spill (>256B) verification!
    g_malloc_calls.store(0);
    g_mutex_calls.store(0);
    g_fsync_calls.store(0);
    g_audit_hot_path.store(true);
    {
        const std::optional<nf::Decimal> no_price = std::nullopt;
        const std::optional<nf::Decimal> limit_price = nf::Decimal{15025, 2};
        auto mkt = fix::NewOrderSingle::make()
            .cl_ord_id("OPT-MKT").symbol("AAPL").side('1')
            .transact_time(t2).order_qty(nf::Decimal{10, 0}).ord_type('1')
            .price(no_price);
        auto lmt = fix::NewOrderSingle::make()
            .cl_ord_id("OPT-LMT").symbol("AAPL").side('2')
            .transact_time(t2).order_qty(nf::Decimal{20, 0}).ord_type('2')
            .price(limit_price);
        CHECK(init.send(mkt, 30, t2).ok());
        CHECK(init.send(lmt, 30, t2).ok());

        fix::NewOrderSingle::Draft draft;
        for (int k = 0; k < 2; ++k) {
            draft.reset();
            // Intentionally set fields out of schema order to verify canonical WireRank sorting!
            if (k == 1) draft.price(nf::Decimal{9950, 2});
            draft.ord_type(k == 1 ? '2' : '1')
                 .order_qty(nf::Decimal{30, 0})
                 .transact_time(t2)
                 .side('1')
                 .symbol("NVDA")
                 .cl_ord_id(k == 0 ? "DRF-MKT" : "DRF-LMT");
            CHECK(init.send(draft, 31, t2).ok());
        }
    }
    g_audit_hot_path.store(false);
    CHECK(g_malloc_calls.load() == 0);
    CHECK(g_mutex_calls.load() == 0);
    CHECK(g_fsync_calls.load() == 0);

    // Verify optional absent/present & canonical ordering on the wire.
    CHECK(init.transport().pop_sent_frame(wire));
    CHECK(field(wire, 11) == "OPT-MKT" && field(wire, 44).empty());
    CHECK(init.transport().pop_sent_frame(wire));
    CHECK(field(wire, 11) == "OPT-LMT" && field(wire, 44) == "150.25");
    CHECK(init.transport().pop_sent_frame(wire));
    CHECK(field(wire, 11) == "DRF-MKT" && field(wire, 44).empty());
    CHECK(init.transport().pop_sent_frame(wire));
    CHECK(field(wire, 11) == "DRF-LMT" && field(wire, 44) == "99.50");
    // Verify canonical schema order (11 before 55 before 54 before 44).
    CHECK(wire.find("11=DRF-LMT") < wire.find("55=NVDA") &&
          wire.find("55=NVDA") < wire.find("44=99.50"));

    // Verify SSO spill (>256B string) does NOT reject orders in either make() or Draft!
    const std::string long_id(320, 'X');
    auto spilled_make = fix::NewOrderSingle::make()
        .cl_ord_id(long_id).symbol("AAPL").side('1')
        .transact_time(t2).order_qty(nf::Decimal{1, 0}).ord_type('1');
    CHECK(spilled_make.status().ok());
    CHECK(init.send(spilled_make, 32, t2).ok());
    CHECK(init.transport().pop_sent_frame(wire));
    CHECK(field(wire, 11) == long_id);

    fix::NewOrderSingle::Draft spilled_draft;
    spilled_draft.cl_ord_id(long_id).symbol("AAPL").side('1')
        .transact_time(t2).order_qty(nf::Decimal{1, 0}).ord_type('1');
    CHECK(spilled_draft.spilled());
    CHECK(spilled_draft.status().ok());
    CHECK(init.send(spilled_draft, 33, t2).ok());
    CHECK(init.transport().pop_sent_frame(wire));
    CHECK(field(wire, 11) == long_id);
}

} // namespace

int main() {
    test_default_session_key_auto_derivation();
    test_named_fixed_enqueue_batch_and_spsc_queue();
    test_zero_alloc_zero_lock_zero_fsync_steady_state();
    test_restart_recovery_resend_replay_and_gapfill();
    test_sigkill_crash_recovery_redelivers_uncommitted_delivery();
    if (failures != 0) {
        std::cerr << failures << " direct initiator test(s) failed\n";
        return 1;
    }
    std::cout << "DirectInitiator zero-overhead, restart recovery, gap-fill and SIGKILL tests passed\n";
    return 0;
}
