#include <nimblefix/codec.hpp>
#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/direct_tls_transport.hpp>
#include <nimblefix/engine.hpp>
#include <nimblefix/fix44_sample.hpp>
#include <nimblefix/journal.hpp>
#include <nimblefix/protocol.hpp>
#include <nimblefix/replay.hpp>
#include <nimblefix/sharded_engine.hpp>
#include <nimblefix/telemetry.hpp>
#include <nimblefix/tls.hpp>

#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44_sample;

namespace {

int g_failures = 0;

void fail_check(const char* expr, int line) {
    std::cerr << "FAIL at " << __FILE__ << ':' << line << ": " << expr << '\n';
    ++g_failures;
}

#define CHECK(expr) do { if (!(expr)) fail_check(#expr, __LINE__); } while (0)

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(std::string_view tag) {
        static std::atomic<int> seq{0};
        path = std::filesystem::temp_directory_path() /
               ("nimblefix_doc_" + std::string{tag} + "_" + std::to_string(::getpid()) + "_" +
                std::to_string(seq.fetch_add(1)));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

template <class Pred>
bool wait_until(Pred&& pred, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

// Compile- and runtime-verify Section 2 snippets (StreamWriter, w.header(), std::optional, typed views).
void test_codec_and_view_snippets() {
    const bool is_limit = true;
    const std::optional<nf::Decimal> opt_px =
        is_limit ? std::optional{nf::Decimal{15025, 2}} : std::nullopt;

    auto write_order = [&](auto& w) {
        w.header().sender_sub_id("DESK-A").target_sub_id("EXCH-1");
        w.cl_ord_id("ORD-0001")
         .symbol("AAPL")
         .side('1')
         .transact_time("20261125-09:30:00.000")
         .order_qty(nf::Decimal{100, 0})
         .ord_type(is_limit ? '2' : '1')
         .price(opt_px);
    };

    std::array<char, 512> wire_buf{};
    const nf::SessionHeader hdr{"BUY", "SELL", 1, "20261125-09:30:00.000"};
    const auto encoded = nf::encode_stream<fix::NewOrderSingle>(hdr, wire_buf, write_order);
    CHECK(encoded.ok());

    nf::FrameView fv{};
    std::array<nf::FieldView, 32> scratch{};
    nf::FlatMessageView msg{};
    CHECK(nf::inspect_frame(std::string_view{wire_buf.data(), encoded.value()}, fv).ok());
    CHECK(nf::parse_flat_verified(fv, scratch, msg).ok());
    CHECK(msg.is<fix::NewOrderSingle>());
    const auto nos = msg.as<fix::NewOrderSingle>();
    CHECK(nos.cl_ord_id().ok() && nos.cl_ord_id().value() == "ORD-0001");
    CHECK(nos.price().ok() && nos.price().value() == (nf::Decimal{15025, 2}));
}

// Compile- and runtime-verify Scenarios A, B, C, D + Section 6 (TLS SocketOptions, Failover,
// TelemetryReader, JournalTailer, JournalReplayer) + Section 7 (use_schema, UnexpectedFieldPolicy, Admin APIs).
void test_runtime_and_operational_snippets() {
    TempDir dir("snippets");
    const auto tel_path = dir.path / "exchange.nfxt";

    // Scenario D: ShardedEngineConfig + IoBackendConfig::idle_strategy + shared telemetry + SocketOptions
    nf::ShardedEngineConfig scfg{};
    scfg.shard_count = 2;
    scfg.base.storage.directory = dir.path / "exchange";
    scfg.base.storage.journal_capacity_bytes = 512 * 1024;
    scfg.base.storage.journal_max_records = 2048;
    scfg.base.storage.segment_bytes = 128 * 1024;
    scfg.base.io_backend = nf::IoBackendConfig::epoll();
    scfg.base.io_backend.idle_strategy = nf::IdleStrategy::hybrid_backoff;
    scfg.base.telemetry_path = tel_path;
    scfg.base.telemetry_stage_probes = true;

    auto server = nf::ShardedEngine<fix::Schema, nf::Inline>::create(scfg).value();
    std::atomic<int> server_orders{0};

    // 3-arg typed callback with DeliveryInfo
    CHECK(server.on<fix::NewOrderSingle>(
        [&](nf::SessionContext& ctx, const fix::NewOrderSingleView& req, const nf::DeliveryInfo& info) -> nf::Status {
            (void)info;
            server_orders.fetch_add(1, std::memory_order_relaxed);
            return ctx.send_stream<fix::ExecutionReport>([&](auto& w) {
                w.order_id("ORD-1").exec_id("EX-1")
                 .cl_ord_id(req.cl_ord_id().value())
                 .exec_type('0').ord_status('0')
                 .symbol(req.symbol().value()).side(req.side().value())
                 .leaves_qty(req.order_qty().value())
                 .cum_qty(nf::Decimal{0, 0}).avg_px(nf::Decimal{0, 0});
            }).status();
        }).ok());
    CHECK(server.ignore<fix::ExecutionReport>("outbound only").ok());

    nf::ListenerConfig lcfg{.name = "main", .endpoint = {"127.0.0.1", 0}};
    lcfg.socket_options.tcp_quickack = true;
    CHECK(server.add_listener(lcfg).ok());

    nf::AcceptorConfig acc_direct{.name = "client-direct", .listener = "main", .local_comp_id = "EXCH", .remote_comp_id = "DIRECT_BUY"};
    acc_direct.settings.check_sending_time = false;
    acc_direct.settings.authentication.method = nf::AuthenticationMethod::callback;
    acc_direct.settings.authentication.require_tls = false;
    acc_direct.settings.authentication.verify = [](const nf::FlatMessageView&) noexcept {
        return nf::Status::success();
    };
    CHECK(server.add_acceptor(acc_direct).ok());

    nf::AcceptorConfig acc_gw{.name = "client-gw", .listener = "main", .local_comp_id = "EXCH", .remote_comp_id = "GW_BUY"};
    acc_gw.settings.check_sending_time = false;
    CHECK(server.add_acceptor(acc_gw).ok());

    std::thread server_thread([&] { (void)server.run(); });
    nf::TcpEndpoint ep{};
    CHECK(wait_until([&] {
        const auto r = server.listener_endpoint("main");
        if (r.ok()) { ep = r.value(); return true; }
        return false;
    }));

    // Scenario A + B: DirectInitiator + enqueue_stream/flush + DirectSendQueue + drain
    struct TradingHandler {
        int fills{0};
        nf::Status on_message(const nf::FlatMessageView& msg, const nf::DeliveryInfo& info) noexcept {
            (void)info;
            if (msg.is<fix::ExecutionReport>()) {
                const auto er = msg.as<fix::ExecutionReport>();
                if (er.exec_id().ok()) ++fills;
            }
            return nf::Status::success();
        }
    };

    nf::DirectInitiatorConfig dcfg{};
    dcfg.endpoints = {{"127.0.0.1", ep.port}};
    dcfg.connection_strategy = nf::ConnectionStrategy::ordered(50, 1000, 2, 0, true, true);
    dcfg.socket_options = nf::SocketOptions{.local_bind_ip = "127.0.0.1", .tcp_quickack = true};
    dcfg.storage_directory = (dir.path / "direct_buy").string();
    dcfg.local_comp_id = "DIRECT_BUY";
    dcfg.remote_comp_id = "EXCH";
    dcfg.timestamp_precision = nf::TimestampPrecision::microseconds;
    dcfg.check_sending_time = false;

    auto init = nf::DirectInitiator<fix::Schema, TradingHandler>::open(dcfg).value();
    CHECK(init.connect().ok());
    CHECK(wait_until([&] {
        (void)init.poll_once();
        (void)init.reconnect_if_due();
        return init.is_active();
    }));

    CHECK(init.send_stream<fix::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("ORD-1").symbol("AAPL").side('1')
         .transact_time("20261125-09:30:00.000000")
         .order_qty(nf::Decimal{100, 0}).ord_type('1');
    }).ok());

    for (int i = 0; i < 2; ++i) {
        CHECK(init.enqueue_stream<fix::NewOrderSingle>([&](auto& w) {
            w.cl_ord_id("BURST").symbol("AAPL").side('1')
             .transact_time("20261125-09:30:00.000000")
             .order_qty(nf::Decimal{10, 0}).ord_type('1');
        }).ok());
    }
    CHECK(init.flush().ok());

    nf::DirectSendQueue send_queue{32, 256};
    CHECK(send_queue.try_push_stream<fix::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("STRAT-1").symbol("AAPL").side('1')
         .transact_time("20261125-09:30:00.000")
         .order_qty(nf::Decimal{50, 0}).ord_type('1');
    }).ok());
    CHECK(init.drain(send_queue).ok());

    CHECK(wait_until([&] {
        (void)init.poll_once();
        (void)init.poll_maintenance();
        return init.handler().fills == 4;
    }));
    CHECK(init.logout("shutdown").ok());
    for (int i = 0; i < 30 && init.valid_transport(); ++i) (void)init.poll_once();

    // Scenario C + Section 7.2/7.3: Engine<FlatSchema> + start()/poll_once()/running()/request_stop()
    // + InitiatorConfig + Sender::receipt + ProducerLane::receipt + use_schema + admin controls
    nf::EngineConfig ecfg{};
    ecfg.storage.directory = dir.path / "gateway";
    ecfg.storage.journal_capacity_bytes = 512 * 1024;
    ecfg.storage.journal_max_records = 2048;
    ecfg.storage.segment_bytes = 128 * 1024;
    ecfg.io_backend = nf::IoBackendConfig::epoll();
    ecfg.io_backend.idle_strategy = nf::IdleStrategy::sleep;

    auto gw = nf::Engine<nf::FlatSchema, nf::Inline>::create(ecfg).value();
    std::atomic<int> gw_fills{0};
    CHECK(gw.on_message([&](nf::SessionContext&, const nf::FlatMessageView& m) {
        if (m.is<fix::ExecutionReport>()) gw_fills.fetch_add(1, std::memory_order_relaxed);
        return nf::Status::success();
    }).ok());

    nf::InitiatorConfig icfg{.name = "venue-exch", .sender_comp_id = "GW_BUY", .target_comp_id = "EXCH", .endpoint = ep};
    icfg.settings.use_schema<fix::Schema>();
    icfg.settings.unexpected_field_policy = nf::UnexpectedFieldPolicy::ignore_with_metric;
    icfg.settings.allow_sequence_reset = true;
    icfg.settings.check_sending_time = false;
    icfg.settings.schedule.enabled = true;
    icfg.settings.schedule.reset_on_schedule_boundary = true;

    auto handle = gw.add_initiator(icfg).value();
    auto mpsc_sender = handle.sender<fix::Schema>();
    auto spsc_lane = handle.producer_lane<fix::Schema>().value();

    // Drive manual event loop (start + running + poll_once + request_stop)
    CHECK(gw.start().ok());
    const auto active_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (gw.running() && std::chrono::steady_clock::now() < active_deadline) {
        (void)gw.poll_once(2);
        const auto snap = gw.snapshot("venue-exch");
        if (snap.ok() && snap.value().state == nf::SessionState::active) break;
    }

    const auto send_rec = mpsc_sender.send_stream<fix::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("MPSC-1").symbol("AAPL").side('1')
         .transact_time("20261125-09:30:00.000").order_qty(nf::Decimal{10, 0}).ord_type('1');
    }).value();
    const auto lane_rec = spsc_lane.send_stream<fix::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("SPSC-1").symbol("AAPL").side('1')
         .transact_time("20261125-09:30:00.000").order_qty(nf::Decimal{20, 0}).ord_type('1');
    }).value();

    const auto fill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (gw.running() && gw_fills.load(std::memory_order_relaxed) < 2 &&
           std::chrono::steady_clock::now() < fill_deadline) {
        (void)gw.poll_once(2);
    }
    CHECK(gw_fills.load(std::memory_order_relaxed) == 2);
    CHECK(mpsc_sender.receipt(send_rec.id).ok());
    CHECK(spsc_lane.receipt(lane_rec.id).ok());

    // Runtime admin controls
    CHECK(gw.request_logout("venue-exch", "maintenance").ok());
    CHECK(gw.request_sequence_reset("venue-exch").ok());
    CHECK(gw.request_reconnect("venue-exch").ok());
    gw.request_stop();
    while (gw.running()) (void)gw.poll_once(2);
    CHECK(gw.adjust_sequences("venue-exch", 10, 50).ok());

    server.request_stop();
    server_thread.join();
    CHECK(server_orders.load(std::memory_order_relaxed) == 6);

    // Section 6.4: TelemetryReader::read_all(), stages[...], unexpected_fields, format_prometheus()
    auto reader = nf::TelemetryReader::open(tel_path).value();
    const auto snaps = reader.read_all();
    CHECK(!snaps.empty());
    std::uint64_t total_rx = 0;
    for (const auto& s : snaps) {
        const auto& parse_lat = s.stages[static_cast<std::size_t>(nf::TelemetryStage::parse)];
        (void)parse_lat.p99_ns;
        (void)s.unexpected_fields;
        total_rx += s.inbound_frames;
    }
    CHECK(total_rx >= 6);
    CHECK(reader.format_prometheus().find("nimblefix_session_inbound_frames_total") != std::string::npos);

    // Section 6.5: JournalTailer & JournalReplayer::replay callback signature
    std::filesystem::path nfxj_path;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir.path / "direct_buy")) {
        if (e.path().filename() == "session.nfxj") { nfxj_path = e.path(); break; }
    }
    CHECK(!nfxj_path.empty());
    const std::string nfxj_str = nfxj_path.string();

    nf::JournalTailerConfig tcfg{};
    tcfg.path = nfxj_str;
    tcfg.start = nf::JournalTailStart::beginning;
    tcfg.verify_crc = true;
    auto tailer = nf::JournalTailer::open(tcfg).value();
    nf::JournalRecord trec{};
    std::size_t tailed = 0;
    for (;;) {
        const auto st = tailer.next(trec);
        if (st.code == nf::ErrorCode::need_more) break;
        CHECK(st.ok());
        if (!st.ok()) break;
        ++tailed;
    }
    CHECK(tailed > 0);

    nf::ReplayOptions ropts{};
    ropts.journal_path = nfxj_path;
    ropts.direction = nf::ReplayDirection::outbound;
    ropts.timing = nf::ReplayTiming::full_speed;
    ropts.skip_admin = true;
    std::size_t replayed_cb = 0;
    const auto rstats = nf::JournalReplayer::replay(ropts,
        [&](const nf::JournalRecord& rec, const nf::FlatMessageView* parsed) noexcept {
            (void)rec;
            if (parsed != nullptr) ++replayed_cb;
            return nf::Status::success();
        }).value();
    CHECK(rstats.replayed_frames == 4 && replayed_cb == 4);

    // Section 6.1 & 7.1: Compile-verify DirectTlsTransport + SocketOptions and FIXT.1.1 constants
    nf::DirectTlsTransport tls_transport{};
    tls_transport.set_socket_options({.tcp_quickack = true});
    CHECK(nf::begin_string(nf::FixVersion::fixt11) == "FIXT.1.1");
    CHECK(nf::appl_ver_id::fix50sp2 == "9");
}

} // namespace

int main() {
    test_codec_and_view_snippets();
    test_runtime_and_operational_snippets();
    if (g_failures != 0) {
        std::cerr << g_failures << " doc snippet check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "doc_snippets_test passed\n";
    return EXIT_SUCCESS;
}
