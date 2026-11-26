#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/engine.hpp>
#include <nimblefix/fix44_sample.hpp>
#include <nimblefix/message.hpp>
#include <nimblefix/sharded_engine.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace nf = nimblefix;
namespace fix44_sample = nf::fix44_sample;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void fail_check(const char* expr, int line) {
    std::cerr << "FAIL at " << __FILE__ << ":" << line << ": " << expr << '\n';
    ++g_failures;
}

#define CHECK(expr) do { if (!(expr)) fail_check(#expr, __LINE__); } while (0)

struct TempDir {
    fs::path path;
    explicit TempDir(std::string_view tag) {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("nfx_p1_" + std::string{tag} + "_" + std::to_string(::getpid()) + "_" + std::to_string(now));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string make_raw_fix(std::string_view body) {
    std::string header = "8=FIX.4.4\x01""9=" + std::to_string(body.size()) + "\x01";
    std::string frame = header + std::string{body};
    unsigned sum = 0;
    for (unsigned char c : frame) sum += c;
    sum %= 256U;
    char tail[8];
    std::snprintf(tail, sizeof(tail), "10=%03u\x01", sum);
    frame.append(tail);
    return frame;
}

std::uint16_t pick_free_loopback_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    CHECK(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const std::uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

template <class Pred>
bool wait_until(Pred&& pred, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return pred();
}

nf::EngineConfig make_engine_cfg(const fs::path& dir) {
    nf::EngineConfig cfg{};
    cfg.storage.directory = dir;
    cfg.storage.journal_capacity_bytes = 2U * 1024U * 1024U;
    cfg.storage.segment_bytes = 256U * 1024U;
    cfg.storage.journal_max_records = 8192;
    cfg.heartbeat_ms = 5000;
    cfg.connect_timeout_ms = 3000;
    cfg.reconnect_delay_ms = 50;
    cfg.logon_timeout_ms = 3000;
    cfg.logout_timeout_ms = 1500;
    return cfg;
}

// ---------------------------------------------------------------------------
// 1. P1-1 Unit test: UnexpectedFieldPolicy (reject / ignore_with_metric / passthrough)
// ---------------------------------------------------------------------------
void test_unexpected_field_policy_unit() {
    using Schema = fix44_sample::Schema;

    const std::string clean_wire = make_raw_fix(
        "35=D\x01""49=BUY\x01""56=SELL\x01""34=2\x01""52=20261127-10:00:00.000\x01"
        "11=ORD1\x01""55=AAPL\x01""54=1\x01""60=20261127-10:00:00.000\x01""40=2\x01""38=100\x01""44=150.25\x01");

    const std::string extra_wire = make_raw_fix(
        "35=D\x01""49=BUY\x01""56=SELL\x01""34=2\x01""52=20261127-10:00:00.000\x01"
        "11=ORD2\x01""55=AAPL\x01""54=1\x01""60=20261127-10:00:00.000\x01""40=2\x01""38=100\x01""44=150.25\x01"
        "9991=VENUE_A\x01""9992=VENUE_B\x01");

    const std::string bad_enum_and_extra = make_raw_fix(
        "35=D\x01""49=BUY\x01""56=SELL\x01""34=2\x01""52=20261127-10:00:00.000\x01"
        "11=ORD3\x01""55=AAPL\x01""54=Z\x01""60=20261127-10:00:00.000\x01""40=2\x01""38=100\x01""44=150.25\x01"
        "9991=VENUE_A\x01");

    auto parse_one = [](std::string_view wire, std::array<nf::FieldView, 64>& storage,
                        nf::detail::ParsedFieldIndex& idx) -> nf::FlatMessageView {
        nf::FrameView fv{};
        CHECK(nf::inspect_frame(wire, fv).ok());
        nf::FlatMessageView msg{};
        CHECK(nf::parse_flat_verified(fv, storage, msg).ok());
        msg.bind_index(&idx);
        return msg;
    };

    std::array<nf::FieldView, 64> storage{};
    nf::detail::ParsedFieldIndex idx{};

    // Clean message passes under all policies with 0 unexpected fields.
    {
        auto msg = parse_one(clean_wire, storage, idx);
        std::uint64_t cnt = 0;
        CHECK(nf::validate_schema_message(Schema::descriptor(), msg, nf::ValidationLevel::strict,
                                          nf::UnexpectedFieldPolicy::reject, &cnt).ok());
        CHECK(cnt == 0);
        CHECK(nf::validate_schema_message(Schema::descriptor(), msg, nf::ValidationLevel::strict,
                                          nf::UnexpectedFieldPolicy::ignore_with_metric, &cnt).ok());
        CHECK(cnt == 0);
        CHECK(nf::validate_schema_message(Schema::descriptor(), msg, nf::ValidationLevel::strict,
                                          nf::UnexpectedFieldPolicy::passthrough, &cnt).ok());
        CHECK(cnt == 0);
    }

    // Message with 2 unknown tags: rejected under reject, accepted & counted under ignore_with_metric, accepted under passthrough.
    {
        auto msg = parse_one(extra_wire, storage, idx);
        const auto rej = nf::validate_schema_message(Schema::descriptor(), msg, nf::ValidationLevel::strict,
                                                     nf::UnexpectedFieldPolicy::reject);
        CHECK(!rej.ok());
        CHECK(rej.code == nf::ErrorCode::invalid_value);
        CHECK(rej.tag == 9991);

        std::uint64_t cnt = 0;
        const auto met = nf::validate_schema_message(Schema::descriptor(), msg, nf::ValidationLevel::strict,
                                                     nf::UnexpectedFieldPolicy::ignore_with_metric, &cnt);
        CHECK(met.ok());
        CHECK(cnt == 2);
        // Typed view still reads known fields cleanly after fallback index invalidation,
        // and FlatMessageView preserves the unknown custom tags.
        fix44_sample::NewOrderSingleView typed{msg};
        const auto clord = typed.cl_ord_id();
        CHECK(clord.ok() && clord.value() == "ORD2");
        const auto custom1 = msg.find_unique(9991);
        CHECK(custom1.ok() && custom1.value().value == "VENUE_A");
        const auto custom2 = msg.find_unique(9992);
        CHECK(custom2.ok() && custom2.value().value == "VENUE_B");

        const auto pass = nf::validate_schema_message(Schema::descriptor(), msg, nf::ValidationLevel::strict,
                                                      nf::UnexpectedFieldPolicy::passthrough);
        CHECK(pass.ok());
    }

    // Real enum/value error is STILL rejected even under ignore_with_metric and passthrough!
    {
        auto msg = parse_one(bad_enum_and_extra, storage, idx);
        const auto st1 = nf::validate_schema_message(Schema::descriptor(), msg, nf::ValidationLevel::strict,
                                                     nf::UnexpectedFieldPolicy::ignore_with_metric);
        CHECK(!st1.ok());
        CHECK(st1.tag == 54);
        const auto st2 = nf::validate_schema_message(Schema::descriptor(), msg, nf::ValidationLevel::strict,
                                                     nf::UnexpectedFieldPolicy::passthrough);
        CHECK(!st2.ok());
        CHECK(st2.tag == 54);
    }
}

// ---------------------------------------------------------------------------
// 2. P1-1 Integration: Heterogeneous Multi-Schema Engine + UnexpectedFieldPolicy + Telemetry
// ---------------------------------------------------------------------------
void test_heterogeneous_multi_schema_engine_and_policies() {
    TempDir dir("hetero");
    const fs::path tel_path = dir.path / "telemetry.nfxt";

    auto srv_cfg = make_engine_cfg(dir.path / "srv");
    srv_cfg.telemetry_path = tel_path;
    auto srv_res = nf::Engine<nf::FlatSchema, nf::Inline>::create(srv_cfg);
    CHECK(srv_res.ok());
    auto srv = std::move(srv_res).value();

    std::mutex mu;
    std::vector<std::pair<std::string, std::string>> delivered;
    CHECK(srv.on_message([&](nf::SessionContext& ctx, const nf::FlatMessageView& msg) -> nf::Status {
        const auto mt = msg.find_unique(35);
        std::string extra;
        if (const auto f = msg.find_unique(9888); f.ok()) extra = std::string{f.value().value};
        else if (const auto f2 = msg.find_unique(9001); f2.ok()) extra = std::string{f2.value().value};
        std::lock_guard lock(mu);
        delivered.emplace_back(std::string{ctx.session_name()},
                               (mt.ok() ? std::string{mt.value().value} : std::string{}) + ":" + extra);
        return nf::Status::success();
    }).ok());

    CHECK(srv.add_listener(nf::ListenerConfig{"L1", {"127.0.0.1", 0}}).ok());

    nf::AcceptorConfig acc_rej{"SESS_REJECT", "L1", "SRV", "CLI_REJ"};
    acc_rej.settings.use_schema<fix44_sample::Schema>();
    acc_rej.settings.unexpected_field_policy = nf::UnexpectedFieldPolicy::reject;
    auto h_rej = srv.add_acceptor(acc_rej);
    CHECK(h_rej.ok());

    nf::AcceptorConfig acc_met{"SESS_METRIC", "L1", "SRV", "CLI_MET"};
    acc_met.settings.use_schema<fix44_sample::Schema>();
    acc_met.settings.unexpected_field_policy = nf::UnexpectedFieldPolicy::ignore_with_metric;
    auto h_met = srv.add_acceptor(acc_met);
    CHECK(h_met.ok());

    nf::AcceptorConfig acc_flat{"SESS_FLAT", "L1", "SRV", "CLI_FLAT"};
    // No per-session schema: raw flat passthrough for arbitrary custom messages.
    auto h_flat = srv.add_acceptor(acc_flat);
    CHECK(h_flat.ok());

    std::thread srv_thread([&] { (void)srv.run(); });
    nf::TcpEndpoint ep{};
    CHECK(wait_until([&] {
        auto r = srv.listener_endpoint("L1");
        if (r.ok()) { ep = r.value(); return true; }
        return false;
    }));

    // Client engine connecting all 3 sessions to the same heterogeneous server Engine!
    auto cli_cfg = make_engine_cfg(dir.path / "cli");
    auto cli_res = nf::Engine<nf::FlatSchema, nf::Inline>::create(cli_cfg);
    CHECK(cli_res.ok());
    auto cli = std::move(cli_res).value();
    std::atomic<int> peer_rejects{0};
    CHECK(cli.on_message([](nf::SessionContext&, const nf::FlatMessageView&) {
        return nf::Status::success();
    }).ok());
    CHECK(cli.on_error([&](const nf::ErrorEvent& ev) {
        if (ev.status.code == nf::ErrorCode::peer_rejected) {
            peer_rejects.fetch_add(1, std::memory_order_relaxed);
        }
    }).ok());

    auto c_rej = cli.add_initiator(nf::InitiatorConfig{"C_REJ", "CLI_REJ", "SRV", ep});
    auto c_met = cli.add_initiator(nf::InitiatorConfig{"C_MET", "CLI_MET", "SRV", ep});
    auto c_flat = cli.add_initiator(nf::InitiatorConfig{"C_FLAT", "CLI_FLAT", "SRV", ep});
    CHECK(c_rej.ok() && c_met.ok() && c_flat.ok());

    std::thread cli_thread([&] { (void)cli.run(); });

    CHECK(wait_until([&] {
        auto s1 = cli.snapshot("C_REJ");
        auto s2 = cli.snapshot("C_MET");
        auto s3 = cli.snapshot("C_FLAT");
        return s1.ok() && s1.value().state == nf::SessionState::active &&
               s2.ok() && s2.value().state == nf::SessionState::active &&
               s3.ok() && s3.value().state == nf::SessionState::active;
    }));

    // Verify heterogeneous Sender<Schema> AND flat Sender<> both work on SESS_METRIC.
    auto typed_sender = h_met.value().sender<fix44_sample::Schema>();
    auto flat_sender = h_met.value().sender();
    CHECK(typed_sender.can_send());
    CHECK(flat_sender.can_send());

    // Build a NewOrderSingle with an unexpected venue tag 9888=VENUE_X.
    auto nos_extra = nf::message<"D">()
        .set<11>("CL1").set<55>("MSFT").set<54>('1')
        .set<60>("20261127-10:00:00.000").set<40>('2')
        .set<38>(100).set<44>("250.50").set<9888>("VENUE_X");

    // Send to SESS_REJECT -> must trigger peer Session Reject (35=3), NOT delivered!
    CHECK(c_rej.value().sender().send(nos_extra).ok());
    // Send to SESS_METRIC -> must be accepted & delivered with unexpected_fields == 1!
    CHECK(c_met.value().sender().send(nos_extra).ok());
    // Send custom U1 message to SESS_FLAT -> must be accepted & delivered!
    auto custom_u1 = nf::message<"U1">().set<9001>("CUSTOM_OK");
    CHECK(c_flat.value().sender().send(custom_u1).ok());

    CHECK(wait_until([&] {
        std::lock_guard lock(mu);
        return delivered.size() == 2 && peer_rejects.load(std::memory_order_relaxed) >= 1;
    }));

    // Verify SESS_REJECT was NOT delivered while SESS_METRIC & SESS_FLAT were!
    {
        std::lock_guard lock(mu);
        bool saw_met = false, saw_flat = false, saw_rej = false;
        for (const auto& [sess, info] : delivered) {
            if (sess == "SESS_METRIC" && info == "D:VENUE_X") saw_met = true;
            if (sess == "SESS_FLAT" && info == "U1:CUSTOM_OK") saw_flat = true;
            if (sess == "SESS_REJECT") saw_rej = true;
        }
        CHECK(saw_met);
        CHECK(saw_flat);
        CHECK(!saw_rej);
    }

    // Verify SessionSnapshot and out-of-band TelemetryReader both report unexpected_fields == 1!
    CHECK(wait_until([&] {
        auto snap = srv.snapshot("SESS_METRIC");
        return snap.ok() && snap.value().unexpected_fields == 1;
    }));
    {
        auto reader = nf::TelemetryReader::open(tel_path);
        CHECK(reader.ok());
        const auto snaps = reader.value().read_all();
        bool found_met = false;
        for (const auto& s : snaps) {
            if (s.name == "SESS_METRIC") {
                found_met = true;
                CHECK(s.unexpected_fields == 1);
            }
        }
        CHECK(found_met);
        const auto prom = reader.value().format_prometheus();
        CHECK(prom.find("nimblefix_session_unexpected_fields_total") != std::string::npos);
    }

    cli.request_stop();
    srv.request_stop();
    cli_thread.join();
    srv_thread.join();
}

// ---------------------------------------------------------------------------
// 3. P1-2: SessionSchedule::window_id + Schedule-Boundary Auto-Reset
// ---------------------------------------------------------------------------
std::atomic<std::int64_t> g_mock_clock_sec{0};

std::chrono::system_clock::time_point mock_schedule_clock() noexcept {
    return std::chrono::system_clock::time_point{std::chrono::seconds{g_mock_clock_sec.load(std::memory_order_acquire)}};
}

void test_schedule_window_id_and_boundary_auto_reset() {
    using namespace std::chrono;

    // Validation: reset_on_schedule_boundary requires enabled && allow_sequence_reset.
    {
        nf::SessionSettings s{};
        s.schedule.enabled = false;
        s.schedule.reset_on_schedule_boundary = true;
        s.allow_sequence_reset = true;
        CHECK(!nf::validate_session_settings(s, false, false).ok());

        s.schedule.enabled = true;
        s.allow_sequence_reset = false;
        CHECK(!nf::validate_session_settings(s, false, false).ok());

        s.allow_sequence_reset = true;
        CHECK(nf::validate_session_settings(s, false, false).ok());
    }

    // Unit test window_id() for daytime & overnight schedules.
    {
        // 2026-11-23 is Monday (weekday c_encoding = 1).
        const auto mon = sys_days{year{2026} / November / day{23}};
        const auto tue = mon + days{1};
        const auto sat = mon + days{5};
        const auto sun = mon - days{1};

        nf::SessionSchedule day_sched{};
        day_sched.enabled = true;
        day_sched.weekdays = 0b00111110; // Mon-Fri
        day_sched.start_second_utc = 9 * 3600;
        day_sched.end_second_utc = 17 * 3600;

        CHECK(day_sched.window_id(mon + hours{8}) == 0);
        const auto mon_w1 = day_sched.window_id(mon + hours{9});
        const auto mon_w2 = day_sched.window_id(mon + hours{16} + minutes{59});
        CHECK(mon_w1 != 0 && mon_w1 == mon_w2);
        CHECK(day_sched.window_id(mon + hours{17}) == 0);
        const auto tue_w = day_sched.window_id(tue + hours{10});
        CHECK(tue_w == mon_w1 + 86400);
        CHECK(day_sched.window_id(sat + hours{12}) == 0);

        nf::SessionSchedule overnight{};
        overnight.enabled = true;
        overnight.weekdays = 0x7f;
        overnight.start_second_utc = 22 * 3600;
        overnight.end_second_utc = 6 * 3600;

        const auto ov_sun = overnight.window_id(sun + hours{23});
        const auto ov_mon_am = overnight.window_id(mon + hours{5});
        const auto ov_mon_pm = overnight.window_id(mon + hours{23});
        CHECK(ov_sun != 0 && ov_sun == ov_mon_am);
        CHECK(ov_mon_pm == ov_sun + 86400);
        CHECK(overnight.window_id(mon + hours{12}) == 0);
    }

    // End-to-end Engine test: Day 1 window -> advance clock to Day 2 -> auto-logout + 141=Y reset!
    {
        TempDir dir("sched_reset");
        const auto mon = sys_days{year{2026} / November / day{23}};
        const auto day1_open = duration_cast<seconds>((mon + hours{10}).time_since_epoch()).count();
        const auto day2_open = duration_cast<seconds>((mon + days{1} + hours{10}).time_since_epoch()).count();
        g_mock_clock_sec.store(day1_open, std::memory_order_release);

        nf::SessionSchedule sched{};
        sched.enabled = true;
        sched.weekdays = 0x7f;
        sched.start_second_utc = 9 * 3600;
        sched.end_second_utc = 17 * 3600;
        sched.reset_on_schedule_boundary = true;

        auto srv_cfg = make_engine_cfg(dir.path / "srv");
        srv_cfg.schedule_clock = &mock_schedule_clock;
        auto srv = nf::Engine<nf::FlatSchema, nf::Inline>::create(srv_cfg).value();
        std::atomic<int> srv_msgs{0};
        CHECK(srv.on_message([&](nf::SessionContext&, const nf::FlatMessageView&) {
            srv_msgs.fetch_add(1, std::memory_order_relaxed);
            return nf::Status::success();
        }).ok());
        CHECK(srv.add_listener(nf::ListenerConfig{"L", {"127.0.0.1", 0}}).ok());
        nf::AcceptorConfig acc{"ACC", "L", "SRV", "CLI"};
        acc.settings.allow_sequence_reset = true;
        acc.settings.schedule = sched;
        CHECK(srv.add_acceptor(acc).ok());

        std::thread srv_thread([&] { (void)srv.run(); });
        nf::TcpEndpoint ep{};
        CHECK(wait_until([&] {
            auto r = srv.listener_endpoint("L");
            if (r.ok()) { ep = r.value(); return true; }
            return false;
        }));

        auto cli_cfg = make_engine_cfg(dir.path / "cli");
        cli_cfg.schedule_clock = &mock_schedule_clock;
        auto cli = nf::Engine<nf::FlatSchema, nf::Inline>::create(cli_cfg).value();
        CHECK(cli.on_message([](nf::SessionContext&, const nf::FlatMessageView&) {
            return nf::Status::success();
        }).ok());
        nf::InitiatorConfig init{"INIT", "CLI", "SRV", ep};
        init.settings.allow_sequence_reset = true;
        init.settings.schedule = sched;
        auto init_h = cli.add_initiator(init);
        CHECK(init_h.ok());

        std::thread cli_thread([&] { (void)cli.run(); });
        CHECK(wait_until([&] {
            auto s = cli.snapshot("INIT");
            return s.ok() && s.value().state == nf::SessionState::active;
        }));

        // Send 3 messages in Day 1 window so next_outbound_seq >= 5.
        for (int i = 0; i < 3; ++i) {
            const std::string id = "D1_" + std::to_string(i);
            auto m = nf::message<"D">().set<11>(id).set<55>("AAPL");
            CHECK(init_h.value().sender().send(m).ok());
        }
        CHECK(wait_until([&] { return srv_msgs.load(std::memory_order_relaxed) == 3; }));
        const auto gen_before = cli.snapshot("INIT").value().generation;
        CHECK(cli.snapshot("INIT").value().next_outbound_seq >= 5);

        // Jump clock directly into Day 2 window -> Engine must logout, reconnect with 141=Y, and reset seqs!
        g_mock_clock_sec.store(day2_open, std::memory_order_release);
        CHECK(wait_until([&] {
            auto s = cli.snapshot("INIT");
            return s.ok() && s.value().generation > gen_before &&
                   s.value().state == nf::SessionState::active &&
                   s.value().next_outbound_seq == 2 && s.value().next_inbound_seq == 2;
        }));

        // Send message in new Day 2 epoch (seq 2) and verify Acceptor delivers it cleanly!
        auto m2 = nf::message<"D">().set<11>("D2_0").set<55>("AAPL");
        CHECK(init_h.value().sender().send(m2).ok());
        CHECK(wait_until([&] { return srv_msgs.load(std::memory_order_relaxed) == 4; }));
        CHECK(srv.snapshot("ACC").value().next_inbound_seq == 3);

        cli.request_stop();
        srv.request_stop();
        cli_thread.join();
        srv_thread.join();
    }
}

// ---------------------------------------------------------------------------
// 4. P1-3: Runtime Admin Controls (Engine, ShardedEngine, SessionDriver + GapFill)
// ---------------------------------------------------------------------------
void test_admin_controls_and_gapfill() {
    // 4a. SessionDriver offline adjust_sequences + ResendRequest GapFill across adjusted range.
    {
        TempDir dir("driver_adj");
        nf::SessionConfig cfg{};
        cfg.role = nf::SessionRole::initiator;
        cfg.name = "DRV";
        cfg.storage_directory = dir.path.string();
        cfg.session_key = "FIX.4.4/CLI/SRV";
        cfg.local_comp_id = "CLI";
        cfg.remote_comp_id = "SRV";
        nf::SessionSettings settings{};
        settings.allow_sequence_reset = true;
        cfg.settings = settings;

        {
            auto drv_res = nf::SessionDriver::create(cfg);
            CHECK(drv_res.ok());
            auto drv = std::move(drv_res).value();

            // Adjust offline from (1, 1) -> (5, 10).
            CHECK(drv.adjust_sequences(5, 10, "ops jump").ok());
            CHECK(drv.next_inbound_seq() == 5);
            CHECK(drv.next_outbound_seq() == 10);
        }

        // Reopen from disk after closing previous driver to prove durability!
        auto drv_res = nf::SessionDriver::create(cfg);
        CHECK(drv_res.ok());
        auto drv = std::move(drv_res).value();
        CHECK(drv.next_inbound_seq() == 5);
        CHECK(drv.next_outbound_seq() == 10);

        // Connect and complete Logon at seq 10 / 5.
        const std::string wt = "20261127-10:00:00.000";
        CHECK(drv.connected(1000, wt).ok());
        std::string tx_bytes;
        auto sink = +[](void* ctx, std::span<const char> b) noexcept -> nf::WriteResult {
            static_cast<std::string*>(ctx)->append(b.data(), b.size());
            return {b.size(), false, nf::Status::success()};
        };
        CHECK(drv.flush(sink, &tx_bytes, 1000, 65536).ok());
        CHECK(tx_bytes.find("34=10\x01") != std::string::npos);

        const std::string peer_logon = make_raw_fix(
            "35=A\x01""49=SRV\x01""56=CLI\x01""34=5\x01""52=20261127-10:00:00.000\x01""98=0\x01""108=30\x01");
        CHECK(drv.receive(peer_logon, 1010, wt).ok());
        CHECK(drv.state() == nf::SessionState::active);

        // Send a business message at seq 11.
        auto ord = nf::message<"D">().set<11>("O11").set<55>("AAPL");
        auto rcpt = drv.send(ord, 1020, wt);
        CHECK(rcpt.ok() && rcpt.value().msg_seq_num == 11);
        tx_bytes.clear();
        CHECK(drv.flush(sink, &tx_bytes, 1020, 65536).ok());

        // Peer sends ResendRequest starting MID-GAP (BeginSeqNo=4, EndSeqNo=11)!
        const std::string resend = make_raw_fix(
            "35=2\x01""49=SRV\x01""56=CLI\x01""34=6\x01""52=20261127-10:00:00.000\x01""7=4\x01""16=11\x01");
        CHECK(drv.receive(resend, 1030, wt).ok());
        tx_bytes.clear();
        CHECK(drv.flush(sink, &tx_bytes, 1030, 65536).ok());
        // Must emit GapFill (35=4, 34=4, 43=Y, 123=Y, 36=11) + replayed order (35=D, 34=11, 43=Y)!
        CHECK(tx_bytes.find("35=4\x01") != std::string::npos);
        CHECK(tx_bytes.find("34=4\x01") != std::string::npos);
        CHECK(tx_bytes.find("123=Y\x01") != std::string::npos);
        CHECK(tx_bytes.find("36=11\x01") != std::string::npos);
        CHECK(tx_bytes.find("35=D\x01") != std::string::npos);
        CHECK(tx_bytes.find("11=O11\x01") != std::string::npos);

        // Lowering sequences while disconnected requires allow_sequence_reset and succeeds with epoch reset.
        CHECK(drv.disconnected(nf::Status{nf::ErrorCode::connection_closed}, 1040).ok());
        CHECK(drv.adjust_sequences(2, 3, "lower reset").ok());
        CHECK(drv.next_inbound_seq() == 2);
        CHECK(drv.next_outbound_seq() == 3);
    }

    // 4b. Engine runtime admin controls (request_logout, request_reconnect, request_sequence_reset, adjust_sequences).
    {
        TempDir dir("engine_admin");
        auto srv_cfg = make_engine_cfg(dir.path / "srv");
        auto srv = nf::Engine<nf::FlatSchema, nf::Inline>::create(srv_cfg).value();
        std::atomic<int> srv_msgs{0};
        CHECK(srv.on_message([&](nf::SessionContext&, const nf::FlatMessageView&) {
            srv_msgs.fetch_add(1, std::memory_order_relaxed);
            return nf::Status::success();
        }).ok());
        CHECK(srv.add_listener(nf::ListenerConfig{"L", {"127.0.0.1", 0}}).ok());
        nf::AcceptorConfig acc{"ACC", "L", "SRV", "CLI"};
        acc.settings.allow_sequence_reset = true;
        CHECK(srv.add_acceptor(acc).ok());

        std::thread srv_thread([&] { (void)srv.run(); });
        nf::TcpEndpoint ep{};
        CHECK(wait_until([&] {
            auto r = srv.listener_endpoint("L");
            if (r.ok()) { ep = r.value(); return true; }
            return false;
        }));

        auto cli_cfg = make_engine_cfg(dir.path / "cli");
        auto cli = nf::Engine<nf::FlatSchema, nf::Inline>::create(cli_cfg).value();
        CHECK(cli.on_message([](nf::SessionContext&, const nf::FlatMessageView&) {
            return nf::Status::success();
        }).ok());
        nf::InitiatorConfig init{"INIT", "CLI", "SRV", ep};
        init.settings.allow_sequence_reset = true;
        auto init_h = cli.add_initiator(init);
        CHECK(init_h.ok());

        std::thread cli_thread([&] { (void)cli.run(); });
        CHECK(wait_until([&] {
            auto s = cli.snapshot("INIT");
            return s.ok() && s.value().state == nf::SessionState::active;
        }));

        // Exchange 2 messages so sequences advance.
        for (int i = 0; i < 2; ++i) {
            const std::string id = "M" + std::to_string(i);
            CHECK(init_h.value().sender().send(nf::message<"D">().set<11>(id)).ok());
        }
        CHECK(wait_until([&] { return srv_msgs.load() == 2; }));

        // adjust_sequences while connected MUST fail with not_ready!
        CHECK(cli.adjust_sequences("INIT", 20, 20).code == nf::ErrorCode::not_ready);

        // request_logout from external thread -> session logs out & reconnects preserving seqs!
        const auto gen1 = cli.snapshot("INIT").value().generation;
        CHECK(cli.request_logout("INIT", "admin maintenance").ok());
        CHECK(wait_until([&] {
            auto s = cli.snapshot("INIT");
            return s.ok() && s.value().generation > gen1 && s.value().state == nf::SessionState::active;
        }));
        CHECK(cli.snapshot("INIT").value().next_outbound_seq >= 5);

        // request_reconnect from external thread -> drops & immediately reconnects!
        const auto gen2 = cli.snapshot("INIT").value().generation;
        CHECK(cli.request_reconnect("INIT").ok());
        CHECK(wait_until([&] {
            auto s = cli.snapshot("INIT");
            return s.ok() && s.value().generation > gen2 && s.value().state == nf::SessionState::active;
        }));

        // request_sequence_reset from external thread -> one-shot 141=Y reset to seq 1!
        const auto gen3 = cli.snapshot("INIT").value().generation;
        CHECK(cli.request_sequence_reset("INIT", "ops eod reset").ok());
        CHECK(wait_until([&] {
            auto s = cli.snapshot("INIT");
            return s.ok() && s.value().generation > gen3 &&
                   s.value().state == nf::SessionState::active &&
                   s.value().next_outbound_seq == 2 && s.value().next_inbound_seq == 2;
        }));

        // Verify one-shot semantics: a subsequent request_reconnect preserves seqs (does NOT reset again)!
        CHECK(init_h.value().sender().send(nf::message<"D">().set<11>("POST_RST")).ok());
        CHECK(wait_until([&] { return srv_msgs.load() == 3; }));
        const auto gen4 = cli.snapshot("INIT").value().generation;
        CHECK(cli.request_reconnect("INIT").ok());
        CHECK(wait_until([&] {
            auto s = cli.snapshot("INIT");
            return s.ok() && s.value().generation > gen4 && s.value().state == nf::SessionState::active;
        }));
        CHECK(cli.snapshot("INIT").value().next_outbound_seq >= 4);

        cli.request_stop();
        srv.request_stop();
        cli_thread.join();
        srv_thread.join();

        // Offline adjust_sequences before restart on BOTH sides: CLI (15, 25), SRV (25, 15)!
        auto srv2 = nf::Engine<nf::FlatSchema, nf::Inline>::create(srv_cfg).value();
        std::atomic<int> srv2_msgs{0};
        CHECK(srv2.on_message([&](nf::SessionContext&, const nf::FlatMessageView&) {
            srv2_msgs.fetch_add(1, std::memory_order_relaxed);
            return nf::Status::success();
        }).ok());
        CHECK(srv2.add_listener(nf::ListenerConfig{"L", {"127.0.0.1", 0}}).ok());
        CHECK(srv2.add_acceptor(acc).ok());
        CHECK(srv2.adjust_sequences("ACC", 25, 15, "offline sync").ok());

        std::thread srv2_thread([&] { (void)srv2.run(); });
        nf::TcpEndpoint ep2{};
        CHECK(wait_until([&] {
            auto r = srv2.listener_endpoint("L");
            if (r.ok()) { ep2 = r.value(); return true; }
            return false;
        }));

        auto cli2 = nf::Engine<nf::FlatSchema, nf::Inline>::create(cli_cfg).value();
        CHECK(cli2.on_message([](nf::SessionContext&, const nf::FlatMessageView&) {
            return nf::Status::success();
        }).ok());
        init.endpoint = ep2;
        auto init_h2 = cli2.add_initiator(init);
        CHECK(init_h2.ok());
        CHECK(cli2.adjust_sequences("INIT", 15, 25, "offline sync").ok());

        std::thread cli2_thread([&] { (void)cli2.run(); });
        CHECK(wait_until([&] {
            auto s = cli2.snapshot("INIT");
            return s.ok() && s.value().state == nf::SessionState::active;
        }));
        CHECK(cli2.snapshot("INIT").value().next_outbound_seq == 26);
        CHECK(cli2.snapshot("INIT").value().next_inbound_seq == 16);
        CHECK(init_h2.value().sender().send(nf::message<"D">().set<11>("AFTER_ADJ")).ok());
        CHECK(wait_until([&] { return srv2_msgs.load() == 1; }));
        CHECK(srv2.snapshot("ACC").value().next_inbound_seq == 27);

        cli2.request_stop();
        srv2.request_stop();
        cli2_thread.join();
        srv2_thread.join();
    }

    // 4c. ShardedEngine admin control routing across shards.
    {
        TempDir dir("sharded_admin");
        nf::ShardedEngineConfig scfg{};
        scfg.base = make_engine_cfg(dir.path / "sharded");
        scfg.shard_count = 2;
        auto se_res = nf::ShardedEngine<nf::FlatSchema, nf::Inline>::create(scfg);
        CHECK(se_res.ok());
        auto se = std::move(se_res).value();
        CHECK(se.on_message([](nf::SessionContext&, const nf::FlatMessageView&) {
            return nf::Status::success();
        }).ok());
        CHECK(se.add_listener(nf::ListenerConfig{"L", {"127.0.0.1", 0}}).ok());
        nf::AcceptorConfig a0{"ACC0", "L", "SRV", "C0"};
        a0.settings.allow_sequence_reset = true;
        nf::AcceptorConfig a1{"ACC1", "L", "SRV", "C1"};
        a1.settings.allow_sequence_reset = true;
        CHECK(se.add_acceptor(a0, 0).ok());
        CHECK(se.add_acceptor(a1, 1).ok());

        CHECK(se.adjust_sequences("ACC0", 7, 8).ok());
        CHECK(se.adjust_sequences("ACC1", 11, 12).ok());
        CHECK(se.snapshot("ACC0").value().next_inbound_seq == 7);
        CHECK(se.snapshot("ACC1").value().next_outbound_seq == 12);
        CHECK(se.request_sequence_reset("ACC1", "sharded reset").ok());
        CHECK(!se.request_logout("UNKNOWN_SESSION").ok());
    }
}

// ---------------------------------------------------------------------------
// 5. P1-4: SocketOptions (local source IP/port binding & low-latency tuning)
//    + DirectInitiator UnexpectedFieldPolicy / request_sequence_reset / adjust_sequences
// ---------------------------------------------------------------------------
void test_socket_options_and_direct_initiator_p1() {
    TempDir dir("sock_direct");

    // 5a. Engine invalid local_bind_ip rejected cleanly.
    {
        auto cfg = make_engine_cfg(dir.path / "bad_bind");
        auto eng = nf::Engine<nf::FlatSchema, nf::Inline>::create(cfg).value();
        nf::InitiatorConfig bad_init{"BAD", "A", "B", {"127.0.0.1", 9999}};
        bad_init.socket_options.local_bind_ip = "not.a.valid.ip";
        CHECK(!eng.add_initiator(bad_init).ok());

        nf::ListenerConfig bad_lis{"BADL", {"127.0.0.1", 0}};
        bad_lis.socket_options.local_bind_ip = "invalid-host";
        CHECK(!eng.add_listener(bad_lis).ok());
    }

    // 5b. Engine Initiator binds exact local source IP/port verified via getpeername()!
    {
        int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(listen_fd >= 0);
        int one = 1;
        ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in saddr{};
        saddr.sin_family = AF_INET;
        saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        saddr.sin_port = 0;
        CHECK(::bind(listen_fd, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr)) == 0);
        socklen_t slen = sizeof(saddr);
        CHECK(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&saddr), &slen) == 0);
        const std::uint16_t srv_port = ntohs(saddr.sin_port);
        CHECK(::listen(listen_fd, 4) == 0);

        const std::uint16_t chosen_src_port = pick_free_loopback_port();

        std::atomic<std::uint16_t> observed_peer_port{0};
        std::string observed_peer_ip;
        std::thread accept_thr([&] {
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            int cfd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (cfd >= 0) {
                char ip[INET_ADDRSTRLEN]{};
                ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
                observed_peer_ip = ip;
                observed_peer_port.store(ntohs(peer.sin_port), std::memory_order_release);
                ::close(cfd);
            }
            ::close(listen_fd);
        });

        auto cfg = make_engine_cfg(dir.path / "eng_src_bind");
        auto eng = nf::Engine<nf::FlatSchema, nf::Inline>::create(cfg).value();
        CHECK(eng.on_message([](nf::SessionContext&, const nf::FlatMessageView&) {
            return nf::Status::success();
        }).ok());
        nf::InitiatorConfig ic{"SRC", "CLI", "SRV", {"127.0.0.1", srv_port}};
        ic.socket_options.local_bind_ip = "127.0.0.1";
        ic.socket_options.local_bind_port = chosen_src_port;
        ic.socket_options.tcp_quickack = true;
        ic.socket_options.send_buffer_bytes = 65536;
        ic.socket_options.recv_buffer_bytes = 65536;
        ic.socket_options.so_busy_poll_us = 10; // best-effort, non-fatal without CAP_NET_ADMIN
        CHECK(eng.add_initiator(ic).ok());

        std::thread eng_thr([&] { (void)eng.run(); });
        CHECK(wait_until([&] { return observed_peer_port.load(std::memory_order_acquire) != 0; }));
        CHECK(observed_peer_ip == "127.0.0.1");
        CHECK(observed_peer_port.load(std::memory_order_acquire) == chosen_src_port);

        eng.request_stop();
        eng_thr.join();
        accept_thr.join();
    }

    // 5c. DirectInitiator: SocketOptions source binding + UnexpectedFieldPolicy::ignore_with_metric
    //     + telemetry + request_sequence_reset() + adjust_sequences()!
    {
        int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(listen_fd >= 0);
        int one = 1;
        ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in saddr{};
        saddr.sin_family = AF_INET;
        saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        saddr.sin_port = 0;
        CHECK(::bind(listen_fd, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr)) == 0);
        socklen_t slen = sizeof(saddr);
        CHECK(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&saddr), &slen) == 0);
        const std::uint16_t srv_port = ntohs(saddr.sin_port);
        CHECK(::listen(listen_fd, 4) == 0);

        const std::uint16_t direct_src_port = pick_free_loopback_port();
        const fs::path tel_path = dir.path / "direct.nfxt";

        struct Handler {
            int delivered{0};
            std::string saw_custom;
            nf::Status on_message(const nf::FlatMessageView& msg, const nf::DeliveryInfo&) noexcept {
                ++delivered;
                if (auto f = msg.find_unique(9701); f.ok()) saw_custom = std::string{f.value().value};
                return nf::Status::success();
            }
        };

        nf::DirectInitiatorConfig dcfg{};
        dcfg.host = "127.0.0.1";
        dcfg.port = srv_port;
        dcfg.storage_directory = (dir.path / "direct_store").string();
        dcfg.local_comp_id = "DCLI";
        dcfg.remote_comp_id = "DSRV";
        dcfg.allow_sequence_reset = true;
        dcfg.unexpected_field_policy = nf::UnexpectedFieldPolicy::ignore_with_metric;
        dcfg.telemetry_path = tel_path;
        dcfg.socket_options.local_bind_ip = "127.0.0.1";
        dcfg.socket_options.local_bind_port = direct_src_port;
        dcfg.socket_options.tcp_quickack = true;
        dcfg.socket_options.send_buffer_bytes = 65536;
        dcfg.socket_options.recv_buffer_bytes = 65536;
        dcfg.socket_options.so_busy_poll_us = 10;

        std::atomic<std::uint16_t> peer_src_port{0};
        std::atomic<bool> saw_logon_seq6{false};
        std::atomic<bool> saw_reset_141{false};
        std::thread srv_thr([&] {
            // Connection 1: verify source port & seq 6, reply Logon seq 4 + ExecutionReport seq 5 with 2 unknown tags!
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            int c1 = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (c1 >= 0) {
                peer_src_port.store(ntohs(peer.sin_port), std::memory_order_release);
                char buf[4096]{};
                const ssize_t n = ::recv(c1, buf, sizeof(buf), 0);
                if (n > 0 && std::string_view{buf, static_cast<std::size_t>(n)}.find("34=6\x01") != std::string_view::npos) {
                    saw_logon_seq6.store(true, std::memory_order_release);
                }
                const std::string logon_reply = make_raw_fix(
                    "35=A\x01""49=DSRV\x01""56=DCLI\x01""34=4\x01""52=20261127-10:00:00.000\x01""98=0\x01""108=30\x01");
                const std::string er_extra = make_raw_fix(
                    "35=8\x01""49=DSRV\x01""56=DCLI\x01""34=5\x01""52=20261127-10:00:00.000\x01"
                    "37=EX1\x01""17=EXEC1\x01""11=CL1\x01""150=0\x01""39=0\x01""55=AAPL\x01""54=1\x01"
                    "151=100\x01""14=0\x01""6=0\x01""9701=EXTRA1\x01""9702=EXTRA2\x01");
                const std::string both = logon_reply + er_extra;
                (void)::send(c1, both.data(), both.size(), 0);
                while (::recv(c1, buf, sizeof(buf), 0) > 0) {}
                ::close(c1);
            }
            // Connection 2: verify one-shot request_sequence_reset sent 34=1 and 141=Y!
            int c2 = ::accept(listen_fd, nullptr, nullptr);
            if (c2 >= 0) {
                char buf[4096]{};
                const ssize_t n = ::recv(c2, buf, sizeof(buf), 0);
                if (n > 0) {
                    const std::string_view sv{buf, static_cast<std::size_t>(n)};
                    if (sv.find("34=1\x01") != std::string_view::npos &&
                        sv.find("141=Y\x01") != std::string_view::npos) {
                        saw_reset_141.store(true, std::memory_order_release);
                    }
                }
                const std::string logon_reply2 = make_raw_fix(
                    "35=A\x01""49=DSRV\x01""56=DCLI\x01""34=1\x01""52=20261127-10:00:01.000\x01"
                    "98=0\x01""108=30\x01""141=Y\x01");
                (void)::send(c2, logon_reply2.data(), logon_reply2.size(), 0);
                while (::recv(c2, buf, sizeof(buf), 0) > 0) {}
                ::close(c2);
            }
            ::close(listen_fd);
        });

        {
            auto di_res = nf::DirectInitiator<fix44_sample::Schema, Handler>::open(dcfg);
            CHECK(di_res.ok());
            auto di = std::move(di_res).value();

            // Adjust sequences offline to (4, 6), then verify next connect uses 34=6!
            CHECK(di.adjust_sequences(4, 6, "direct adjust").ok());
            CHECK(di.next_inbound_seq() == 4);
            CHECK(di.next_outbound_seq() == 6);

            CHECK(di.connect().ok());
            CHECK(wait_until([&] {
                (void)di.poll_once();
                return di.handler().delivered == 1;
            }));
            CHECK(peer_src_port.load(std::memory_order_acquire) == direct_src_port);
            CHECK(saw_logon_seq6.load(std::memory_order_acquire));
            CHECK(di.handler().saw_custom == "EXTRA1");
            CHECK(di.unexpected_fields_count() == 2);
            {
                auto r = nf::TelemetryReader::open(tel_path);
                CHECK(r.ok());
                const auto snaps = r.value().read_all();
                CHECK(snaps.size() == 1 && snaps[0].unexpected_fields == 2);
            }
            di.disconnect();
        }

        // Reopen with ephemeral local port to verify durability + one-shot reset across clean reconnect.
        nf::DirectInitiatorConfig dcfg2 = dcfg;
        dcfg2.socket_options.local_bind_port = 0;
        dcfg2.telemetry_path.clear();
        auto di2_res = nf::DirectInitiator<fix44_sample::Schema, Handler>::open(dcfg2);
        CHECK(di2_res.ok());
        auto di2 = std::move(di2_res).value();
        // Verify previous adjusted & advanced seqs persisted across reopen (6, 7)!
        CHECK(di2.next_inbound_seq() == 6);
        CHECK(di2.next_outbound_seq() == 7);
        CHECK(di2.request_sequence_reset("direct one-shot").ok());
        CHECK(di2.connect().ok());
        CHECK(wait_until([&] {
            (void)di2.poll_once();
            return di2.is_active();
        }));
        CHECK(saw_reset_141.load(std::memory_order_acquire));
        CHECK(di2.next_inbound_seq() == 2);
        CHECK(di2.next_outbound_seq() == 2);
        di2.disconnect();
        srv_thr.join();
    }
}

} // namespace

int main() {
    test_unexpected_field_policy_unit();
    test_heterogeneous_multi_schema_engine_and_policies();
    test_schedule_window_id_and_boundary_auto_reset();
    test_admin_controls_and_gapfill();
    test_socket_options_and_direct_initiator_p1();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "p1_features_test passed\n";
    return EXIT_SUCCESS;
}
