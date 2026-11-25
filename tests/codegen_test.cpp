#include "../tools/schema/schema_model.hpp"
#include "test_process_utils.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
namespace cg = nimblefix::codegen;
namespace tu = nimblefix::test_util;

int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #cond << '\n'; ++g_failures; } \
} while (false)

#define EXPECT_ERROR(expr, substr) do { \
    bool caught = false; \
    try { (void)(expr); } catch (const cg::SchemaError& e) { \
        caught = true; \
        if (std::string{e.what()}.find(substr) == std::string::npos) { \
            std::cerr << __FILE__ << ':' << __LINE__ << ": expected error containing '" << substr \
                      << "', got: " << e.what() << '\n'; \
            ++g_failures; \
        } \
    } \
    if (!caught) { \
        std::cerr << __FILE__ << ':' << __LINE__ << ": expected SchemaError containing '" << substr << "'\n"; \
        ++g_failures; \
    } \
} while (false)

std::string schema_xml(std::string_view fields, std::string_view messages,
                       std::string_view root_attrs = "type=\"FIX\" major=\"4\" minor=\"4\"") {
    return "<fix " + std::string{root_attrs} + ">" + std::string{fields} + std::string{messages} + "</fix>";
}

std::string run_gen(const std::filesystem::path& gen_bin,
                    const std::vector<std::string>& args,
                    int expected_rc = 0) {
    std::vector<std::string> argv{gen_bin.string()};
    argv.insert(argv.end(), args.begin(), args.end());
    const auto res = tu::run_command(argv, {}, {}, {}, false, 30'000);
    if (res.exit_code != expected_rc) {
        std::cerr << "generator exit=" << res.exit_code << " expected=" << expected_rc
                  << "\nstdout=" << res.stdout_text << "\nstderr=" << res.stderr_text << '\n';
        ++g_failures;
    }
    return res.stderr_text;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path gen_bin;
    std::filesystem::path root = std::filesystem::current_path();
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        if (a == "--generator" && i + 1 < argc) gen_bin = std::filesystem::absolute(argv[++i]);
        else if (a == "--root" && i + 1 < argc) root = std::filesystem::absolute(argv[++i]);
    }
    if (gen_bin.empty()) {
        std::cerr << "usage: codegen-test --generator PATH [--root DIR]\n";
        return 2;
    }

    tu::TempDir tmp("nimblefix-codegen-");
    auto write = [&](const std::string& name, std::string_view text) {
        const auto p = tmp.path / name;
        tu::write_file_bytes(p, text);
        return p;
    };

    // 1. Sample output determinism, metadata, and no absolute source path leak
    {
        const auto src = root / "schemas" / "fix44-sample.xml";
        const auto out1 = tmp.path / "sample1.hpp";
        const auto out2 = tmp.path / "sample2.hpp";
        run_gen(gen_bin, {"--output", out1.string(), "--namespace", "nimblefix::fix44_sample", src.string()});
        run_gen(gen_bin, {"--output", out2.string(), "--namespace", "nimblefix::fix44_sample", src.string()});
        const std::string first = tu::read_file_bytes(out1);
        const std::string second = tu::read_file_bytes(out2);
        CHECK(first == second);
        CHECK(first.find("struct NewOrderSingle") != std::string::npos);
        CHECK(first.find("class NewOrderSingleStreamWriter") != std::string::npos);
        CHECK(first.find("struct Schema") != std::string::npos);
        CHECK(first.find("version = \"4.4.0\"") != std::string::npos);
        CHECK(first.find("cl_ord_id(V&& value)") != std::string::npos);
        CHECK(first.find("set<11>(std::forward<V>(value))") != std::string::npos);
        CHECK(first.find("WireRank<nimblefix::fix44_sample::NewOrderSinglePolicy, 11>") != std::string::npos);
        CHECK(first.find("FieldWireType<nimblefix::fix44_sample::NewOrderSinglePolicy, 60>") != std::string::npos);
        CHECK(first.find("WireType::utc_timestamp") != std::string::npos);
        CHECK(first.find("NewOrderSingle_required_tags") != std::string::npos);
        CHECK(first.find("TypedMessageView<Schema>") != std::string::npos);
        CHECK(first.find("explicit NewOrderSingleView(const ::nimblefix::FlatMessageView& flat)") != std::string::npos);
        CHECK(first.find("enum_value_allowed") != std::string::npos);
        CHECK(first.find("EnumValueDescriptor") != std::string::npos);
        CHECK(first.find(src.string()) == std::string::npos);

        // write_if_changed mtime stability
        const auto mt1 = std::filesystem::last_write_time(out1);
        run_gen(gen_bin, {"--output", out1.string(), "--namespace", "nimblefix::fix44_sample", src.string()});
        CHECK(std::filesystem::last_write_time(out1) == mt1);
    }

    // 2. Multi-input merge order independence & deduplication
    {
        const auto f = write("fields.xml", schema_xml("<fields><field number=\"11\" name=\"ClOrdID\" type=\"STRING\"/></fields>", ""));
        const auto m = write("messages.xml", schema_xml("", "<messages><message name=\"NewOrderSingle\" msgtype=\"D\"><field name=\"ClOrdID\" required=\"Y\"/></message></messages>"));
        const auto o1 = tmp.path / "merge1.hpp";
        const auto o2 = tmp.path / "merge2.hpp";
        run_gen(gen_bin, {"--output", o1.string(), "--namespace", "custom::schema", f.string(), m.string()});
        run_gen(gen_bin, {"--output", o2.string(), "--namespace", "custom::schema", m.string(), f.string()});
        CHECK(tu::read_file_bytes(o1) == tu::read_file_bytes(o2));

        const auto dup = write("dup.xml", schema_xml(
            "<fields><field number=\"11\" name=\"ClOrdID\" type=\"STRING\"/></fields>",
            "<messages><message name=\"NewOrderSingle\" msgtype=\"D\"><field name=\"ClOrdID\" required=\"Y\"/></message></messages>"));
        const auto s = cg::load_schema({dup, dup});
        CHECK(s.fields_by_name.size() == 1 && s.messages_by_name.size() == 1);
    }

    // 3. Conflicting field / message / unknown ref / limits / managed tags / DTD / namespaces
    {
        const auto c1 = write("c1.xml", schema_xml("<fields><field number=\"11\" name=\"ClOrdID\" type=\"STRING\"/></fields>", ""));
        const auto c2 = write("c2.xml", schema_xml("<fields><field number=\"11\" name=\"ClOrdID\" type=\"CHAR\"/></fields>", ""));
        EXPECT_ERROR(cg::load_schema({c2, c1}), "conflicting field definition");

        const auto m1 = write("m1.xml", schema_xml("<fields><field number=\"11\" name=\"ClOrdID\" type=\"STRING\"/></fields>",
            "<messages><message name=\"NewOrderSingle\" msgtype=\"D\"><field name=\"ClOrdID\" required=\"Y\"/></message></messages>"));
        const auto m2 = write("m2.xml", schema_xml("<fields><field number=\"11\" name=\"ClOrdID\" type=\"STRING\"/></fields>",
            "<messages><message name=\"NewOrderSingle\" msgtype=\"D\"><field name=\"ClOrdID\" required=\"N\"/></message></messages>"));
        EXPECT_ERROR(cg::load_schema({m1, m2}), "conflicting message definition");

        const auto unk = write("unk.xml", schema_xml("<fields/>",
            "<messages><message name=\"NewOrderSingle\" msgtype=\"D\"><field name=\"NoSuchField\" required=\"Y\"/></message></messages>"));
        EXPECT_ERROR(cg::load_schema({unk}), "unknown field reference 'NoSuchField'");

        const auto oor = write("oor.xml", schema_xml("<fields><field number=\"1000000000\" name=\"CustomField\" type=\"STRING\"/></fields>", ""));
        EXPECT_ERROR(cg::load_schema({oor}), "field number must be between 1 and 999999999");

        const auto mg = write("mg.xml", schema_xml("<fields><field number=\"34\" name=\"MsgSeqNum\" type=\"INT\"/></fields>",
            "<messages><message name=\"InvalidAppMessage\" msgtype=\"Z\"><field name=\"MsgSeqNum\" required=\"Y\"/></message></messages>"));
        EXPECT_ERROR(cg::load_schema({mg}), "engine-managed FIX tag 34");

        const auto dtd1 = write("dtd1.xml", "<!DOCTYPE fix [<!ENTITY x \"bad\">]><fix><fields/><messages/></fix>");
        EXPECT_ERROR(cg::load_schema({dtd1}), "DOCTYPE");

        const auto nsxml = write("ns.xml", "<fix xmlns=\"urn:fix\"><fields/><messages/></fix>");
        EXPECT_ERROR(cg::load_schema({nsxml}), "XML namespaces are not supported");

        const auto bad_msg = write("badmsg.xml", schema_xml(
            "<fields><field number=\"11\" name=\"ClOrdID\" type=\"STRING\"/></fields>",
            "<messages><message name=\"class\" msgtype=\"D\"><field name=\"ClOrdID\" required=\"Y\"/></message></messages>"));
        EXPECT_ERROR(cg::load_schema({bad_msg}), "invalid C++");

        const auto rset = write("rset.xml", schema_xml(
            "<fields><field number=\"58\" name=\"Status\" type=\"STRING\"/></fields>",
            "<messages><message name=\"CustomMessage\" msgtype=\"Z\"><field name=\"Status\" required=\"N\"/></message></messages>"));
        EXPECT_ERROR(cg::load_schema({rset}), "reserved builder setter");

        const auto dunder = write("dunder.xml", schema_xml(
            "<fields><field number=\"11\" name=\"ClOrdID\" type=\"STRING\"/></fields>",
            "<messages><message name=\"Bad__Name\" msgtype=\"D\"><field name=\"ClOrdID\" required=\"Y\"/></message></messages>"));
        EXPECT_ERROR(cg::load_schema({dunder}), "non-reserved ASCII identifier");

        const auto nested_err = write("nested_err.xml", schema_xml(
            "<fields><field number=\"11\" name=\"ClOrdID\" type=\"STRING\"/></fields>",
            "<messages><message name=\"NewOrderSingle\" msgtype=\"D\"><field name=\"ClOrdID\" required=\"Y\"><extension/></field></message></messages>"));
        EXPECT_ERROR(cg::load_schema({nested_err}), "unexpected <extension>");

        std::string deep = "<fix>";
        for (std::size_t i = 0; i < cg::kMaxXmlDepth + 2; ++i) deep += "<a>";
        for (std::size_t i = 0; i < cg::kMaxXmlDepth + 2; ++i) deep += "</a>";
        deep += "</fix>";
        const auto deep_p = write("deep.xml", deep);
        EXPECT_ERROR(cg::load_schema({deep_p}), "nesting depth exceeds");

        // CLI rejects reserved namespace and core symbol collisions
        const auto col_xml = write("col.xml", schema_xml(
            "<fields><field number=\"11\" name=\"ClOrdID\" type=\"STRING\"/></fields>",
            "<messages><message name=\"MessageBuilder\" msgtype=\"D\"><field name=\"ClOrdID\" required=\"Y\"/></message></messages>"));
        const std::string err_col = run_gen(gen_bin, {"--output", (tmp.path / "col.hpp").string(), "--namespace", "nimblefix", col_xml.string()}, 2);
        CHECK(err_col.find("collides with nimblefix core symbol") != std::string::npos);
        const std::string err_ns = run_gen(gen_bin, {"--output", (tmp.path / "std.hpp").string(), "--namespace", "std::bad", col_xml.string()}, 2);
        CHECK(err_ns.find("std namespace") != std::string::npos);
    }

    // 4. Groups, DATA/LENGTH pairs, extended legacy types, and all 5 standard dictionaries
    {
        const auto gxml = write("group.xml", schema_xml(
            "<fields><field number=\"11\" name=\"ClOrdID\" type=\"STRING\"/>"
            "<field number=\"453\" name=\"NoPartyIDs\" type=\"NUMINGROUP\"/>"
            "<field number=\"448\" name=\"PartyID\" type=\"STRING\"/>"
            "<field number=\"802\" name=\"NoPartySubIDs\" type=\"NUMINGROUP\"/>"
            "<field number=\"523\" name=\"PartySubID\" type=\"STRING\"/></fields>",
            "<messages><message name=\"NewOrderSingle\" msgtype=\"D\">"
            "<field name=\"ClOrdID\" required=\"Y\"/><group name=\"NoPartyIDs\" required=\"N\">"
            "<field name=\"PartyID\" required=\"Y\"/><group name=\"NoPartySubIDs\" required=\"N\">"
            "<field name=\"PartySubID\" required=\"Y\"/></group></group></message></messages>"));
        const auto gout = tmp.path / "group.hpp";
        run_gen(gen_bin, {"--output", gout.string(), "--namespace", "nimblefix::fixture", gxml.string()});
        const std::string txt = tu::read_file_bytes(gout);
        CHECK(txt.find("class NoPartyIDsGroupStreamScope") != std::string::npos);
        CHECK(txt.find("class NoPartySubIDsGroupStreamScope") != std::string::npos);
        CHECK(txt.find("no_party_i_ds(std::size_t count, Fn&& fn)") != std::string::npos);

        const auto std_dir = root / "schemas" / "standard";
        for (const char* name : {"FIX42.xml", "FIX43.xml", "FIX44.xml", "FIXT11.xml", "FIX50SP2.xml"}) {
            const auto p = std_dir / name;
            CHECK(std::filesystem::is_regular_file(p));
            const auto s = cg::load_schema({p});
            CHECK(!s.fields_by_name.empty() && !s.messages_by_name.empty());
            if (std::string_view{name} == "FIX43.xml") {
                const auto& ms = s.expanded_messages.at("RegistrationInstructions");
                int cnt = 0;
                for (const auto& m : ms) if (m.field_name == "OwnershipType") ++cnt;
                CHECK(cnt == 1);
            }
        }
    }

    // 5. Log-driven schema pruning (--from-fix-log and --keep-all-fields)
    {
        const auto fix44 = root / "schemas" / "standard" / "FIX44.xml";
        const auto pipe_log = write("venue.log",
            "2026-11-25 08:00:00 OUT 8=FIX.4.4|9=140|35=D|49=BUY|56=SELL|34=2|52=20261125-08:00:00.000|"
            "11=ORD-1|55=AAPL|54=1|60=20261125-08:00:00.000|38=100|40=2|44=185.25|"
            "453=1|448=CLIENT1|447=D|452=1|10=000|\n");
        const std::string soh_raw = std::string{"\x00\x01HEADER_NOISE", 14} +
            "8=FIX.4.4\x01" "9=90\x01" "35=8\x01" "49=SELL\x01" "56=BUY\x01" "34=2\x01" "52=20261125-08:00:00.001\x01"
            "37=EX1\x01" "17=E1\x01" "150=0\x01" "39=0\x01" "55=AAPL\x01" "54=1\x01" "151=100\x01" "14=0\x01" "6=0\x01"
            "355=raw\x01" "10=000\x01";
        const auto soh_log = write("session.nfxj", soh_raw);

        const auto obs = cg::scan_fix_logs({pipe_log, soh_log});
        CHECK(obs.contains("D") && obs.contains("8"));
        CHECK(obs.at("D").contains(44) && obs.at("D").contains(448) && obs.at("8").contains(355));

        const auto full = cg::load_schema({fix44});
        auto [pruned, stats] = cg::prune_schema_from_observations(full, obs, false);
        CHECK(stats.messages_after < 15 && stats.fields_after < 200);
        CHECK(pruned.messages_by_name.contains("NewOrderSingle"));
        CHECK(pruned.messages_by_name.contains("ExecutionReport"));
        CHECK(pruned.messages_by_name.contains("Logon"));
        CHECK(!pruned.messages_by_name.contains("QuoteRequest"));

        std::set<std::uint32_t> nos_tags, er_tags;
        for (const auto& m : pruned.expanded_messages.at("NewOrderSingle")) nos_tags.insert(pruned.fields_by_name.at(m.field_name).number);
        for (const auto& m : pruned.expanded_messages.at("ExecutionReport")) er_tags.insert(pruned.fields_by_name.at(m.field_name).number);
        CHECK(nos_tags.contains(11) && nos_tags.contains(44) && nos_tags.contains(453));
        CHECK(!nos_tags.contains(1) && !nos_tags.contains(152));
        CHECK(er_tags.contains(355) && er_tags.contains(354));

        const auto pout = tmp.path / "pruned_venue.hpp";
        run_gen(gen_bin, {"--output", pout.string(), "--namespace", "venue::pruned",
                          "--from-fix-log", pipe_log.string(), "--from-fix-log", soh_log.string(), fix44.string()});
        const std::string ptxt = tu::read_file_bytes(pout);
        CHECK(ptxt.find("struct NewOrderSingle") != std::string::npos);
        CHECK(ptxt.find("struct QuoteRequest") == std::string::npos);
        CHECK(ptxt.find("fast_validate_message") != std::string::npos);

        auto [pruned_all, _s2] = cg::prune_schema_from_observations(full, obs, true);
        bool has_account = false;
        for (const auto& m : pruned_all.expanded_messages.at("NewOrderSingle")) {
            if (pruned_all.fields_by_name.at(m.field_name).number == 1) has_account = true;
        }
        CHECK(has_account);
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " codegen test(s) failed\n";
        return 1;
    }
    std::cout << "C++ schema codegen, validation, standard dictionaries, and --from-fix-log tests passed\n";
    return 0;
}
