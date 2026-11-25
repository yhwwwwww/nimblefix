#include <nimblefix/codec.hpp>
#include <nimblefix/engine.hpp>
#include <nimblefix/fix44.hpp>
#include <nimblefix/group.hpp>
#include <nimblefix/schema.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
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

    // Stream NewOrderSingle with repeating group directly via cross-thread Sender!
    const auto sent = sender.send_stream<fix44::NewOrderSingle>([&](auto& w) {
        w.cl_ord_id("GRP-SEND-1")
         .no_party_i_ds(2, [](auto& g) {
             g.entry().party_id("P1").party_id_source('D').party_role(1);
             g.entry().party_id("P2").party_id_source('D').party_role(3);
         })
         .symbol("AAPL")
         .side('1')
         .transact_time(t0)
         .ord_type('1');
    });
    CHECK(sent.ok());

    for (int i = 0; i < 400 && !got_group.load(std::memory_order_acquire); ++i) ::usleep(5000);
    CHECK(got_group.load());

    client.request_stop();
    server.request_stop();
    cli_thread.join();
    srv_thread.join();
    std::filesystem::remove_all(dir);
}

void test_streaming_nested_groups_and_validation() {
    std::array<char, 2048> wire{};
    std::array<nf::FieldView, 64> scratch{};

    // 1. Two-level nested repeating group streaming (NoPartyIDs -> NoPartySubIDs)
    {
        const auto enc = nf::encode_fix44_stream<fix44::NewOrderSingle>(
            nf::SessionHeader{"BUY", "SELL", 2, t0}, wire, [&](auto& w) {
                w.cl_ord_id("STREAM-NESTED-1")
                 .symbol("AAPL")
                 .side('1')
                 .transact_time(t0)
                 .ord_type('1')
                 .no_party_i_ds(2, [](auto& g) {
                     g.entry().party_id("CLIENT1").party_id_source('D').party_role(1);
                     g.entry().party_id("BROKER2").party_id_source('D').party_role(17)
                         .no_party_sub_i_ds(2, [](auto& sg) {
                             sg.entry().party_sub_id("DESK-A").party_sub_id_type(1);
                             sg.entry().party_sub_id("TRADER-B").party_sub_id_type(3);
                         });
                 });
            });
        CHECK(enc.ok());
        if (enc.ok()) {
            nf::FrameView frame{};
            CHECK(nf::inspect_frame({wire.data(), enc.value()}, frame, fix44::Schema::wire_hints()).ok());
            nf::FlatMessageView msg{};
            CHECK(nf::parse_flat_verified(frame, scratch, msg, fix44::Schema::wire_hints()).ok());
            CHECK(nf::validate_schema_message(fix44::Schema::descriptor(), msg, nf::ValidationLevel::strict).ok());
            const auto nos = msg.as<fix44::NewOrderSingle>();
            const auto parties = nos.no_party_i_ds();
            CHECK(parties.ok());
            if (parties.ok()) {
                CHECK(parties.value().size() == 2);
                CHECK(parties.value().at(0).get<448>().value() == "CLIENT1");
                const auto p1 = parties.value().at(1);
                CHECK(p1.get<448>().value() == "BROKER2");
                const auto subs = p1.group<802>();
                CHECK(subs.ok());
                if (subs.ok()) {
                    CHECK(subs.value().size() == 2);
                    CHECK(subs.value().at(0).get<523>().value() == "DESK-A");
                    CHECK(subs.value().at(1).get<523>().value() == "TRADER-B");
                }
            }
        }
    }

    // 2. Group delimiter-first enforcement (setting non-delimiter field first must fail)
    {
        const auto enc = nf::encode_fix44_stream<fix44::NewOrderSingle>(
            nf::SessionHeader{"BUY", "SELL", 3, t0}, wire, [&](auto& w) {
                w.cl_ord_id("STREAM-BAD-DELIM")
                 .symbol("AAPL")
                 .side('1')
                 .transact_time(t0)
                 .ord_type('1')
                 .no_party_i_ds(1, [](auto& g) {
                     g.entry().party_id_source('D').party_role(1);
                 });
            });
        CHECK(!enc.ok());
        CHECK(enc.status().code == nf::ErrorCode::required_field_missing);
        CHECK(enc.status().tag == 448);
    }

    // 3. Entry count mismatch rejection (declared 2, wrote 1)
    {
        const auto enc = nf::encode_fix44_stream<fix44::NewOrderSingle>(
            nf::SessionHeader{"BUY", "SELL", 4, t0}, wire, [&](auto& w) {
                w.cl_ord_id("STREAM-BAD-COUNT")
                 .symbol("AAPL")
                 .side('1')
                 .transact_time(t0)
                 .ord_type('1')
                 .no_party_i_ds(2, [](auto& g) {
                     g.entry().party_id("ONLY-ONE").party_id_source('D').party_role(1);
                 });
            });
        CHECK(!enc.ok());
        CHECK(enc.status().code == nf::ErrorCode::invalid_value);
        CHECK(enc.status().tag == 453);
    }

    // 4. Dynamic loop inside repeating group (runtime count & dynamic entries)
    {
        const auto enc = nf::encode_fix44_stream<fix44::NewOrderSingle>(
            nf::SessionHeader{"BUY", "SELL", 5, t0}, wire, [&](auto& w) {
                w.cl_ord_id("LOOP-1")
                 .symbol("AAPL")
                 .side('1')
                 .transact_time(t0)
                 .ord_type('1')
                 .no_party_i_ds(4, [](auto& g) {
                     for (int i = 0; i < 4; ++i) {
                         const std::string id = "PARTY-LONG-ID-" + std::to_string(i);
                         g.entry().party_id(id).party_id_source('D').party_role(i + 1);
                     }
                 });
            });
        CHECK(enc.ok());
        if (enc.ok()) {
            nf::FrameView frame{};
            CHECK(nf::inspect_frame({wire.data(), enc.value()}, frame, fix44::Schema::wire_hints()).ok());
            nf::FlatMessageView msg{};
            CHECK(nf::parse_flat_verified(frame, scratch, msg, fix44::Schema::wire_hints()).ok());
            CHECK(nf::validate_schema_message(fix44::Schema::descriptor(), msg, nf::ValidationLevel::strict).ok());
            const auto parsed_parties = msg.as<fix44::NewOrderSingle>().no_party_i_ds();
            CHECK(parsed_parties.ok());
            if (parsed_parties.ok()) {
                CHECK(parsed_parties.value().size() == 4);
                CHECK(parsed_parties.value().at(0).get<448>().value() == "PARTY-LONG-ID-0");
                CHECK(parsed_parties.value().at(3).get<448>().value() == "PARTY-LONG-ID-3");
            }
        }
    }
}

void test_stage_b_fast_validate_and_indexed_view() {
    // Copy descriptor with fast_validate cleared so we can assert 100% equivalence
    // between the generated switch-based fast validator and generic validate_members_fast.
    nf::SchemaDescriptor generic_desc = fix44::Schema::descriptor();
    CHECK(generic_desc.fast_validate != nullptr);
    generic_desc.fast_validate = nullptr;

    auto assert_equiv = [&](std::span<const nf::FieldView> fields,
                            std::string_view msg_type,
                            nf::ValidationLevel level) {
        nf::detail::ParsedFieldIndex idx{};
        const auto fast_st = nf::validate_schema_message_for_type(
            fix44::Schema::descriptor(), msg_type, 0, fields, level, &idx);
        const auto gen_st = nf::validate_schema_message_for_type(
            generic_desc, msg_type, 0, fields, level, nullptr);
        CHECK(fast_st.code == gen_st.code);
        CHECK(fast_st.tag == gen_st.tag);
        if (!fast_st.ok()) {
            CHECK(idx.schema_id == nullptr);
        }
    };

    // 1. Valid NewOrderSingle with NoPartyIDs[1] + O(1) ParsedFieldIndex reuse across messages
    std::array<char, 2048> wire{};
    std::array<nf::FieldView, 64> scratch{};
    nf::detail::ParsedFieldIndex idx{};
    {
        const auto enc = nf::encode_fix44_stream<fix44::NewOrderSingle>(
            nf::SessionHeader{"BUY", "SELL", 10, t0}, wire, [&](auto& w) {
                w.cl_ord_id("IDX-1")
                 .symbol("AAPL")
                 .side('1')
                 .transact_time(t0)
                 .order_qty(nf::Decimal{250, 0})
                 .ord_type('2')
                 .price(nf::Decimal{18525, 2})
                 .no_party_i_ds(1, [](auto& g) {
                     g.entry().party_id("DESK1").party_id_source('D').party_role(1);
                 });
            });
        CHECK(enc.ok());
        nf::FrameView fv{};
        CHECK(nf::inspect_frame({wire.data(), enc.value()}, fv, fix44::Schema::wire_hints()).ok());
        nf::FlatMessageView mv{};
        CHECK(nf::parse_flat_verified(fv, scratch, mv, fix44::Schema::wire_hints()).ok());
        mv.bind_index(&idx);
        CHECK(nf::validate_schema_message(fix44::Schema::descriptor(), mv, nf::ValidationLevel::strict).ok());
        CHECK(idx.schema_id == &fix44::Schema::descriptor());
        const auto nos = mv.as<fix44::NewOrderSingle>();
        CHECK(nos.cl_ord_id().value() == "IDX-1");
        CHECK(nos.symbol().value() == "AAPL");
        CHECK(nos.side().value() == '1');
        CHECK(nos.order_qty().value() == nf::Decimal(250, 0));
        CHECK(nos.price().value() == nf::Decimal(18525, 2));
        // Absent optional field must return missing_field in O(1)
        const auto abs_acc = nos.account();
        CHECK(!abs_acc.ok() && abs_acc.status().code == nf::ErrorCode::missing_field && abs_acc.status().tag == 1);
        const auto grp = nos.no_party_i_ds();
        CHECK(grp.ok() && grp.value().size() == 1);
        CHECK(grp.value().at(0).get<448>().value() == "DESK1");
        CHECK(grp.value().at(0).get<447>().value() == 'D');
        CHECK(grp.value().at(0).get<452>().value() == 1);
        std::size_t iter_count = 0;
        for (const auto& e : grp.value()) {
            CHECK(e.get<448>().value() == "DESK1");
            ++iter_count;
        }
        CHECK(iter_count == 1);
    }

    // Reuse same ParsedFieldIndex for a second NewOrderSingle WITHOUT price or parties:
    // generation stamp must ensure old Price(44) and NoPartyIDs(453) are NOT visible!
    {
        const auto enc = nf::encode_fix44_stream<fix44::NewOrderSingle>(
            nf::SessionHeader{"BUY", "SELL", 11, t0}, wire, [&](auto& w) {
                w.cl_ord_id("IDX-2")
                 .symbol("MSFT")
                 .side('2')
                 .transact_time(t0)
                 .ord_type('1');
            });
        CHECK(enc.ok());
        nf::FrameView fv{};
        CHECK(nf::inspect_frame({wire.data(), enc.value()}, fv, fix44::Schema::wire_hints()).ok());
        nf::FlatMessageView mv{};
        CHECK(nf::parse_flat_verified(fv, scratch, mv, fix44::Schema::wire_hints()).ok());
        mv.bind_index(&idx);
        CHECK(nf::validate_schema_message(fix44::Schema::descriptor(), mv, nf::ValidationLevel::strict).ok());
        const auto nos = mv.as<fix44::NewOrderSingle>();
        CHECK(nos.cl_ord_id().value() == "IDX-2");
        CHECK(nos.symbol().value() == "MSFT");
        CHECK(!nos.price().ok() && nos.price().status().code == nf::ErrorCode::missing_field);
        CHECK(!nos.no_party_i_ds().ok() && nos.no_party_i_ds().status().code == nf::ErrorCode::missing_field);
    }

    // 2. Equivalence matrix across error kinds, validation levels, and precedence
    const std::array<nf::FieldView, 7> base_nos{{
        {8, "FIX.4.4", 0}, {9, "60", 10}, {35, "D", 15},
        {11, "O1", 20}, {54, "1", 26}, {60, t0, 31}, {40, "1", 56},
    }};
    assert_equiv(base_nos, "D", nf::ValidationLevel::strict);
    assert_equiv(base_nos, "D", nf::ValidationLevel::required);
    assert_equiv(base_nos, "D", nf::ValidationLevel::structure);

    // Bad timestamp error code must be invalid_timestamp
    {
        auto f = base_nos;
        f[5].value = "NOT-A-TIMESTAMP";
        assert_equiv(f, "D", nf::ValidationLevel::strict);
        const auto st = nf::validate_schema_message_for_type(
            fix44::Schema::descriptor(), "D", 0, f, nf::ValidationLevel::strict, &idx);
        CHECK(st.code == nf::ErrorCode::invalid_timestamp && st.tag == 60);
    }

    // Invalid enum rejected in strict, accepted in required/structure
    {
        auto f = base_nos;
        f[4].value = "Z"; // invalid Side enum
        assert_equiv(f, "D", nf::ValidationLevel::strict);
        assert_equiv(f, "D", nf::ValidationLevel::required);
        assert_equiv(f, "D", nf::ValidationLevel::structure);
    }

    // Duplicate beats earlier bad scalar
    {
        const std::array<nf::FieldView, 8> f{{
            {8, "FIX.4.4", 0}, {9, "60", 10}, {35, "D", 15},
            {11, "O1", 20}, {54, "Z", 26}, {60, t0, 31}, {40, "1", 56}, {11, "DUP", 61},
        }};
        assert_equiv(f, "D", nf::ValidationLevel::strict);
    }

    // Bad group delimiter beats earlier bad scalar
    {
        const std::array<nf::FieldView, 9> f{{
            {8, "FIX.4.4", 0}, {9, "60", 10}, {35, "D", 15},
            {11, "O1", 20}, {54, "Z", 26}, {60, t0, 31}, {40, "1", 56},
            {453, "1", 61}, {447, "D", 67}, // missing delimiter 448!
        }};
        assert_equiv(f, "D", nf::ValidationLevel::strict);
        assert_equiv(f, "D", nf::ValidationLevel::structure);
    }

    // Unknown tag rejected in strict, accepted in required/structure; required beats unknown
    {
        const std::array<nf::FieldView, 7> f{{
            {8, "FIX.4.4", 0}, {9, "60", 10}, {35, "D", 15},
            {99999, "CUSTOM", 20}, {11, "O1", 33}, {54, "1", 39}, {60, t0, 44}, // missing required 40
        }};
        assert_equiv(f, "D", nf::ValidationLevel::strict);
        assert_equiv(f, "D", nf::ValidationLevel::required);
        assert_equiv(f, "D", nf::ValidationLevel::structure);
    }

    // DATA/LENGTH pair validation (EncodedTextLen=354 / EncodedText=355)
    {
        const std::array<nf::FieldView, 8> orphan_data{{
            {8, "FIX.4.4", 0}, {9, "60", 10}, {35, "D", 15},
            {11, "O1", 20}, {54, "1", 26}, {60, t0, 31}, {40, "1", 56}, {355, "abc", 61},
        }};
        assert_equiv(orphan_data, "D", nf::ValidationLevel::strict);
        const std::array<nf::FieldView, 9> mismatch_len{{
            {8, "FIX.4.4", 0}, {9, "60", 10}, {35, "D", 15},
            {11, "O1", 20}, {54, "1", 26}, {60, t0, 31}, {40, "1", 56},
            {354, "5", 61}, {355, "abc", 67},
        }};
        assert_equiv(mismatch_len, "D", nf::ValidationLevel::strict);
    }
}

} // namespace

int main() {
    test_two_level_nested_repeating_group_parsing_and_validation();
    test_sender_owned_message_repeating_group_relocation();
    test_streaming_nested_groups_and_validation();
    test_stage_b_fast_validate_and_indexed_view();
    if (failures != 0) {
        std::cerr << failures << " nested group test(s) failed\n";
        return 1;
    }
    std::cout << "nested repeating group and cross-thread group relocation tests passed\n";
    return 0;
}
