#include <nimblefix/codec.hpp>
#include <nimblefix/engine.hpp>
#include <nimblefix/fix44.hpp>
#include <nimblefix/group.hpp>
#include <nimblefix/schema.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

namespace nf = nimblefix;
namespace fix44 = nimblefix::fix44;

namespace {

int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #cond << '\n'; \
        ++failures; \
    } \
} while (false)

constexpr std::string_view t0{"20261123-00:00:00.000"};

// 1. Verify 2-level nested repeating groups (NoPartyIDs 453 -> NoPartySubIDs 802) followed
//    by outer top-level fields (Symbol 55, Side 54, TransactTime 60, OrdType 40) are parsed
//    cleanly without swallowing outer fields AND validated by single-pass validate_schema_message!
void test_two_level_nested_repeating_group_parsing_and_validation() {
    std::string body =
        "35=D\x01"
        "49=BUY\x01" "56=SELL\x01" "34=2\x01" "52=20261123-00:00:00.000\x01"
        "11=NESTED-1\x01"
        "453=2\x01"
            "448=PARTY-A\x01" "447=D\x01" "452=1\x01"
            "802=2\x01"
                "523=SUB-A1\x01" "803=1\x01"
                "523=SUB-A2\x01" "803=2\x01"
            "448=PARTY-B\x01" "447=D\x01" "452=3\x01"
            "802=1\x01"
                "523=SUB-B1\x01" "803=4\x01"
        "55=NVDA\x01"
        "54=1\x01"
        "60=20261123-00:00:00.000\x01"
        "40=1\x01";

    std::string wire = "8=FIX.4.4\x01" "9=" + std::to_string(body.size()) + "\x01" + body;
    unsigned sum = 0;
    for (unsigned char ch : wire) sum += ch;
    char cs[8]{};
    std::snprintf(cs, sizeof(cs), "10=%03u\x01", sum % 256U);
    wire += cs;

    nf::FrameView fv{};
    CHECK(nf::inspect_frame(wire, fv, fix44::Schema::wire_hints()).ok());
    std::array<nf::FieldView, 256> scratch{};
    nf::FlatMessageView flat{};
    CHECK(nf::parse_flat_verified(fv, scratch, flat, fix44::Schema::wire_hints()).ok());

    const auto val = nf::validate_schema_message(
        fix44::Schema::descriptor(), flat, nf::ValidationLevel::strict);
    if (!val.ok()) std::cerr << "nested validate failed: " << val.message() << " tag=" << val.tag << '\n';
    CHECK(val.ok());

    fix44::NewOrderSingleView view{flat};
    CHECK(view.cl_ord_id().ok() && view.cl_ord_id().value() == "NESTED-1");
    // Outer fields AFTER nested repeating group MUST be visible at top level!
    CHECK(view.symbol().ok() && view.symbol().value() == "NVDA");
    CHECK(view.side().ok() && view.side().value() == '1');
    CHECK(view.ord_type().ok() && view.ord_type().value() == '1');

    // Verify range-based for on outer and inner GroupViews!
    auto parties = view.no_party_i_ds();
    CHECK(parties.ok() && parties.value().size() == 2);
    if (parties.ok()) {
        std::vector<std::string_view> party_ids;
        std::vector<std::string_view> sub_ids;
        for (const auto& party : parties.value()) {
            const auto pid = party.template get<448>();
            CHECK(pid.ok());
            if (pid.ok()) party_ids.push_back(pid.value());
            // Must NOT have swallowed outer tag 55!
            CHECK(!party.template get<55>().ok());
            auto subs = party.template group<802>();
            CHECK(subs.ok());
            if (subs.ok()) {
                for (const auto& sub : subs.value()) {
                    const auto sid = sub.template get<523>();
                    CHECK(sid.ok());
                    if (sid.ok()) sub_ids.push_back(sid.value());
                    CHECK(!sub.template get<55>().ok());
                }
            }
        }
        CHECK(party_ids.size() == 2 && party_ids[0] == "PARTY-A" && party_ids[1] == "PARTY-B");
        CHECK(sub_ids.size() == 3 && sub_ids[0] == "SUB-A1" && sub_ids[1] == "SUB-A2" && sub_ids[2] == "SUB-B1");
    }
}

// 2. Verify repeating group sent through Sender<Schema> (arena -> scratch memcpy relocation!).
void test_sender_owned_message_repeating_group_relocation() {
    char pat[] = "/tmp/nimblefix-group-reloc-XXXXXX";
    char* dir_raw = ::mkdtemp(pat);
    CHECK(dir_raw != nullptr);
    if (dir_raw == nullptr) return;
    std::string dir{dir_raw};

    nf::EngineConfig srv_cfg{};
    srv_cfg.storage.directory = dir + "/srv";
    auto srv_res = nf::Engine<fix44::Schema, nf::Inline>::create(srv_cfg);
    CHECK(srv_res.ok());
    auto cli_cfg = srv_cfg;
    cli_cfg.storage.directory = dir + "/cli";
    auto cli_res = nf::Engine<fix44::Schema, nf::Inline>::create(cli_cfg);
    CHECK(cli_res.ok());
    if (!srv_res.ok() || !cli_res.ok()) { std::filesystem::remove_all(dir); return; }
    auto server = std::move(srv_res).value();
    auto client = std::move(cli_res).value();

    std::atomic<bool> got_group{false};
    std::atomic<bool> cli_active{false};
    CHECK(server.on<fix44::NewOrderSingle>([&](nf::SessionContext&, fix44::NewOrderSingleView view) {
        auto parties = view.no_party_i_ds();
        if (parties.ok() && parties.value().size() == 2) {
            auto e0 = parties.value().at(0);
            auto e1 = parties.value().at(1);
            if (e0.template get<448>().ok() && e1.template get<448>().ok() &&
                e0.template get<448>().value() == "P1" &&
                e1.template get<448>().value() == "P2" &&
                view.symbol().ok() && view.symbol().value() == "AAPL") {
                got_group.store(true, std::memory_order_release);
            }
        }
        return nf::Status::success();
    }).ok());
    CHECK(client.on<fix44::ExecutionReport>([](nf::SessionContext&, fix44::ExecutionReportView) {
        return nf::Status::success();
    }).ok());
    CHECK(client.on_session_active([&](nf::SessionContext&) {
        cli_active.store(true, std::memory_order_release);
        return nf::Status::success();
    }).ok());

    nf::ListenerConfig lcfg{"main", {"127.0.0.1", 0}};
    CHECK(server.add_listener(lcfg).ok());
    nf::AcceptorConfig acfg{"srv-acc", "main", "SELL", "BUY"};
    acfg.settings.check_sending_time = false;
    CHECK(server.add_acceptor(acfg).ok());

    std::thread srv_thread([&] { (void)server.run(); });
    nf::TcpEndpoint ep{};
    for (int i = 0; i < 200; ++i) {
        auto r = server.listener_endpoint("main");
        if (r.ok() && r.value().port != 0) { ep = r.value(); break; }
        ::usleep(5000);
    }
    CHECK(ep.port != 0);

    nf::InitiatorConfig icfg{"cli-init", "BUY", "SELL", ep};
    icfg.settings.check_sending_time = false;
    auto handle = client.add_initiator(icfg);
    CHECK(handle.ok());
    auto sender = handle.value().sender<fix44::Schema>();
    std::thread cli_thread([&] { (void)client.run(); });

    for (int i = 0; i < 400 && !cli_active.load(std::memory_order_acquire); ++i) ::usleep(5000);
    CHECK(cli_active.load());

    // Build NewOrderSingle with attached repeating group and send via cross-thread Sender!
    auto p1 = fix44::NoPartyIDsGroupEntry::make().party_id("P1").party_id_source('D').party_role(1);
    auto p2 = fix44::NoPartyIDsGroupEntry::make().party_id("P2").party_id_source('D').party_role(3);
    fix44::NoPartyIDsGroup<4> parties;
    CHECK(parties.add(std::move(p1)).ok());
    CHECK(parties.add(std::move(p2)).ok());
    auto order = fix44::NewOrderSingle::make()
        .cl_ord_id("GRP-SEND-1")
        .no_party_i_ds(std::move(parties))
        .symbol("AAPL")
        .side('1')
        .transact_time(t0)
        .ord_type('1');
    const auto sent = sender.send(order);
    CHECK(sent.ok());

    for (int i = 0; i < 400 && !got_group.load(std::memory_order_acquire); ++i) ::usleep(5000);
    CHECK(got_group.load());

    client.request_stop();
    server.request_stop();
    cli_thread.join();
    srv_thread.join();
    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    test_two_level_nested_repeating_group_parsing_and_validation();
    test_sender_owned_message_repeating_group_relocation();
    if (failures != 0) {
        std::cerr << failures << " nested group test(s) failed\n";
        return 1;
    }
    std::cout << "nested repeating group and cross-thread group relocation tests passed\n";
    return 0;
}
