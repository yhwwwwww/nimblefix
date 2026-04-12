// bench/fastfix_loopback.cpp
//
// Two-process FIX loopback latency benchmark: genuine acceptor process vs. initiator process.
// Forks the current binary: the child becomes the acceptor, the parent becomes the initiator.
//
// Sends both 35=D (NewOrderSingle) and 35=F (OrderCancelRequest) with the full standard
// FIX44 field set (Account, Currency, ExecInst, HandlInst, SecurityID/Source, OrderQty,
// OrdType, Price, Side, Symbol, TimeInForce, TransactTime, ExDestination, SecurityExchange,
// NoPartyIDs group) and matches acks to originating requests by ClOrdID.
//
// Initiator uses two threads:
//   session thread  – owns all AdminProtocol state; drains a send queue (built by
//                     sender), calls SendApplication + BusySend; also receives all
//                     inbound frames via TryReceiveFrameView, calls OnInbound, sends
//                     admin responses (heartbeats etc.), and records RTTs.
//   sender thread   – builds business structs → fastfix::message::Message, records T0,
//                     pushes to the send queue, then sleeps send_interval_us. No
//                     protocol state touched here.
//
// Measurement boundary:
//   T0 = immediately before struct→FIX message construction
//   T1 = immediately after FIX ExecutionReport→struct extraction
//
// Usage:
//   fastfix-loopback --artifact <path/to/quickfix_FIX44.art>
//                    [--iterations <N>]   default 1000 per message type
//                    [--warmup <W>]        default 50  per message type

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <sched.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fastfix/codec/fix_codec.h"
#include "fastfix/message/message.h"
#include "fastfix/profile/normalized_dictionary.h"
#include "fastfix/profile/profile_loader.h"
#include "fastfix/session/admin_protocol.h"
#include "fastfix/store/memory_store.h"
#include "fastfix/transport/tcp_transport.h"

#include "bench_support.h"

namespace {

// ---------------------------------------------------------------------------
// CPU affinity (best-effort)
// ---------------------------------------------------------------------------

static void pin_to_core(int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
        std::fprintf(stderr, "warning: sched_setaffinity(core %d) failed: %s\n",
            core, std::strerror(errno));
    }
}

// ---------------------------------------------------------------------------
// Business-level structs (what real application code works with)
// ---------------------------------------------------------------------------

struct NewOrderFields {
    std::string cl_ord_id;
    std::string account{"ACC001"};
    std::string currency{"USD"};
    char exec_inst{'G'};             // G = AllOrNone
    char hand_l_inst{'1'};           // 1 = AutomatedExecution
    std::string security_id{"US0231351067"};
    char security_id_source{'4'};    // 4 = ISIN
    double order_qty{100.0};
    char ord_type{'2'};              // 2 = Limit
    double price{150.25};
    char side{'1'};                  // 1 = Buy
    std::string symbol{"AAPL"};
    char time_in_force{'0'};         // 0 = Day
    std::string transact_time;
    std::string ex_destination{"XNYS"};
    std::string security_exchange{"XNYS"};
    std::string party_id{"CUSTODIAN"};
    char party_id_source{'D'};
    int party_role{3};
};

struct CancelOrderFields {
    std::string cl_ord_id;
    std::string orig_cl_ord_id;
    std::string account{"ACC001"};
    std::string security_id{"US0231351067"};
    char security_id_source{'4'};
    double order_qty{100.0};
    char side{'1'};
    std::string symbol{"AAPL"};
    std::string transact_time;
    std::string ex_destination{"XNYS"};
    std::string security_exchange{"XNYS"};
    std::string party_id{"CUSTODIAN"};
    char party_id_source{'D'};
    int party_role{3};
};

struct ParsedAck {
    std::string cl_ord_id;
    char exec_type{'\0'};
    char ord_status{'\0'};
    double leaves_qty{0.0};
    double cum_qty{0.0};
    double avg_px{0.0};
};

// ---------------------------------------------------------------------------
// Timestamp
// ---------------------------------------------------------------------------

auto NowFixTimestamp() -> std::string {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::gmtime_r(&t, &tm);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d.000",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

auto NowNs() -> std::uint64_t {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
        .count());
}

// ---------------------------------------------------------------------------
// FIX message builders – application struct → FIX library message
// ---------------------------------------------------------------------------

auto BuildNewOrder(const NewOrderFields& req) -> fastfix::message::Message {
    fastfix::message::MessageBuilder b{"D"};
    b.reserve_fields(17U).reserve_groups(1U).reserve_group_entries(453U, 1U);
    b.set(1U,   req.account)
     .set(11U,  req.cl_ord_id)
     .set(15U,  req.currency)
     .set(18U,  req.exec_inst)
     .set(21U,  req.hand_l_inst)
     .set(22U,  req.security_id_source)
     .set(38U,  req.order_qty)
     .set(40U,  req.ord_type)
     .set(44U,  req.price)
     .set(48U,  req.security_id)
     .set(54U,  req.side)
     .set(55U,  req.symbol)
     .set(59U,  req.time_in_force)
     .set(60U,  req.transact_time)
     .set(100U, req.ex_destination)
     .set(207U, req.security_exchange);
    b.add_group_entry(453U)
     .set(448U, req.party_id)
     .set(447U, req.party_id_source)
     .set(452U, static_cast<std::int64_t>(req.party_role));
    return std::move(b).build();
}

auto BuildCancelOrder(const CancelOrderFields& req) -> fastfix::message::Message {
    fastfix::message::MessageBuilder b{"F"};
    b.reserve_fields(12U).reserve_groups(1U).reserve_group_entries(453U, 1U);
    b.set(1U,   req.account)
     .set(11U,  req.cl_ord_id)
     .set(22U,  req.security_id_source)
     .set(38U,  req.order_qty)
     .set(41U,  req.orig_cl_ord_id)
     .set(48U,  req.security_id)
     .set(54U,  req.side)
     .set(55U,  req.symbol)
     .set(60U,  req.transact_time)
     .set(100U, req.ex_destination)
     .set(207U, req.security_exchange);
    b.add_group_entry(453U)
     .set(448U, req.party_id)
     .set(447U, req.party_id_source)
     .set(452U, static_cast<std::int64_t>(req.party_role));
    return std::move(b).build();
}

// ---------------------------------------------------------------------------
// FIX response builders (acceptor side)
// ---------------------------------------------------------------------------

auto BuildExecReportNew(fastfix::message::MessageView order, std::uint32_t exec_id) -> fastfix::message::Message {
    const auto cl_ord_id  = order.get_string(11U).value_or("UNKNOWN");
    const auto side       = order.get_char(54U).value_or('1');
    const auto order_qty  = order.get_float(38U).value_or(100.0);
    const auto symbol     = order.get_string(55U).value_or("");
    const auto account    = order.get_string(1U).value_or("");

    fastfix::message::MessageBuilder b{"8"};
    b.reserve_fields(13U);
    b.set(1U,   account)
     .set(6U,   0.0)
     .set(11U,  cl_ord_id)
     .set(14U,  0.0)
     .set(17U,  std::string("E") + std::to_string(exec_id))
     .set(37U,  std::string("O") + std::to_string(exec_id))
     .set(38U,  order_qty)
     .set(39U,  '0')
     .set(54U,  side)
     .set(55U,  symbol)
     .set(60U,  NowFixTimestamp())
     .set(150U, '0')
     .set(151U, order_qty);
    return std::move(b).build();
}

auto BuildExecReportCanceled(fastfix::message::MessageView cancel_req, std::uint32_t exec_id) -> fastfix::message::Message {
    const auto cl_ord_id       = cancel_req.get_string(11U).value_or("UNKNOWN");
    const auto orig_cl_ord_id  = cancel_req.get_string(41U).value_or("");
    const auto side            = cancel_req.get_char(54U).value_or('1');
    const auto order_qty       = cancel_req.get_float(38U).value_or(100.0);
    const auto symbol          = cancel_req.get_string(55U).value_or("");
    const auto account         = cancel_req.get_string(1U).value_or("");

    fastfix::message::MessageBuilder b{"8"};
    b.reserve_fields(14U);
    b.set(1U,   account)
     .set(6U,   150.25)
     .set(11U,  cl_ord_id)
     .set(14U,  order_qty)
     .set(17U,  std::string("E") + std::to_string(exec_id))
     .set(37U,  std::string("O") + std::to_string(exec_id))
     .set(38U,  order_qty)
     .set(39U,  '4')
     .set(41U,  orig_cl_ord_id)
     .set(54U,  side)
     .set(55U,  symbol)
     .set(60U,  NowFixTimestamp())
     .set(150U, '4')
     .set(151U, 0.0);
    return std::move(b).build();
}

// ---------------------------------------------------------------------------
// Ack parser – FIX ExecutionReport → application struct
// ---------------------------------------------------------------------------

auto ParseExecReport(fastfix::message::MessageView view) -> ParsedAck {
    ParsedAck ack;
    if (auto v = view.get_string(11U)) { ack.cl_ord_id = std::string(*v); }
    if (auto v = view.get_char(150U))  { ack.exec_type  = *v; }
    if (auto v = view.get_char(39U))   { ack.ord_status = *v; }
    if (auto v = view.get_float(151U)) { ack.leaves_qty = *v; }
    if (auto v = view.get_float(14U))  { ack.cum_qty    = *v; }
    if (auto v = view.get_float(6U))   { ack.avg_px     = *v; }
    return ack;
}

// ---------------------------------------------------------------------------
// CommonAdminProtocolConfig
// ---------------------------------------------------------------------------

constexpr std::string_view kBeginString     = "FIX.4.4";
constexpr std::string_view kAcceptorCompId  = "SELL";
constexpr std::string_view kInitiatorCompId = "BUY";

auto MakeAcceptorProtocolConfig(std::uint32_t profile_id) -> fastfix::session::AdminProtocolConfig {
    return fastfix::session::AdminProtocolConfig{
        .session = fastfix::session::SessionConfig{
            .session_id = 1U,
            .key = fastfix::session::SessionKey{
                std::string(kBeginString),
                std::string(kAcceptorCompId),
                std::string(kInitiatorCompId)},
            .profile_id = profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
        },
        .begin_string          = std::string(kBeginString),
        .sender_comp_id        = std::string(kAcceptorCompId),
        .target_comp_id        = std::string(kInitiatorCompId),
        .heartbeat_interval_seconds = 30U,
        .validation_policy = fastfix::session::ValidationPolicy::Compatible(),
    };
}

auto MakeInitiatorProtocolConfig(std::uint32_t profile_id) -> fastfix::session::AdminProtocolConfig {
    return fastfix::session::AdminProtocolConfig{
        .session = fastfix::session::SessionConfig{
            .session_id = 2U,
            .key = fastfix::session::SessionKey{
                std::string(kBeginString),
                std::string(kInitiatorCompId),
                std::string(kAcceptorCompId)},
            .profile_id = profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = true,
        },
        .begin_string          = std::string(kBeginString),
        .sender_comp_id        = std::string(kInitiatorCompId),
        .target_comp_id        = std::string(kAcceptorCompId),
        .heartbeat_interval_seconds = 30U,
        .validation_policy = fastfix::session::ValidationPolicy::Compatible(),
    };
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct LatencyStats {
    std::size_t  count{0};
    std::uint64_t min_ns{0};
    std::uint64_t p50_ns{0};
    std::uint64_t p95_ns{0};
    std::uint64_t p99_ns{0};
    std::uint64_t p999_ns{0};
    std::uint64_t max_ns{0};
    double        avg_ns{0.0};
};

auto ComputeStats(std::vector<std::uint64_t> samples) -> LatencyStats {
    if (samples.empty()) return {};
    std::sort(samples.begin(), samples.end());
    const auto n = samples.size();
    LatencyStats s;
    s.count   = n;
    s.min_ns  = samples.front();
    s.max_ns  = samples.back();
    s.p50_ns  = samples[n * 50 / 100];
    s.p95_ns  = samples[n * 95 / 100];
    s.p99_ns  = samples[n * 99 / 100];
    s.p999_ns = samples[std::max(std::size_t{0}, n * 999 / 1000)];
    double sum = 0.0;
    for (auto v : samples) sum += static_cast<double>(v);
    s.avg_ns  = sum / static_cast<double>(n);
    return s;
}

auto PrintStats(std::string_view label, const LatencyStats& s) -> void {
    if (s.count == 0) {
        std::printf("  %-30s  no data\n", std::string(label).c_str());
        return;
    }
    std::printf("  %-30s  count=%5zu  avg=%8.0f ns  p50=%8.0f ns  p95=%8.0f ns  p99=%8.0f ns  p999=%8.0f ns\n",
        std::string(label).c_str(),
        s.count,
        s.avg_ns,
        static_cast<double>(s.p50_ns),
        static_cast<double>(s.p95_ns),
        static_cast<double>(s.p99_ns),
        static_cast<double>(s.p999_ns));
}

// ---------------------------------------------------------------------------
// Acceptor process (child)
// ---------------------------------------------------------------------------

[[noreturn]] void RunAcceptor(
    int port_write_fd,
    const fastfix::profile::NormalizedDictionaryView& dictionary,
    std::uint32_t iterations,
    std::uint32_t warmup) {

    pin_to_core(1);  // Acceptor on core 1

    auto listener = fastfix::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
    if (!listener.ok()) {
        std::fprintf(stderr, "[acceptor] Listen failed: %s\n",
            std::string(listener.status().message()).c_str());
        _exit(1);
    }

    const std::uint16_t port = listener.value().port();
    if (::write(port_write_fd, &port, sizeof(port)) != sizeof(port)) {
        std::fprintf(stderr, "[acceptor] pipe write failed\n");
        _exit(1);
    }
    ::close(port_write_fd);

    // Reserve storage proportional to the actual message count (2 messages per pair,
    // plus a small admin overhead).
    const std::uint32_t total_pairs = warmup + iterations;
    const std::size_t   n_msgs      = 2U * total_pairs + 16U;
    const std::size_t   arena_bytes = n_msgs * 512U + 4096U;

    fastfix::store::MemorySessionStore store;
    store.ReserveAdditionalSessionStorage(1U, n_msgs, n_msgs, arena_bytes);
    fastfix::session::AdminProtocol protocol{
        MakeAcceptorProtocolConfig(dictionary.profile().header().profile_id),
        dictionary,
        &store};

    auto conn = listener.value().Accept(std::chrono::seconds(30));
    if (!conn.ok()) {
        std::fprintf(stderr, "[acceptor] Accept failed: %s\n",
            std::string(conn.status().message()).c_str());
        _exit(1);
    }

    auto start_event = protocol.OnTransportConnected(NowNs());
    if (!start_event.ok()) {
        _exit(1);
    }
    for (const auto& frame : start_event.value().outbound_frames) {
        conn.value().Send(frame.bytes, std::chrono::seconds(5));
    }

    std::uint32_t exec_id = 0U;
    while (true) {
        auto raw_frame = conn.value().ReceiveFrameView(std::chrono::seconds(30));
        if (!raw_frame.ok()) break;

        auto decoded = fastfix::codec::DecodeFixMessageView(raw_frame.value(), dictionary);
        if (!decoded.ok()) continue;

        auto event = protocol.OnInbound(decoded.value(), NowNs());
        if (!event.ok()) break;

        for (const auto& outbound : event.value().outbound_frames) {
            conn.value().Send(outbound.bytes, std::chrono::seconds(5));
        }

        for (const auto& app : event.value().application_messages) {
            const auto msg_type = app.view().msg_type();
            ++exec_id;
            fastfix::message::Message ack;
            if (msg_type == "D") {
                ack = BuildExecReportNew(app.view(), exec_id);
            } else if (msg_type == "F") {
                ack = BuildExecReportCanceled(app.view(), exec_id);
            } else {
                continue;
            }
            auto encoded = protocol.SendApplication(ack, NowNs());
            if (encoded.ok()) {
                conn.value().Send(encoded.value().bytes, std::chrono::seconds(5));
            }
        }

        if (event.value().disconnect) {
            protocol.OnTransportClosed();
            break;
        }
    }
    _exit(0);
}

// ---------------------------------------------------------------------------
// Initiator process (parent): async two-thread model
//
// Sender (main thread):   D → sleep(interval) → F → sleep(interval) → D → ...
//                         Records T0 (before struct→FIX construction) in a
//                         shared pending map.  Never waits for acks.
// Receiver (new thread):  independently drains TCP; stamps T1 on each arriving
//                         ExecutionReport, looks up T0 by ClOrdID, records RTT.
//
// proto_send_mu serialises AdminProtocol state and all conn.BusySend calls so
// that admin responses from the receiver don't interleave with app sends.
// ---------------------------------------------------------------------------

struct LoopbackResult {
    std::vector<std::uint64_t> nos_rtts;
    std::vector<std::uint64_t> ocr_rtts;
    std::uint64_t wall_time_ns{0};  // total elapsed from first send to last ack
};

auto RunInitiator(
    std::uint16_t port,
    const fastfix::profile::NormalizedDictionaryView& dictionary,
    std::uint32_t iterations,
    std::uint32_t warmup,
    std::uint32_t send_interval_us) -> LoopbackResult {

    auto conn = fastfix::transport::TcpConnection::Connect(
        "127.0.0.1", port, std::chrono::seconds(10));
    if (!conn.ok()) {
        std::fprintf(stderr, "[initiator] Connect failed: %s\n",
            std::string(conn.status().message()).c_str());
        return {};
    }

    const std::uint32_t total_pairs = warmup + iterations;
    fastfix::store::MemorySessionStore store;
    store.ReserveAdditionalSessionStorage(2U, 2U * total_pairs + 32U, 2U * total_pairs + 32U,
        2U * total_pairs * 1024U);

    fastfix::session::AdminProtocol protocol{
        MakeInitiatorProtocolConfig(dictionary.profile().header().profile_id),
        dictionary,
        &store};

    // --- Logon ---
    auto logon_event = protocol.OnTransportConnected(NowNs());
    if (!logon_event.ok()) { return {}; }
    for (const auto& frame : logon_event.value().outbound_frames) {
        conn.value().Send(frame.bytes, std::chrono::seconds(5));
    }
    bool session_active = false;
    while (!session_active) {
        auto raw = conn.value().BusyReceiveFrameView(std::chrono::seconds(10));
        if (!raw.ok()) { return {}; }
        auto decoded = fastfix::codec::DecodeFixMessageView(raw.value(), dictionary);
        if (!decoded.ok()) { return {}; }
        auto ev = protocol.OnInbound(decoded.value(), NowNs());
        if (!ev.ok()) { return {}; }
        for (const auto& outbound : ev.value().outbound_frames) {
            conn.value().Send(outbound.bytes, std::chrono::seconds(5));
        }
        session_active = ev.value().session_active;
    }

    // ClOrdID helpers
    auto make_nos_id = [](std::uint32_t i) -> std::string {
        char buf[12];
        std::snprintf(buf, sizeof(buf), "N%07u", i + 1U);
        return {buf};
    };
    auto make_ocr_id = [](std::uint32_t i) -> std::string {
        char buf[12];
        std::snprintf(buf, sizeof(buf), "C%07u", i + 1U);
        return {buf};
    };

    const std::uint32_t total_msgs = 2U * total_pairs;

    // Send queue: main thread pushes built messages; session thread drains and sends.
    struct SendRequest {
        fastfix::message::Message msg;
        std::string cl_ord_id;
        bool is_cancel;
        std::chrono::steady_clock::time_point t0;  // recorded before construction in sender
    };
    std::deque<SendRequest> send_queue;
    std::mutex send_queue_mu;

    // Results written exclusively by session thread; read by main thread after join().
    std::vector<std::uint64_t> nos_rtts, ocr_rtts;
    nos_rtts.reserve(total_pairs);
    ocr_rtts.reserve(total_pairs);

    const auto wall_start = std::chrono::steady_clock::now();

    // --- Session thread: sole owner of protocol state, conn reads, conn writes ---
    // No mutex needed here — protocol and conn are touched only by this thread.
    std::thread session_th([&] {
        // Per-thread pending map: ClOrdID → T0
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> pending;
        pending.reserve(total_msgs);
        std::uint32_t acks_done = 0;

        while (acks_done < total_msgs) {
            // 1. Drain send queue: grab batch, release lock, then call protocol + send.
            std::vector<SendRequest> batch;
            {
                std::lock_guard<std::mutex> lk(send_queue_mu);
                while (!send_queue.empty()) {
                    batch.push_back(std::move(send_queue.front()));
                    send_queue.pop_front();
                }
            }
            for (auto& req : batch) {
                pending[req.cl_ord_id] = req.t0;
                auto outbound = protocol.SendApplication(req.msg, NowNs());
                if (outbound.ok()) {
                    conn.value().BusySend(outbound.value().bytes, std::chrono::seconds(5));
                }
            }

            // 2. Try to receive one inbound frame (non-blocking).
            auto raw = conn.value().TryReceiveFrameView();
            if (!raw.ok()) break;
            if (!raw.value().has_value()) continue;

            auto decoded = fastfix::codec::DecodeFixMessageView(*raw.value(), dictionary);
            if (!decoded.ok()) continue;

            auto ev = protocol.OnInbound(decoded.value(), NowNs());
            if (!ev.ok()) break;

            // Admin responses (heartbeats, test requests, etc.) — no lock needed.
            for (const auto& outbound : ev.value().outbound_frames) {
                conn.value().BusySend(outbound.bytes, std::chrono::seconds(5));
            }

            // Application acks.
            for (const auto& app : ev.value().application_messages) {
                if (app.view().msg_type() != "8") continue;
                const auto ack = ParseExecReport(app.view());
                const auto t1 = std::chrono::steady_clock::now();  // T1: after ack→struct extraction
                auto it = pending.find(ack.cl_ord_id);
                if (it == pending.end()) continue;
                const auto rtt_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        t1 - it->second).count());
                const bool is_cancel = (!ack.cl_ord_id.empty() && ack.cl_ord_id[0] == 'C');
                pending.erase(it);
                if (is_cancel) ocr_rtts.push_back(rtt_ns);
                else           nos_rtts.push_back(rtt_ns);
                ++acks_done;
            }

            if (ev.value().disconnect) break;
        }
    });

    // --- Sender (main thread): builds messages, pushes to queue, sleeps between ---
    const NewOrderFields nos_template;
    for (std::uint32_t i = 0; i < total_pairs; ++i) {
        const auto ts = NowFixTimestamp();

        // 35=D  NewOrderSingle
        {
            NewOrderFields nos = nos_template;
            nos.cl_ord_id     = make_nos_id(i);
            nos.transact_time = ts;
            const auto t0 = std::chrono::steady_clock::now();  // T0: before construction
            auto msg = BuildNewOrder(nos);
            std::lock_guard<std::mutex> lk(send_queue_mu);
            send_queue.push_back({std::move(msg), nos.cl_ord_id, false, t0});
        }

        if (send_interval_us > 0) {
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::microseconds(send_interval_us);
            while (std::chrono::steady_clock::now() < deadline) {}
        }

        // 35=F  OrderCancelRequest
        {
            CancelOrderFields ocr;
            ocr.cl_ord_id      = make_ocr_id(i);
            ocr.orig_cl_ord_id = make_nos_id(i);
            ocr.transact_time  = ts;
            const auto t0 = std::chrono::steady_clock::now();  // T0: before construction
            auto msg = BuildCancelOrder(ocr);
            std::lock_guard<std::mutex> lk(send_queue_mu);
            send_queue.push_back({std::move(msg), ocr.cl_ord_id, true, t0});
        }

        if (send_interval_us > 0) {
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::microseconds(send_interval_us);
            while (std::chrono::steady_clock::now() < deadline) {}
        }
    }

    session_th.join();
    const auto wall_end = std::chrono::steady_clock::now();

    // Graceful logout — session thread has exited, safe to call from main thread.
    {
        auto logout = protocol.BeginLogout({}, NowNs());
        if (logout.ok()) {
            conn.value().BusySend(logout.value().bytes, std::chrono::seconds(5));
        }
    }
    conn.value().Close();

    // Collect results (skip first 'warmup' samples of each type).
    // No lock needed: session thread has joined.
    LoopbackResult result;
    result.wall_time_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start).count());
    if (nos_rtts.size() > warmup) {
        result.nos_rtts.assign(nos_rtts.begin() + static_cast<std::ptrdiff_t>(warmup),
                               nos_rtts.end());
    }
    if (ocr_rtts.size() > warmup) {
        result.ocr_rtts.assign(ocr_rtts.begin() + static_cast<std::ptrdiff_t>(warmup),
                               ocr_rtts.end());
    }
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// operator new overrides for allocation tracking
// ---------------------------------------------------------------------------

void* operator new(std::size_t size) {
    auto* p = bench_support::AllocateRaw(size, alignof(std::max_align_t));
    if (!p) throw std::bad_alloc();
    bench_support::RecordAllocation(size);
    return p;
}
void* operator new[](std::size_t size) {
    auto* p = bench_support::AllocateRaw(size, alignof(std::max_align_t));
    if (!p) throw std::bad_alloc();
    bench_support::RecordAllocation(size);
    return p;
}
void* operator new(std::size_t s, std::align_val_t a) {
    auto* p = bench_support::AllocateRaw(s, static_cast<std::size_t>(a));
    if (!p) throw std::bad_alloc();
    bench_support::RecordAllocation(s);
    return p;
}
void* operator new[](std::size_t s, std::align_val_t a) {
    auto* p = bench_support::AllocateRaw(s, static_cast<std::size_t>(a));
    if (!p) throw std::bad_alloc();
    bench_support::RecordAllocation(s);
    return p;
}
void* operator new(std::size_t s, const std::nothrow_t&) noexcept {
    auto* p = bench_support::AllocateRaw(s, alignof(std::max_align_t));
    if (p) bench_support::RecordAllocation(s);
    return p;
}
void* operator new[](std::size_t s, const std::nothrow_t&) noexcept {
    auto* p = bench_support::AllocateRaw(s, alignof(std::max_align_t));
    if (p) bench_support::RecordAllocation(s);
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    std::filesystem::path artifact_path;
    std::uint32_t iterations       = 1000U;
    std::uint32_t warmup           = 50U;
    std::uint32_t send_interval_us = 0U;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--artifact" && i + 1 < argc) {
            artifact_path = argv[++i];
        } else if (arg == "--iterations" && i + 1 < argc) {
            iterations = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--warmup" && i + 1 < argc) {
            warmup = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--send-interval-us" && i + 1 < argc) {
            send_interval_us = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else {
            std::fprintf(stderr,
                "usage: fastfix-loopback --artifact <path> [--iterations N] [--warmup N] [--send-interval-us N]\n");
            return 1;
        }
    }

    if (artifact_path.empty()) {
        std::fprintf(stderr,
            "usage: fastfix-loopback --artifact <path> [--iterations N] [--warmup N] [--send-interval-us N]\n");
        return 1;
    }

    // Load dictionary before fork so both processes share a warm read-only mapping.
    auto profile = fastfix::profile::LoadProfileArtifact(artifact_path);
    if (!profile.ok()) {
        std::fprintf(stderr, "error: failed to load artifact '%s': %s\n",
            artifact_path.c_str(), std::string(profile.status().message()).c_str());
        return 1;
    }
    auto dictionary = fastfix::profile::NormalizedDictionaryView::FromProfile(std::move(profile).value());
    if (!dictionary.ok()) {
        std::fprintf(stderr, "error: failed to build dictionary view\n");
        return 1;
    }

    // Pipe: acceptor writes bound port, initiator reads it.
    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) { std::perror("pipe"); return 1; }

    const pid_t pid = ::fork();
    if (pid < 0) { std::perror("fork"); return 1; }

    if (pid == 0) {
        // Child – acceptor
        ::close(pipe_fds[0]);
        RunAcceptor(pipe_fds[1], dictionary.value(), iterations, warmup);
        // RunAcceptor is [[noreturn]]
    }

    // Parent – initiator
    pin_to_core(0);  // Initiator on core 0
    ::close(pipe_fds[1]);

    std::uint16_t port = 0;
    if (::read(pipe_fds[0], &port, sizeof(port)) != sizeof(port)) {
        std::fprintf(stderr, "initiator: failed to read acceptor port\n");
        ::kill(pid, SIGTERM);
        return 1;
    }
    ::close(pipe_fds[0]);

    // Brief pause to let the acceptor call Accept().
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::printf("FastFix two-process loopback: %u iterations per message type, %u warmup  send-interval=%u us\n",
        iterations, warmup, send_interval_us);
    std::printf("  Messages: 35=D (NewOrderSingle, %d fields + NoPartyIDs) "
                "and 35=F (OrderCancelRequest, %d fields + NoPartyIDs)\n", 16, 11);
    std::printf("  Mode: %s\n",
        send_interval_us > 0 ? "rate-limited (send pair → wait acks → sleep interval)"
                             : "burst-send all pairs then pipeline-drain acks");
    std::printf("  T0 = before struct→FIX build   T1 = after ack→struct extraction\n\n");

    const auto result = RunInitiator(port, dictionary.value(), iterations, warmup, send_interval_us);

    // Wait for acceptor child
    int child_status = 0;
    ::waitpid(pid, &child_status, 0);

    if (result.nos_rtts.empty() && result.ocr_rtts.empty()) {
        std::fprintf(stderr, "error: no timing results collected\n");
        return 1;
    }

    auto combined = result.nos_rtts;
    combined.insert(combined.end(), result.ocr_rtts.begin(), result.ocr_rtts.end());

    std::printf("=== FastFix two-process loopback latency ===\n");
    PrintStats("35=D  NewOrderSingle",     ComputeStats(result.nos_rtts));
    PrintStats("35=F  OrderCancelRequest", ComputeStats(result.ocr_rtts));
    PrintStats("Combined",               ComputeStats(combined));
    std::printf("  Total wall time: %.3f ms  (%zu messages, send-interval=%u us)\n",
        static_cast<double>(result.wall_time_ns) / 1e6,
        result.nos_rtts.size() + result.ocr_rtts.size(),
        send_interval_us);
    std::printf("\n");

    return 0;
}
