#include "test_process_utils.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
namespace tu = nimblefix::test_util;

namespace {

std::string lua_string(const std::string& v) {
    std::string out = "\"";
    for (char c : v) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string sh_single_quote(const std::string& v) {
    std::string out = "'";
    for (char c : v) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string invoke_ok(const std::vector<std::string>& cmd,
                      const fs::path& cwd,
                      const std::map<std::string, std::string>& env,
                      int timeout_ms = 120'000) {
    const auto r = tu::run_command(cmd, cwd, env, {}, true, timeout_ms);
    if (!r.ok()) {
        std::ostringstream oss;
        for (const auto& a : cmd) oss << a << ' ';
        throw std::runtime_error("command failed (exit=" + std::to_string(r.exit_code) +
                                 ", sig=" + std::to_string(r.term_signal) + "): " +
                                 oss.str() + "\n" + r.stdout_text);
    }
    return r.stdout_text;
}

void build_project(const std::string& xmake,
                   const fs::path& project,
                   const fs::path& output,
                   const std::map<std::string, std::string>& env) {
    std::vector<std::string> configure = {
        xmake, "f", "-P", project.string(), "-o", output.string(), "-m", "release", "-y"
    };
    if (const char* cc = std::getenv("CC"); cc && *cc) configure.push_back(std::string{"--cc="} + cc);
    if (const char* cxx = std::getenv("CXX"); cxx && *cxx) {
        configure.push_back(std::string{"--cxx="} + cxx);
        configure.push_back(std::string{"--ld="} + cxx);
    }
    invoke_ok(configure, project, env);
    invoke_ok({xmake, "-P", project.string(), "-j", "1"}, project, env);
}

void write_project(const fs::path& project,
                   const fs::path& rule,
                   const fs::path& include_dir,
                   std::string_view fields_xml,
                   std::string_view message_xml,
                   std::string_view ns = "external::venue",
                   std::string_view header = "order.hpp",
                   const std::optional<fs::path>& generator = std::nullopt,
                   bool reverse = false) {
    const std::string inputs = reverse
        ? (lua_string("messages.xml") + ", " + lua_string("fields.xml"))
        : (lua_string("fields.xml") + ", " + lua_string("messages.xml"));
    std::string gen_line;
    if (generator) {
        gen_line = "    set_values(\"nimblefix.schema.generator\", " + lua_string(generator->string()) + ")\n";
    }
    const std::string xmake_lua =
        "set_project(\"nimblefix-schema-install-test\")\n"
        "set_languages(\"cxx20\")\n"
        "includes(" + lua_string(rule.string()) + ")\n"
        "add_includedirs(" + lua_string(include_dir.string()) + ")\n\n"
        "target(\"schema-consumer\")\n"
        "    set_kind(\"binary\")\n"
        "    add_rules(\"nimblefix.schema\")\n"
        "    set_values(\"nimblefix.schema.namespace\", " + lua_string(std::string{ns}) + ")\n"
        "    set_values(\"nimblefix.schema.header\", " + lua_string(std::string{header}) + ")\n"
        "    set_values(\"nimblefix.schema.xml_files\", {" + inputs + "})\n" +
        gen_line +
        "    add_files(\"main.cpp\")\n"
        "target_end()\n";
    tu::write_file_bytes(project / "xmake.lua", xmake_lua);
    tu::write_file_bytes(project / "fields.xml", fields_xml);
    tu::write_file_bytes(project / "messages.xml", message_xml);
    const std::string main_cpp =
        "#include <array>\n#include \"" + std::string{header} + "\"\n"
        "int main() { std::array<char, 128> buf{}; "
        "auto r = nimblefix::encode_body_stream<" + std::string{ns} + "::CustomOrder>(buf, [](auto& w) { w.cl_ord_id(\"EXTERNAL\"); }); "
        "return r.ok() ? 0 : 1; }\n";
    tu::write_file_bytes(project / "main.cpp", main_cpp);
}

std::string extract_fingerprint(const fs::path& header) {
    const std::string text = tu::read_file_bytes(header);
    static const std::regex re("normalized fingerprint ([0-9a-f]{64})");
    std::smatch m;
    if (!std::regex_search(text, m, re)) {
        throw std::runtime_error("generated header has no normalized fingerprint: " + header.string());
    }
    return m[1].str();
}

std::vector<fs::path> find_named(const fs::path& root, std::string_view filename, bool exec_only = false) {
    std::vector<fs::path> out;
    if (!fs::exists(root)) return out;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (e.is_regular_file() && e.path().filename() == filename) {
            if (!exec_only || ::access(e.path().c_str(), X_OK) == 0) out.push_back(e.path());
        }
    }
    return out;
}

std::size_t count_lines(const fs::path& p) {
    if (!fs::is_regular_file(p)) return 0;
    const std::string s = tu::read_file_bytes(p);
    std::size_t n = 0;
    for (char c : s) if (c == '\n') ++n;
    return n;
}

void bump_mtime(const fs::path& p, long delta_sec = 2) {
    timespec now{};
    ::clock_gettime(CLOCK_REALTIME, &now);
    timespec ts[2]{};
    ts[0].tv_sec = now.tv_sec + delta_sec;
    ts[0].tv_nsec = now.tv_nsec;
    ts[1] = ts[0];
    if (::utimensat(AT_FDCWD, p.c_str(), ts, 0) != 0) {
        throw std::runtime_error("utimensat failed for " + p.string());
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::string xmake = "xmake";
        fs::path rule;
        fs::path generator;
        fs::path include_dir;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--xmake" && i + 1 < argc) xmake = argv[++i];
            else if (arg == "--rule" && i + 1 < argc) rule = argv[++i];
            else if (arg == "--generator" && i + 1 < argc) generator = argv[++i];
            else if (arg == "--include-dir" && i + 1 < argc) include_dir = argv[++i];
            else throw std::runtime_error("unknown argument: " + std::string{arg});
        }
        if (rule.empty() || generator.empty() || include_dir.empty()) {
            throw std::runtime_error("usage: xmake_schema-test --rule PATH --generator PATH --include-dir PATH [--xmake PATH]");
        }
        rule = fs::weakly_canonical(rule);
        generator = fs::weakly_canonical(generator);
        include_dir = fs::weakly_canonical(include_dir);
        for (const auto& p : {rule, generator, include_dir}) {
            if (!fs::exists(p)) throw std::runtime_error("missing required path: " + p.string());
        }

        tu::TempDir tmp("nimblefix-xmake-schema-");
        const fs::path project = tmp.path / "external-project";
        const fs::path build_dir = tmp.path / "external-build";
        fs::create_directories(project);

        std::map<std::string, std::string> env = {
            {"XMAKE_CONFIGDIR", (tmp.path / "xmake-config").string()},
            {"XMAKE_GLOBALDIR", (tmp.path / "xmake-global").string()},
        };

        std::string fields_xml =
            "<?xml version=\"1.0\"?>\n"
            "<fix type=\"FIX\" major=\"4\" minor=\"4\">\n"
            "  <fields><field number=\"11\" name=\"ClOrdID\" type=\"STRING\">\n"
            "    <value enum=\"EXTERNAL\" description=\"Initial fingerprint marker\"/>\n"
            "  </field></fields>\n"
            "</fix>\n";
        const std::string message_xml =
            "<?xml version=\"1.0\"?>\n"
            "<fix type=\"FIX\" major=\"4\" minor=\"4\">\n"
            "  <messages><message name=\"CustomOrder\" msgtype=\"Z\" msgcat=\"app\">\n"
            "    <field name=\"ClOrdID\" required=\"Y\"/>\n"
            "  </message></messages>\n"
            "</fix>\n";

        // If the rule has a sibling installed nimblefix-schema-gen, exercise default lookup first.
        const bool has_default_sibling =
            fs::is_regular_file(rule.parent_path() / ".." / ".." / ".." / "bin" / "nimblefix-schema-gen") ||
            fs::is_regular_file(rule.parent_path() / ".." / "bin" / "nimblefix-schema-gen");
        write_project(project, rule, include_dir, fields_xml, message_xml,
                      "external::venue", "order.hpp",
                      has_default_sibling ? std::nullopt : std::optional<fs::path>{generator});
        build_project(xmake, project, build_dir, env);

        const auto generated = find_named(build_dir, "order.hpp");
        if (generated.size() != 1) throw std::runtime_error("expected one generated order.hpp");
        const fs::path output = generated[0];
        const std::string original_bytes = tu::read_file_bytes(output);
        const std::string original_fp = extract_fingerprint(output);
        if (original_bytes.find("CustomOrderStreamWriter") == std::string::npos ||
            original_bytes.find("FieldWireType") == std::string::npos) {
            throw std::runtime_error("generated header missing typed schema metadata");
        }

        // Wrap generator in a POSIX shell call-counter (zero Python).
        const fs::path call_log = tmp.path / "generator-calls.txt";
        const fs::path wrapper = project / "counted-generator.sh";
        tu::write_file_bytes(wrapper,
            "#!/bin/sh\n"
            "printf 'call\\n' >> " + sh_single_quote(call_log.string()) + "\n"
            "exec " + sh_single_quote(generator.string()) + " \"$@\"\n");
        if (::chmod(wrapper.c_str(), 0755) != 0) throw std::runtime_error("chmod wrapper failed");

        write_project(project, rule, include_dir, fields_xml, message_xml,
                      "external::venue", "order.hpp", wrapper);
        build_project(xmake, project, build_dir, env);
        if (count_lines(call_log) != 1) {
            throw std::runtime_error("changing generator dependency did not trigger exactly 1 generation");
        }

        // No-op rebuild must not re-invoke generator.
        build_project(xmake, project, build_dir, env);
        if (count_lines(call_log) != 1) {
            throw std::runtime_error("unchanged XML and namespace reran the schema generator");
        }
        if (tu::read_file_bytes(output) != original_bytes) {
            throw std::runtime_error("no-op build changed generated output");
        }

        // XML edit + mtime bump must regenerate once and change fingerprint.
        const std::string old_marker = "Initial fingerprint marker";
        const std::string new_marker = "Changed fingerprint marker with new length";
        fields_xml.replace(fields_xml.find(old_marker), old_marker.size(), new_marker);
        tu::write_file_bytes(project / "fields.xml", fields_xml);
        bump_mtime(project / "fields.xml", 2);
        build_project(xmake, project, build_dir, env);

        const std::string changed_fp = extract_fingerprint(output);
        const std::string changed_text = tu::read_file_bytes(output);
        if (changed_fp == original_fp || changed_text.find("Changed fingerprint marker") == std::string::npos) {
            throw std::runtime_error("XML edit did not regenerate fingerprint and enum metadata");
        }
        if (count_lines(call_log) != 2) {
            throw std::runtime_error("XML edit did not cause exactly one regeneration");
        }

        // Reverse XML input order must produce byte-identical output.
        const std::string before_reverse = tu::read_file_bytes(output);
        write_project(project, rule, include_dir, fields_xml, message_xml,
                      "external::venue", "order.hpp", wrapper, true);
        build_project(xmake, project, build_dir, env);
        if (tu::read_file_bytes(output) != before_reverse || extract_fingerprint(output) != changed_fp) {
            throw std::runtime_error("multi-file XML generation depends on input order");
        }

        // Namespace rename invalidates output.
        write_project(project, rule, include_dir, fields_xml, message_xml,
                      "external::renamed", "order.hpp", wrapper, true);
        build_project(xmake, project, build_dir, env);
        if (tu::read_file_bytes(output).find("namespace external::renamed") == std::string::npos) {
            throw std::runtime_error("namespace change not reflected in generated header");
        }

        // Header filename rename produces new header and compiles consumer.
        write_project(project, rule, include_dir, fields_xml, message_xml,
                      "external::renamed", "renamed.hpp", wrapper, true);
        build_project(xmake, project, build_dir, env);
        const auto renamed = find_named(build_dir, "renamed.hpp");
        if (renamed.size() != 1 ||
            tu::read_file_bytes(renamed[0]).find("namespace external::renamed") == std::string::npos) {
            throw std::runtime_error("header path change did not produce requested generated header");
        }
        const auto binaries = find_named(build_dir, "schema-consumer", true);
        if (binaries.size() != 1) throw std::runtime_error("expected one compiled schema-consumer");
        invoke_ok({binaries[0].string()}, tmp.path, env);

        std::cout << "xmake XML rule default lookup, multi-input merge, incrementality, "
                     "fingerprint, namespace, and header changes passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "xmake_schema-test failed: " << e.what() << '\n';
        return 1;
    }
}
