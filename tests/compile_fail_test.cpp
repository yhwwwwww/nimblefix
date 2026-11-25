#include "test_process_utils.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace tu = nimblefix::test_util;

namespace {

struct Case {
    std::string name;
    std::string body;
    std::string diagnostic;
    int group; // 0 = core, 1 = generated, 2 = engine
};

} // namespace

int main(int argc, char** argv) {
    std::string cxx = std::getenv("CXX") ? std::getenv("CXX") : "c++";
    std::filesystem::path gen_bin;
    std::filesystem::path root = std::filesystem::current_path();
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        if (a == "--cxx" && i + 1 < argc) cxx = argv[++i];
        else if (a == "--generator" && i + 1 < argc) gen_bin = std::filesystem::absolute(argv[++i]);
        else if (a == "--root" && i + 1 < argc) root = std::filesystem::absolute(argv[++i]);
    }
    if (gen_bin.empty()) {
        std::cerr << "usage: compile_fail-test --generator PATH [--cxx COMPILER] [--root DIR]\n";
        return 2;
    }

    const std::vector<Case> cases = {
        // Core cases
        {"managed_tag", R"(auto x = nf::message<"D">().set<34>(1);)", "engine managed", 0},
        {"managed_possdup", R"(auto x = nf::message<"D">().set<43>(true);)", "engine managed", 0},
        {"managed_origtime", R"(auto x = nf::message<"D">().set<122>("20261031-00:00:00");)", "engine managed", 0},
        {"managed_processed", R"(auto x = nf::message<"D">().set<369>(1);)", "engine managed", 0},
        {"invalid_tag", R"(auto x = nf::message<"D">().set<0>(1);)", "FIX tags must", 0},
        {"duplicate_flat_tag", R"(auto x = nf::message<"D">().set<11>("a").set<11>("b");)", "same tag twice", 0},
        {"runtime_tag", R"(int tag = 11; auto x = nf::message<"D">().set(tag, "a");)", "set", 0},
        {"binary_float", R"(auto x = nf::message<"D">().set<44>(1.25);)", "unsupported static FIX", 0},
        {"invalid_decimal_literal", R"(constexpr auto x = "not-a-price"_dec;)", "constant expression", 0},
        {"unshaped_set", R"(nf::FixedMessage<"D", nf::Fields<nf::Field<11, nf::FieldKind::string>>> x; (void)x.set<55>("a");)", "must name a field", 0},
        {"wrong_fixed_type", R"(nf::FixedMessage<"D", nf::Fields<nf::Field<11, nf::FieldKind::string>>> x; (void)x.set<11>(4);)", "value type does not match", 0},
        // Generated cases
        {"quantity_string", R"(auto& w = *static_cast<nf::fix44_sample::NewOrderSingle::Writer*>(nullptr); (void)w.order_qty("bad");)", "generated FIX field type", 1},
        {"tagged_price_string", R"(auto& w = *static_cast<nf::fix44_sample::NewOrderSingle::Writer*>(nullptr); (void)w.set<44>("bad");)", "generated FIX field type", 1},
        {"side_integer", R"(auto& w = *static_cast<nf::fix44_sample::NewOrderSingle::Writer*>(nullptr); (void)w.side(1);)", "generated FIX field type", 1},
        {"typed_generic_callback", R"(nf::Engine<nf::fix44_sample::Schema>& e=*static_cast<nf::Engine<nf::fix44_sample::Schema>*>(nullptr); (void)e.on_message([](nf::SessionContext&, const nf::FlatMessageView&) { return nf::Status::success(); });)", "on_message", 1},
        {"typed_wrong_view", R"(nf::Engine<nf::fix44_sample::Schema>& e=*static_cast<nf::Engine<nf::fix44_sample::Schema>*>(nullptr); (void)e.on<nf::fix44_sample::NewOrderSingle>([](nf::SessionContext&, int) { return nf::Status::success(); });)", "on", 1},
        {"unknown_generated_field", R"(auto& w = *static_cast<nf::fix44_sample::NewOrderSingle::Writer*>(nullptr); (void)w.set<900001>("x");)", "not allowed", 1},
        // Engine cases
        {"unsupported_engine_schema", R"((void)sizeof(nf::Engine<int>);)", "typed Engine Schema must expose descriptor()", 2},
        {"wrong_message_callback", R"(nf::Engine<>& engine = *static_cast<nf::Engine<>*>(nullptr); (void)engine.on_message([](int) {});)", "on_message", 2},
        {"invalid_execution_policy", R"((void)sizeof(nf::Engine<nf::FlatSchema, int>);)", "Engine Policy must", 2},
        {"retained_session_context", R"(void save(nf::SessionContext&); auto f=[](nf::SessionContext& context) { auto retained = context; };)", "deleted", 2},
        {"wrong_active_callback", R"(nf::Engine<>& engine = *static_cast<nf::Engine<>*>(nullptr); (void)engine.on_session_active([]() {});)", "on_session_active", 2},
    };

    tu::TempDir tmp("nimblefix-compile-");
    const auto source = tmp.path / "case.cpp";
    const auto generated = tmp.path / "nimblefix" / "fix44_sample.hpp";
    const std::string prefix = "#include <nimblefix/message.hpp>\nnamespace nf=nimblefix;\nusing namespace nf::literals;\n";

    // Positive control
    tu::write_file_bytes(source, prefix + "int main() { auto x = nf::message<\"D\">().set<11>(\"ok\"); }\n");
    std::vector<std::string> base_cmd{cxx, "-std=c++20", "-fsyntax-only", "-I", (root / "include").string(), source.string()};
    const auto pos = tu::run_command(base_cmd);
    if (!pos.ok()) {
        std::cerr << "positive control failed:\n" << pos.stderr_text << '\n';
        return 1;
    }

    // Generate fix44_sample.hpp via native C++ generator
    const auto gen_res = tu::run_command({
        gen_bin.string(), "--namespace", "nimblefix::fix44_sample",
        "--output", generated.string(), (root / "schemas" / "fix44-sample.xml").string(),
    });
    if (!gen_res.ok()) {
        std::cerr << "schema generator failed:\n" << gen_res.stderr_text << '\n';
        return 1;
    }

    base_cmd.push_back("-I");
    base_cmd.push_back(tmp.path.string());

    for (const auto& c : cases) {
        std::string inc;
        if (c.group == 1) inc = "#include <nimblefix/fix44_sample.hpp>\n#include <nimblefix/engine.hpp>\n";
        else if (c.group == 2) inc = "#include <nimblefix/engine.hpp>\n";
        tu::write_file_bytes(source, prefix + inc + "int main() { " + c.body + " }\n");
        const auto res = tu::run_command(base_cmd);
        if (res.ok() || res.stderr_text.find(c.diagnostic) == std::string::npos) {
            std::cerr << c.name << ": expected compile failure containing '" << c.diagnostic
                      << "'\nexit=" << res.exit_code << "\nstderr:\n" << res.stderr_text << '\n';
            return 1;
        }
    }

    std::cout << cases.size() << " compile-fail contracts passed (" << cxx << ")\n";
    return 0;
}
