#include "test_process_utils.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>

namespace fs = std::filesystem;
namespace tu = nimblefix::test_util;

namespace {

std::string run_ok(const std::vector<std::string>& cmd,
                   const fs::path& cwd = {},
                   int timeout_ms = 600'000) {
    const auto r = tu::run_command(cmd, cwd, {}, {}, true, timeout_ms);
    if (!r.ok()) {
        std::ostringstream oss;
        for (const auto& a : cmd) oss << a << ' ';
        throw std::runtime_error("command failed (exit=" + std::to_string(r.exit_code) +
                                 ", sig=" + std::to_string(r.term_signal) + "): " +
                                 oss.str() + "\n" + r.stdout_text);
    }
    return r.stdout_text;
}

struct ReadOnlyGuard {
    std::vector<fs::path> roots;
    ~ReadOnlyGuard() {
        std::error_code ec;
        for (const auto& r : roots) {
            if (!fs::exists(r, ec)) continue;
            ::chmod(r.c_str(), 0755);
            for (auto it = fs::recursive_directory_iterator(r, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); ++it) {
                if (it->is_directory(ec)) ::chmod(it->path().c_str(), 0755);
            }
        }
    }
};

void bump_mtime(const fs::path& p, long delta_sec = 2) {
    timespec now{};
    ::clock_gettime(CLOCK_REALTIME, &now);
    timespec ts[2]{};
    ts[0].tv_sec = now.tv_sec + delta_sec;
    ts[0].tv_nsec = now.tv_nsec;
    ts[1] = ts[0];
    if (::utimensat(AT_FDCWD, p.c_str(), ts, 0) != 0) {
        throw std::runtime_error("utimensat failed");
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        fs::path root = fs::current_path();
        std::string cxx = std::getenv("CXX") ? std::getenv("CXX") : "g++";
        bool core_only = false;
        bool features = false;
        bool examples = false;
        fs::path endpoint_test;
        fs::path xmake_schema_test;

        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--root" && i + 1 < argc) root = argv[++i];
            else if (arg == "--cxx" && i + 1 < argc) cxx = argv[++i];
            else if (arg == "--core-only") core_only = true;
            else if (arg == "--features") features = true;
            else if (arg == "--examples") examples = true;
            else if (arg == "--endpoint-test" && i + 1 < argc) endpoint_test = argv[++i];
            else if (arg == "--xmake-schema-test" && i + 1 < argc) xmake_schema_test = argv[++i];
            else throw std::runtime_error("unknown argument: " + std::string{arg});
        }
        if (core_only && features) {
            throw std::runtime_error("--core-only and --features are mutually exclusive");
        }
        root = fs::weakly_canonical(root);
        if (!endpoint_test.empty()) endpoint_test = fs::weakly_canonical(endpoint_test);
        if (!xmake_schema_test.empty()) xmake_schema_test = fs::weakly_canonical(xmake_schema_test);

        tu::TempDir tmp("nimblefix-install-");
        const fs::path base = tmp.path;
        const fs::path source = base / "source";
        const fs::path build = base / "build";
        const fs::path prefix = base / "prefix";
        const fs::path hidden_source = base / "hidden-source";
        fs::create_directories(source);

        ReadOnlyGuard ro_guard{{source, hidden_source}};

        for (const char* dir : {"include", "src", "cmake", "tools/schema", "tools/nimblefix",
                                "schemas", "examples", "xmake"}) {
            fs::create_directories((source / dir).parent_path());
            fs::copy(root / dir, source / dir, fs::copy_options::recursive);
        }
        fs::create_directories(source / "third_party/tomlplusplus");
        for (const char* file : {
                "CMakeLists.txt",
                "LICENSE",
                "third_party/README.md",
                "third_party/quickfix-LICENSE",
                "third_party/tomlplusplus/LICENSE",
                "third_party/tomlplusplus/toml.hpp",
            }) {
            fs::copy_file(root / file, source / file);
        }
        if (examples) {
            fs::create_directories(source / "tests");
            fs::copy_file(root / "tests/hello_fix.cpp", source / "tests/hello_fix.cpp");
        }

        // Make copied source tree strictly read-only.
        std::vector<fs::path> ro_dirs;
        for (const auto& entry : fs::recursive_directory_iterator(source)) {
            if (entry.is_directory()) ro_dirs.push_back(entry.path());
            else ::chmod(entry.path().c_str(), 0444);
        }
        for (auto it = ro_dirs.rbegin(); it != ro_dirs.rend(); ++it) {
            ::chmod(it->c_str(), 0555);
        }
        ::chmod(source.c_str(), 0555);

        const bool sample_schema = !core_only && (!features || examples);
        std::vector<std::string> configure = {
            "cmake", "-S", source.string(), "-B", build.string(),
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_CXX_COMPILER=" + cxx,
            "-DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE",
            "-DNIMBLEFIX_BUILD_TESTS=OFF",
            std::string{"-DNIMBLEFIX_BUILD_EXAMPLES="} + (examples ? "ON" : "OFF"),
            "-DNIMBLEFIX_BUILD_CLI=ON",
            std::string{"-DNIMBLEFIX_BUILD_SAMPLE_SCHEMA="} + (sample_schema ? "ON" : "OFF"),
            std::string{"-DNIMBLEFIX_BUILD_STANDARD_SCHEMAS="} + (features ? "ON" : "OFF"),
            std::string{"-DNIMBLEFIX_ENABLE_TLS="} + (features ? "ON" : "OFF"),
            std::string{"-DNIMBLEFIX_ENABLE_XML="} + (features ? "ON" : "OFF"),
        };
        if (core_only) {
            configure.push_back("-DCMAKE_DISABLE_FIND_PACKAGE_LibXml2=TRUE");
            configure.push_back("-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE");
        }
        run_ok(configure);
        run_ok({"cmake", "--build", build.string(), "--parallel", features ? "2" : "4"});
        run_ok({"cmake", "--install", build.string(), "--prefix", prefix.string()});

        // Relocate installed prefix and hide both source and build directories.
        const fs::path relocated = base / "relocated";
        fs::rename(prefix, relocated);
        fs::rename(source, hidden_source);
        fs::rename(build, base / "hidden-build");

        const fs::path consumer = base / "outside-consumer";
        const fs::path external_build = base / "outside-build";
        fs::copy(root / "tests/consumer", consumer, fs::copy_options::recursive);
        run_ok({
            "cmake", "-S", consumer.string(), "-B", external_build.string(),
            "-DCMAKE_PREFIX_PATH=" + relocated.string(),
            "-DCMAKE_CXX_COMPILER=" + cxx,
            "-DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE",
            std::string{"-DCONSUMER_SCHEMA="} + (core_only ? "OFF" : "ON"),
            std::string{"-DCONSUMER_FEATURES="} + (features ? "ON" : "OFF"),
        });
        run_ok({"cmake", "--build", external_build.string(), "--parallel", features ? "2" : "4"});
        run_ok({(external_build / "consumer").string()}, base);

        const fs::path cli = relocated / "bin" / "nimblefix";
        const std::string doctor = run_ok({cli.string(), "doctor", "--json"}, base);
        if (tu::json_bool_field(doctor, "tls") != features) {
            throw std::runtime_error("installed CLI tls mismatch: " + doctor);
        }

        const fs::path cli_config_dir = base / "installed cli config";
        const std::string initialized = run_ok({cli.string(), "init", "--directory", cli_config_dir.string(), "--json"}, base);
        const auto acceptor_cfg = tu::json_string_field(initialized, "acceptor");
        if (!acceptor_cfg) throw std::runtime_error("init missing acceptor path: " + initialized);
        const std::string validated = run_ok({cli.string(), "validate", "--config", *acceptor_cfg, "--json"}, base);
        if (tu::json_bool_field(validated, "valid") != true) {
            throw std::runtime_error("installed CLI failed to validate generated acceptor config: " + validated);
        }

        if (examples) {
            run_ok({(relocated / "bin/hello-fix").string(), "--self-test", "--restart"}, base);
            if (!core_only) {
                run_ok({(relocated / "bin/hello-fix").string(), "--self-test", "--typed", "--restart"}, base);
            }
            if (!endpoint_test.empty()) {
                run_ok({endpoint_test.string(),
                        "--acceptor", (relocated / "bin/acceptor-example").string(),
                        "--initiator", (relocated / "bin/initiator-example").string()}, base);
            }
        }

        if (!core_only) {
            run_ok({(external_build / "schema-consumer").string()}, base);
            const fs::path generated = external_build / "generated/venue_schema/venue.hpp";
            const std::string original = tu::read_file_bytes(generated);
            const auto orig_mtime = fs::last_write_time(generated);
            run_ok({"cmake", "--build", external_build.string(), "--parallel", features ? "2" : "4"});
            if (fs::last_write_time(generated) != orig_mtime) {
                throw std::runtime_error("no-op installed helper build rewrote generated header");
            }

            const fs::path xml = consumer / "venue.xml";
            std::string xml_text = tu::read_file_bytes(xml);
            const std::string target = "type=\"STRING\"/>";
            const auto pos = xml_text.find(target);
            if (pos == std::string::npos) throw std::runtime_error("venue.xml missing expected marker");
            xml_text.replace(pos, target.size(),
                "type=\"STRING\"><value enum=\"external\" description=\"Changed\"/></field>");
            tu::write_file_bytes(xml, xml_text);
            bump_mtime(xml, 2);
            run_ok({"cmake", "--build", external_build.string(), "-j", "2"});
            if (tu::read_file_bytes(generated) == original) {
                throw std::runtime_error("XML edit did not regenerate installed helper header");
            }
            run_ok({(external_build / "schema-consumer").string()}, base);

            if (features) {
                run_ok({(external_build / "features").string()}, base);
                run_ok({(external_build / "xml-runtime").string(), xml.string()}, base);
                const std::string summary = run_ok({cli.string(), "schema", "check", "--xml", xml.string(), "--json"}, base);
                if (tu::json_bool_field(summary, "valid") != true) {
                    throw std::runtime_error("installed CLI did not load runtime XML schema: " + summary);
                }
                const fs::path cli_header = base / "cli generated" / "venue.hpp";
                fs::create_directories(cli_header.parent_path());
                run_ok({cli.string(), "schema", "generate", "--xml", xml.string(),
                        "--namespace", "installed::venue", "--output", cli_header.string()}, base);
                if (!fs::is_regular_file(cli_header) ||
                    tu::read_file_bytes(cli_header).find("installed::venue") == std::string::npos) {
                    throw std::runtime_error("installed CLI schema generate did not produce requested header");
                }
                if (!xmake_schema_test.empty()) {
                    run_ok({xmake_schema_test.string(),
                            "--rule", (relocated / "share/nimblefix/xmake/nimblefix-schema.lua").string(),
                            "--generator", (relocated / "bin/nimblefix-schema-gen").string(),
                            "--include-dir", (relocated / "include").string()}, base);
                }
            }
        }

        // Unknown requested components must fail during find_package.
        const fs::path cmakelists = consumer / "CMakeLists.txt";
        std::string cm = tu::read_file_bytes(cmakelists);
        const std::string comp_pat = "COMPONENTS core";
        std::size_t replaced = 0;
        for (std::size_t cpos = 0; (cpos = cm.find(comp_pat, cpos)) != std::string::npos; ) {
            cm.replace(cpos, comp_pat.size(), "COMPONENTS nonexistent");
            cpos += std::string_view{"COMPONENTS nonexistent"}.size();
            ++replaced;
        }
        if (replaced == 0) throw std::runtime_error("consumer CMakeLists missing COMPONENTS core");
        tu::write_file_bytes(cmakelists, cm);
        const auto failed = tu::run_command({
            "cmake", "-S", consumer.string(), "-B", (base / "unknown-component").string(),
            "-DCMAKE_PREFIX_PATH=" + relocated.string(),
            "-DCMAKE_CXX_COMPILER=" + cxx,
            std::string{"-DCONSUMER_FEATURES="} + (features ? "ON" : "OFF"),
        }, base, {}, {}, true, 60'000);
        if (failed.ok() || failed.stdout_text.find("NimbleFIX") == std::string::npos) {
            throw std::runtime_error("unknown required component was not rejected clearly:\n" + failed.stdout_text);
        }

        const char* mode = core_only ? "core only"
                                     : (features ? "TLS/XML/standard features" : "sample XML");
        std::cout << "read-only build, relocated install, external consumer, and installed CLI passed ("
                  << mode << ", zero Python)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "install-test failed: " << e.what() << '\n';
        return 1;
    }
}
