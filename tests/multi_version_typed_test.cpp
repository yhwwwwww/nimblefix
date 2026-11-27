#include <nimblefix/codec.hpp>
#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/engine.hpp>
#include <nimblefix/fix42.hpp>
#include <nimblefix/fix43.hpp>
#include <nimblefix/fix44.hpp>
#include <nimblefix/fix50sp2.hpp>
#include <nimblefix/sharded_engine.hpp>
#include <nimblefix/stream_encoder.hpp>

#if defined(NIMBLEFIX_ENABLE_XML)
#include <nimblefix/xml_schema.hpp>
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace nf = nimblefix;

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
               ("nimblefix_mv_" + std::string{tag} + "_" + std::to_string(::getpid()) + "_" +
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

nf::EngineConfig make_engine_cfg(const std::filesystem::path& storage_dir) {
    nf::EngineConfig cfg{};
    cfg.storage.directory = storage_dir;
    cfg.storage.journal_capacity_bytes = 512 * 1024;
    cfg.storage.journal_max_records = 2048;
    cfg.storage.segment_bytes = 128 * 1024;
    cfg.heartbeat_ms = 10'000;
    cfg.reconnect_delay_ms = 20;
    cfg.logon_timeout_ms = 2'000;
    cfg.logout_timeout_ms = 1'000;
    return cfg;
}

// 1. Descriptor & FIXT.1.1 + FIX50SP2 XML parity checks across all standard schemas.
void test_schema_identities_and_xml_parity(const std::filesystem::path& std_schemas_dir) {
    const auto& d42 = nf::fix42::Schema::descriptor();
    const auto& d43 = nf::fix43::Schema::descriptor();
    const auto& d44 = nf::fix44::Schema::descriptor();
    const auto& d50 = nf::fix50sp2::Schema::descriptor();

    CHECK(d42.type == "FIX" && d42.version == "4.2.0");
    CHECK(d43.type == "FIX" && d43.version == "4.3.0");
    CHECK(d44.type == "FIX" && d44.version == "4.4.0");
    CHECK(d50.type == "FIX" && d50.version == "5.0.2");

    CHECK(d42.matches_session(nf::FixVersion::fix42, {}));
    CHECK(!d42.matches_session(nf::FixVersion::fix44, {}));
    CHECK(d43.matches_session(nf::FixVersion::fix43, {}));
    CHECK(d44.matches_session(nf::FixVersion::fix44, {}));

    // FIX 5.0 SP2 runs over FIXT.1.1 with DefaultApplVerID = "9" (appl_ver_id::fix50sp2).
    CHECK(d50.matches_session(nf::FixVersion::fixt11, nf::appl_ver_id::fix50sp2));
    CHECK(!d50.matches_session(nf::FixVersion::fixt11, nf::appl_ver_id::fix50));
    CHECK(!d50.matches_session(nf::FixVersion::fix44, nf::appl_ver_id::fix50sp2));

    // Merged FIXT11.xml + FIX50SP2.xml must contain BOTH FIXT session admin messages + Header/Trailer
    // AND FIX 5.0 SP2 application messages!
    CHECK(d50.find_message(nf::msg_type::logon) != nullptr);
    CHECK(d50.find_message(nf::msg_type::reject) != nullptr);
    CHECK(d50.find_message(nf::msg_type::new_order_single) != nullptr);
    CHECK(d50.find_message(nf::msg_type::execution_report) != nullptr);
    CHECK(d50.header() != nullptr && !d50.header()->members.empty());
    CHECK(d50.trailer() != nullptr && !d50.trailer()->members.empty());

    // Also verify descriptor-fallback strict validation on non-hot post-trade/admin-app messages (e.g. UserRequest 35=BE)
    {
        std::array<char, 512> ubuf{};
        nf::SessionHeader h50{"TRADER50", "VENUE50", 1, "20261127-12:00:00.000"};
        h50.version = nf::FixVersion::fixt11;
        const auto uenc = nf::encode_stream<nf::fix50sp2::UserRequest>(h50, ubuf, [](auto& w) {
            w.header().appl_ver_id(nf::appl_ver_id::fix50sp2);
            w.user_request_id("UR-1").user_request_type(1).username("alice");
        });
        CHECK(uenc.ok());
        nf::FrameView ufv{};
        std::array<nf::FieldView, 32> uscratch{};
        nf::FlatMessageView uflat{};
        CHECK(nf::inspect_frame(std::string_view{ubuf.data(), uenc.value()}, ufv, nf::fix50sp2::Schema::wire_hints()).ok());
        CHECK(nf::parse_flat_verified(ufv, uscratch, uflat, nf::fix50sp2::Schema::wire_hints()).ok());
        CHECK(nf::validate_schema_message(d50, uflat, nf::ValidationLevel::strict).ok());
        nf::fix50sp2::UserRequestView uview{uflat};
        CHECK(uview.user_request_id().ok() && uview.user_request_id().value() == "UR-1");
        CHECK(uview.username().ok() && uview.username().value() == "alice");
    }

#if defined(NIMBLEFIX_ENABLE_XML)
    const auto fixt11_xml = (std_schemas_dir / "FIXT11.xml").string();
    const auto fix50sp2_xml = (std_schemas_dir / "FIX50SP2.xml").string();
    if (std::filesystem::is_regular_file(fixt11_xml) && std::filesystem::is_regular_file(fix50sp2_xml)) {
        nf::XmlSchemaDiagnostic diag{};
        nf::XmlSchema xml_ab{};
        nf::XmlSchema xml_ba{};
        const std::array<std::string_view, 2> ab{fixt11_xml, fix50sp2_xml};
        const std::array<std::string_view, 2> ba{fix50sp2_xml, fixt11_xml};
        CHECK(nf::load_xml_schema(ab, xml_ab, diag).ok());
        CHECK(nf::load_xml_schema(ba, xml_ba, diag).ok());
        CHECK(nf::check_schema_fingerprint(d50, xml_ab).ok());
        CHECK(nf::check_schema_fingerprint(d50, xml_ba).ok());
    }
#else
    (void)std_schemas_dir;
#endif
}

// 2. FIX.4.2 & FIX.4.3 typed codec + session differences (e.g. HandlInst=21 & ExecTransType=20 in 4.2,
//    Parties repeating group + duplicate group detection in 4.3).
void test_fix42_and_fix43_typed_sessions() {
    using namespace nf::literals;

    // 2a. FIX.4.2 codec: HandlInst (tag 21) is required in FIX 4.2 NewOrderSingle; ExecTransType (20) in ExecutionReport.
    {
        std::array<char, 1024> buf{};
        nf::SessionHeader h42{"BUY42", "SELL42", 1, "20261127-12:00:00.000"};
        h42.version = nf::FixVersion::fix42;

        // Missing HandlInst (21) fails in FIX 4.2!
        const auto missing_21 = nf::encode_stream<nf::fix42::NewOrderSingle>(h42, buf, [](auto& w) {
            w.cl_ord_id("O42-MISS").symbol("IBM").side('1')
             .transact_time("20261127-12:00:00.000").order_qty(50).ord_type('2').price("125.50"_dec);
        });
        CHECK(!missing_21.ok() && missing_21.status().code == nf::ErrorCode::required_field_missing &&
              missing_21.status().tag == nf::fix42::field::HandlInst::tag);

        const auto ok_42 = nf::encode_stream<nf::fix42::NewOrderSingle>(h42, buf, [](auto& w) {
            w.cl_ord_id("O42-1").handl_inst('1').symbol("IBM").side('1')
             .transact_time("20261127-12:00:00.000").order_qty(50).ord_type('2').price("125.50"_dec);
        });
        CHECK(ok_42.ok());
        const std::string_view wire42{buf.data(), ok_42.value()};
        CHECK(wire42.starts_with("8=FIX.4.2\x01"));

        nf::FrameView fv{};
        std::array<nf::FieldView, 64> scratch{};
        nf::FlatMessageView flat{};
        CHECK(nf::inspect_frame(wire42, fv, nf::fix42::Schema::wire_hints()).ok());
        CHECK(fv.version == nf::FixVersion::fix42);
        CHECK(nf::parse_flat_verified(fv, scratch, flat, nf::fix42::Schema::wire_hints()).ok());
        CHECK(nf::validate_schema_message(nf::fix42::Schema::descriptor(), flat).ok());
        nf::fix42::NewOrderSingleView view{flat};
        CHECK(view.cl_ord_id().value() == "O42-1");
        CHECK(view.handl_inst().value() == '1');
    }

    // 2b. FIX.4.2 live typed Engine round-trip (NewOrderSingle -> ExecutionReport with tag 20 ExecTransType).
    {
        TempDir dir("fix42_engine");
        auto srv = nf::Engine<nf::fix42::Schema, nf::Inline>::create(make_engine_cfg(dir.path / "srv")).value();
        const auto srv_on = srv.on<nf::fix42::NewOrderSingle>(
            [](nf::SessionContext& ctx, const nf::fix42::NewOrderSingleView& ord) {
                const auto cid = ord.cl_ord_id();
                const auto sym = ord.symbol();
                if (!cid.ok() || !sym.ok()) return nf::Status{nf::ErrorCode::invalid_value};
                const auto sent = ctx.send_stream<nf::fix42::ExecutionReport>([&](auto& w) {
                    w.order_id("ORD42-1").cl_ord_id(cid.value()).exec_id("EX42-1")
                     .exec_trans_type('0') // FIX 4.2 required tag 20!
                     .exec_type('0').ord_status('0').symbol(sym.value()).side('1')
                     .leaves_qty(50).cum_qty(0).avg_px(0);
                });
                return sent.ok() ? nf::Status::success() : sent.status();
            });
        CHECK(srv_on.ok());
        CHECK(srv.ignore<nf::fix42::ExecutionReport>("outbound only").ok());
        CHECK(srv.add_listener(nf::ListenerConfig{"L42", {"127.0.0.1", 0}}).ok());
        nf::AcceptorConfig acc{"ACC42", "L42", "SRV42", "CLI42"};
        acc.settings.version = nf::FixVersion::fix42;
        CHECK(srv.add_acceptor(acc).ok());

        std::thread srv_thr([&] { (void)srv.run(); });
        nf::TcpEndpoint ep{};
        CHECK(wait_until([&] {
            auto r = srv.listener_endpoint("L42");
            if (r.ok()) { ep = r.value(); return true; }
            return false;
        }));

        auto cli = nf::Engine<nf::fix42::Schema, nf::Inline>::create(make_engine_cfg(dir.path / "cli")).value();
        std::atomic<bool> got_er42{false};
        const auto cli_on = cli.on<nf::fix42::ExecutionReport>(
            [&](nf::SessionContext&, const nf::fix42::ExecutionReportView& er) {
                if (er.cl_ord_id().ok() && er.cl_ord_id().value() == "LIVE-42" &&
                    er.exec_trans_type().ok() && er.exec_trans_type().value() == '0') {
                    got_er42.store(true, std::memory_order_release);
                }
                return nf::Status::success();
            });
        CHECK(cli_on.ok());
        CHECK(cli.ignore<nf::fix42::NewOrderSingle>("outbound only").ok());
        nf::InitiatorConfig ic{"INIT42", "CLI42", "SRV42", ep};
        ic.settings.version = nf::FixVersion::fix42;
        auto ih = cli.add_initiator(ic);
        CHECK(ih.ok());
        std::thread cli_thr([&] { (void)cli.run(); });

        CHECK(wait_until([&] {
            auto s = cli.snapshot("INIT42");
            return s.ok() && s.value().state == nf::SessionState::active;
        }));
        CHECK(ih.value().sender<nf::fix42::Schema>().send_stream<nf::fix42::NewOrderSingle>([](auto& w) {
            w.cl_ord_id("LIVE-42").handl_inst('1').symbol("IBM").side('1')
             .transact_time("20261127-12:00:00.000").order_qty(50).ord_type('1');
        }).ok());
        CHECK(wait_until([&] { return got_er42.load(std::memory_order_acquire); }));

        cli.request_stop();
        srv.request_stop();
        cli_thr.join();
        srv_thr.join();
    }

    // 2c. FIX.4.3 codec + duplicate repeating group detection + Queued Engine round-trip.
    {
        std::array<char, 1024> buf{};
        nf::SessionHeader h43{"BUY43", "SELL43", 1, "20261127-12:00:00.000"};
        h43.version = nf::FixVersion::fix43;

        // Duplicate repeating group call on StreamWriter must fail with duplicate_field (tag 453)!
        const auto dup_grp = nf::encode_stream<nf::fix43::NewOrderSingle>(h43, buf, [](auto& w) {
            w.cl_ord_id("O43-DUP").handl_inst('1').symbol("MSFT").side('1')
             .transact_time("20261127-12:00:00.000").order_qty(10).ord_type('1');
            w.no_party_i_ds(1, [](auto& g) { g.entry().party_id("P1"); });
            w.no_party_i_ds(1, [](auto& g) { g.entry().party_id("P2"); });
        });
        CHECK(!dup_grp.ok() && dup_grp.status().code == nf::ErrorCode::duplicate_field &&
              dup_grp.status().tag == nf::fix43::field::NoPartyIDs::tag);

        TempDir dir("fix43_queued");
        auto srv = nf::Engine<nf::fix43::Schema, nf::Queued>::create(make_engine_cfg(dir.path / "srv")).value();
        std::atomic<int> seen_parties{0};
        const auto srv_on = srv.on<nf::fix43::NewOrderSingle>(
            [&](nf::SessionContext&, const nf::fix43::NewOrderSingleView& ord) {
                auto grp = ord.no_party_i_ds();
                if (grp.ok() && grp.value().size() == 2) {
                    seen_parties.store(static_cast<int>(grp.value().size()), std::memory_order_release);
                }
                return nf::Status::success();
            });
        CHECK(srv_on.ok());
        CHECK(srv.add_listener(nf::ListenerConfig{"L43", {"127.0.0.1", 0}}).ok());
        nf::AcceptorConfig acc{"ACC43", "L43", "SRV43", "CLI43"};
        acc.settings.version = nf::FixVersion::fix43;
        CHECK(srv.add_acceptor(acc).ok());
        std::thread srv_thr([&] { (void)srv.run(); });
        nf::TcpEndpoint ep{};
        CHECK(wait_until([&] {
            auto r = srv.listener_endpoint("L43");
            if (r.ok()) { ep = r.value(); return true; }
            return false;
        }));

        auto cli = nf::Engine<nf::fix43::Schema, nf::Inline>::create(make_engine_cfg(dir.path / "cli")).value();
        CHECK(cli.ignore<nf::fix43::ExecutionReport>("none").ok());
        nf::InitiatorConfig ic{"INIT43", "CLI43", "SRV43", ep};
        ic.settings.version = nf::FixVersion::fix43;
        auto ih = cli.add_initiator(ic);
        CHECK(ih.ok());
        std::thread cli_thr([&] { (void)cli.run(); });
        CHECK(wait_until([&] {
            auto s = cli.snapshot("INIT43");
            return s.ok() && s.value().state == nf::SessionState::active;
        }));
        CHECK(ih.value().sender<nf::fix43::Schema>().send_stream<nf::fix43::NewOrderSingle>([](auto& w) {
            w.cl_ord_id("LIVE-43").handl_inst('1').symbol("MSFT").side('1')
             .transact_time("20261127-12:00:00.000").order_qty(25).ord_type('1')
             .no_party_i_ds(2, [](auto& g) {
                 g.entry().party_id("TRADER-A").party_id_source('D').party_role(1);
                 g.entry().party_id("FIRM-B").party_id_source('D').party_role(13);
             });
        }).ok());
        CHECK(wait_until([&] { return seen_parties.load(std::memory_order_acquire) == 2; }));

        cli.request_stop();
        srv.request_stop();
        cli_thr.join();
        srv_thr.join();
    }
}

// 3. FIXT.1.1 + FIX 5.0 SP2 end-to-end typed tests:
//    - 1137=DefaultApplVerID Logon negotiation & mismatch rejection
//    - 1128=ApplVerID & 50=SenderSubID per-message StandardHeaderWriter
//    - Componentized Instrument/OrderQtyData/Parties nested repeating groups
//    - FIX 5.0 SP2 ExecutionReport without AvgPx (optional in 5.0 SP2 vs required in 4.4!)
//    - ShardedEngine<fix50sp2::Schema, Queued> <-> DirectInitiator<fix50sp2::Schema> + ResendRequest/GapFill!
void test_fixt11_fix50sp2_end_to_end() {
    using namespace nf::literals;
    TempDir dir("fixt11_50sp2");

    nf::ShardedEngineConfig scfg{};
    scfg.base = make_engine_cfg(dir.path / "srv");
    scfg.shard_count = 2;
    auto srv = nf::ShardedEngine<nf::fix50sp2::Schema, nf::Queued>::create(scfg).value();

    std::atomic<int> orders_handled{0};
    std::string observed_appl_ver;
    std::string observed_sub_id;
    std::string observed_party_sub;

    const auto on_nos = srv.on<nf::fix50sp2::NewOrderSingle>(
        [&](nf::SessionContext& ctx, const nf::fix50sp2::NewOrderSingleView& ord, const nf::DeliveryInfo& info) {
            (void)info;
            const auto& flat = ord.flat();
            if (const auto av = flat.find_unique(nf::tag::appl_ver_id); av.ok()) {
                observed_appl_ver = std::string{av.value().value};
            }
            if (const auto sub = flat.find_unique(nf::tag::sender_sub_id); sub.ok()) {
                observed_sub_id = std::string{sub.value().value};
            }
            const auto parties = ord.no_party_i_ds();
            if (parties.ok() && parties.value().size() == 1) {
                const auto p0 = parties.value().at(0);
                const auto subs = p0.group<nf::fix50sp2::field::NoPartySubIDs::tag>();
                if (subs.ok() && subs.value().size() == 1) {
                    const auto ps = subs.value().at(0).get<nf::fix50sp2::field::PartySubID::tag>();
                    if (ps.ok()) observed_party_sub = std::string{ps.value()};
                }
            }
            const auto cid = ord.cl_ord_id();
            const auto sym = ord.symbol();
            if (!cid.ok() || !sym.ok()) return nf::Status{nf::ErrorCode::invalid_value};

            // In FIX 5.0 SP2, AvgPx (tag 6) is optional (unlike FIX 4.4 where it was required)!
            const auto sent = ctx.send_stream<nf::fix50sp2::ExecutionReport>([&](auto& w) {
                w.header().appl_ver_id(nf::appl_ver_id::fix50sp2);
                w.order_id("ORD50-1").cl_ord_id(cid.value()).exec_id("EX50-1")
                 .exec_type('0').ord_status('0')
                 .symbol(sym.value()).side('1')
                 .leaves_qty(100).cum_qty(0);
            });
            if (!sent.ok()) return sent.status();
            orders_handled.fetch_add(1, std::memory_order_release);
            return nf::Status::success();
        });
    CHECK(on_nos.ok());
    CHECK(srv.ignore<nf::fix50sp2::ExecutionReport>("outbound only").ok());
    CHECK(srv.add_listener(nf::ListenerConfig{"LT", {"127.0.0.1", 0}}).ok());

    nf::AcceptorConfig acc_ok{"ACC50", "LT", "VENUE50", "TRADER50"};
    acc_ok.settings.version = nf::FixVersion::fixt11;
    acc_ok.settings.default_appl_ver_id = std::string{nf::appl_ver_id::fix50sp2};
    acc_ok.settings.check_sending_time = false;
    CHECK(srv.add_acceptor(acc_ok, 1).ok());

    nf::AcceptorConfig acc_bad_peer{"ACC50_BAD", "LT", "VENUE50", "BADVER_PEER"};
    acc_bad_peer.settings.version = nf::FixVersion::fixt11;
    acc_bad_peer.settings.default_appl_ver_id = std::string{nf::appl_ver_id::fix50sp2};
    acc_bad_peer.settings.check_sending_time = false;
    CHECK(srv.add_acceptor(acc_bad_peer, 1).ok());

    std::thread srv_thr([&] { (void)srv.run(); });
    nf::TcpEndpoint ep{};
    CHECK(wait_until([&] {
        auto r = srv.listener_endpoint("LT");
        if (r.ok()) { ep = r.value(); return true; }
        return false;
    }));

    // 3a. Peer sending wrong 1137=7 (FIX50 instead of 9=FIX50SP2) in FIXT.1.1 Logon must be rejected!
    {
        int bad_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(bad_fd >= 0);
        sockaddr_in saddr{};
        saddr.sin_family = AF_INET;
        saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        saddr.sin_port = htons(ep.port);
        CHECK(::connect(bad_fd, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr)) == 0);
        const std::string body =
            "35=A\x01""49=BADVER_PEER\x01""56=VENUE50\x01""34=1\x01""52=20261127-12:00:00.000\x01"
            "98=0\x01""108=30\x01""1137=7\x01";
        std::string prefix = "8=FIXT.1.1\x01""9=" + std::to_string(body.size()) + "\x01" + body;
        unsigned sum = 0;
        for (unsigned char ch : prefix) sum += ch;
        sum %= 256U;
        char tail[16];
        std::snprintf(tail, sizeof(tail), "10=%03u\x01", sum);
        prefix += tail;
        CHECK(::send(bad_fd, prefix.data(), prefix.size(), 0) == static_cast<ssize_t>(prefix.size()));
        CHECK(wait_until([&] {
            auto s = srv.snapshot("ACC50_BAD");
            return s.ok() && s.value().last_error.code == nf::ErrorCode::unsupported_protocol &&
                   s.value().last_error.tag == nf::tag::default_appl_ver_id;
        }));
        ::close(bad_fd);
    }

    // 3b. DirectInitiator<fix50sp2::Schema> over FIXT.1.1 -> ShardedEngine Shard 1 Acceptor!
    struct Direct50Handler {
        int er_count{0};
        std::string last_exec_id;
        std::string last_appl_ver;
        nf::Status on_message(const nf::FlatMessageView& flat, const nf::DeliveryInfo&) noexcept {
            const auto mt = flat.find_unique(nf::tag::msg_type);
            if (mt.ok() && mt.value().value == nf::msg_type::execution_report) {
                nf::fix50sp2::ExecutionReportView er{flat};
                if (const auto eid = er.exec_id(); eid.ok()) {
                    try { last_exec_id = std::string{eid.value()}; } catch (...) {}
                }
                if (const auto av = flat.find_unique(nf::tag::appl_ver_id); av.ok()) {
                    try { last_appl_ver = std::string{av.value().value}; } catch (...) {}
                }
                ++er_count;
            }
            return nf::Status::success();
        }
    };

    nf::DirectInitiatorConfig dcfg{};
    dcfg.version = nf::FixVersion::fixt11;
    dcfg.default_appl_ver_id = std::string{nf::appl_ver_id::fix50sp2};
    dcfg.host = "127.0.0.1";
    dcfg.port = ep.port;
    dcfg.storage_directory = (dir.path / "direct_cli").string();
    dcfg.local_comp_id = "TRADER50";
    dcfg.remote_comp_id = "VENUE50";
    dcfg.check_sending_time = false;

    auto di_res = nf::DirectInitiator<nf::fix50sp2::Schema, Direct50Handler>::open(dcfg);
    CHECK(di_res.ok());
    auto di = std::move(di_res).value();
    CHECK(di.connect().ok());
    CHECK(wait_until([&] {
        (void)di.poll_once();
        return di.is_active();
    }));

    const auto send_res = di.send_stream<nf::fix50sp2::NewOrderSingle>([](auto& w) {
        w.header().appl_ver_id(nf::appl_ver_id::fix50sp2).sender_sub_id("DESK-FIXT");
        w.cl_ord_id("FIXT-ORD-1")
         .no_party_i_ds(1, [](auto& pg) {
             pg.entry().party_id("CLIENT-50").party_id_source('D').party_role(1)
               .no_party_sub_i_ds(1, [](auto& sg) {
                   sg.entry().party_sub_id("SUB-DESK-9").party_sub_id_type(1);
               });
         })
         .symbol("NVDA")
         .side('1')
         .transact_time("20261127-12:00:00.000")
         .order_qty(100)
         .ord_type('2')
         .price("140.75"_dec);
    });
    CHECK(send_res.ok());
    CHECK(wait_until([&] {
        (void)di.poll_once();
        return di.handler().er_count == 1;
    }));
    CHECK(orders_handled.load(std::memory_order_acquire) == 1);
    CHECK(observed_appl_ver == nf::appl_ver_id::fix50sp2);
    CHECK(observed_sub_id == "DESK-FIXT");
    CHECK(observed_party_sub == "SUB-DESK-9");
    CHECK(di.handler().last_exec_id == "EX50-1");
    CHECK(di.handler().last_appl_ver == nf::appl_ver_id::fix50sp2);

    // 3c. Disconnect, advance initiator offline or reconnect cleanly over FIXT.1.1!
    di.disconnect();
    CHECK(di.connect().ok());
    CHECK(wait_until([&] {
        (void)di.poll_once();
        return di.is_active();
    }));
    CHECK(di.logout("fixt done").ok());
    for (int i = 0; i < 50 && di.valid_transport(); ++i) {
        (void)di.poll_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    srv.request_stop();
    srv_thr.join();
}

// 4. Single heterogeneous Engine<FlatSchema> running FIX.4.2, FIX.4.3, FIX.4.4, and FIXT.1.1+FIX50SP2 simultaneously!
void test_four_protocol_heterogeneous_engine() {
    TempDir dir("hetero_4ver");
    auto srv = nf::Engine<nf::FlatSchema, nf::Inline>::create(make_engine_cfg(dir.path / "srv")).value();
    std::atomic<int> received_42{0}, received_43{0}, received_44{0}, received_50{0};
    CHECK(srv.on_message([&](nf::SessionContext& ctx, const nf::FlatMessageView& msg) {
        if (ctx.session_name() == "A42") received_42.fetch_add(1, std::memory_order_relaxed);
        else if (ctx.session_name() == "A43") received_43.fetch_add(1, std::memory_order_relaxed);
        else if (ctx.session_name() == "A44") received_44.fetch_add(1, std::memory_order_relaxed);
        else if (ctx.session_name() == "A50") received_50.fetch_add(1, std::memory_order_relaxed);
        (void)msg;
        return nf::Status::success();
    }).ok());
    CHECK(srv.add_listener(nf::ListenerConfig{"LH", {"127.0.0.1", 0}}).ok());

    nf::AcceptorConfig a42{"A42", "LH", "S42", "C42"};
    a42.settings.version = nf::FixVersion::fix42;
    a42.settings.use_schema<nf::fix42::Schema>();

    nf::AcceptorConfig a43{"A43", "LH", "S43", "C43"};
    a43.settings.version = nf::FixVersion::fix43;
    a43.settings.use_schema<nf::fix43::Schema>();

    nf::AcceptorConfig a44{"A44", "LH", "S44", "C44"};
    a44.settings.version = nf::FixVersion::fix44;
    a44.settings.use_schema<nf::fix44::Schema>();

    nf::AcceptorConfig a50{"A50", "LH", "S50", "C50"};
    a50.settings.version = nf::FixVersion::fixt11;
    a50.settings.default_appl_ver_id = std::string{nf::appl_ver_id::fix50sp2};
    a50.settings.use_schema<nf::fix50sp2::Schema>();

    CHECK(srv.add_acceptor(a42).ok());
    CHECK(srv.add_acceptor(a43).ok());
    CHECK(srv.add_acceptor(a44).ok());
    CHECK(srv.add_acceptor(a50).ok());

    std::thread srv_thr([&] { (void)srv.run(); });
    nf::TcpEndpoint ep{};
    CHECK(wait_until([&] {
        auto r = srv.listener_endpoint("LH");
        if (r.ok()) { ep = r.value(); return true; }
        return false;
    }));

    auto cli = nf::Engine<nf::FlatSchema, nf::Inline>::create(make_engine_cfg(dir.path / "cli")).value();
    CHECK(cli.on_message([](nf::SessionContext&, const nf::FlatMessageView&) {
        return nf::Status::success();
    }).ok());

    nf::InitiatorConfig i42{"I42", "C42", "S42", ep};
    i42.settings.version = nf::FixVersion::fix42;
    i42.settings.use_schema<nf::fix42::Schema>();

    nf::InitiatorConfig i43{"I43", "C43", "S43", ep};
    i43.settings.version = nf::FixVersion::fix43;
    i43.settings.use_schema<nf::fix43::Schema>();

    nf::InitiatorConfig i44{"I44", "C44", "S44", ep};
    i44.settings.version = nf::FixVersion::fix44;
    i44.settings.use_schema<nf::fix44::Schema>();

    nf::InitiatorConfig i50{"I50", "C50", "S50", ep};
    i50.settings.version = nf::FixVersion::fixt11;
    i50.settings.default_appl_ver_id = std::string{nf::appl_ver_id::fix50sp2};
    i50.settings.use_schema<nf::fix50sp2::Schema>();

    auto h42 = cli.add_initiator(i42).value();
    auto h43 = cli.add_initiator(i43).value();
    auto h44 = cli.add_initiator(i44).value();
    auto h50 = cli.add_initiator(i50).value();

    std::thread cli_thr([&] { (void)cli.run(); });
    CHECK(wait_until([&] {
        for (const char* n : {"I42", "I43", "I44", "I50"}) {
            const auto s = cli.snapshot(n);
            if (!s.ok() || s.value().state != nf::SessionState::active) return false;
        }
        return true;
    }));

    CHECK(h42.sender<nf::fix42::Schema>().send_stream<nf::fix42::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("H42").handl_inst('1').symbol("AAPL").side('1')
         .transact_time("20261127-12:00:00.000").order_qty(10).ord_type('1');
    }).ok());
    CHECK(h43.sender<nf::fix43::Schema>().send_stream<nf::fix43::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("H43").handl_inst('1').symbol("AAPL").side('1')
         .transact_time("20261127-12:00:00.000").order_qty(10).ord_type('1');
    }).ok());
    CHECK(h44.sender<nf::fix44::Schema>().send_stream<nf::fix44::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("H44").symbol("AAPL").side('1')
         .transact_time("20261127-12:00:00.000").order_qty(10).ord_type('1');
    }).ok());
    CHECK(h50.sender<nf::fix50sp2::Schema>().send_stream<nf::fix50sp2::NewOrderSingle>([](auto& w) {
        w.header().appl_ver_id(nf::appl_ver_id::fix50sp2);
        w.cl_ord_id("H50").symbol("AAPL").side('1')
         .transact_time("20261127-12:00:00.000").order_qty(10).ord_type('1');
    }).ok());

    CHECK(wait_until([&] {
        return received_42.load() == 1 && received_43.load() == 1 &&
               received_44.load() == 1 && received_50.load() == 1;
    }));

    cli.request_stop();
    srv.request_stop();
    cli_thr.join();
    srv_thr.join();
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path std_schemas_dir = std::filesystem::current_path() / "schemas" / "standard";
    if (argc >= 2) std_schemas_dir = std::filesystem::path{argv[1]};

    test_schema_identities_and_xml_parity(std_schemas_dir);
    test_fix42_and_fix43_typed_sessions();
    test_fixt11_fix50sp2_end_to_end();
    test_four_protocol_heterogeneous_engine();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "multi_version_typed_test passed (FIX.4.2, FIX.4.3, FIX.4.4, FIXT.1.1+FIX50SP2)\n";
    return EXIT_SUCCESS;
}
