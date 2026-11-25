#include "test_process_utils.hpp"

#include <csignal>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace tu = nimblefix::test_util;

namespace {

int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #cond << '\n'; ++g_failures; } \
} while (false)

tu::CommandResult exec_cli(const std::filesystem::path& bin, const std::filesystem::path& cwd,
                           const std::vector<std::string>& args, int expected = 0,
                           const std::map<std::string, std::string>& env = {}) {
    std::vector<std::string> argv{bin.string()};
    argv.insert(argv.end(), args.begin(), args.end());
    auto res = tu::run_command(argv, cwd, env, {}, false, 20'000);
    if (res.exit_code != expected) {
        std::cerr << "CLI failed: exit=" << res.exit_code << " expected=" << expected
                  << "\nstdout=" << res.stdout_text << "\nstderr=" << res.stderr_text << '\n';
        ++g_failures;
    }
    return res;
}

std::string replace_first(std::string s, std::string_view from, std::string_view to) {
    const auto pos = s.find(from);
    if (pos != std::string::npos) s.replace(pos, from.size(), to);
    return s;
}

std::string json_quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path binary;
    std::filesystem::path root_repo = std::filesystem::current_path();
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        if (a == "--binary" && i + 1 < argc) binary = std::filesystem::absolute(argv[++i]);
        else if (a == "--root" && i + 1 < argc) root_repo = std::filesystem::absolute(argv[++i]);
    }
    if (binary.empty()) {
        std::cerr << "usage: cli-test --binary PATH [--root DIR]\n";
        return 2;
    }

    try {
        tu::TempDir tmp("nimblefix-cli-");
        const auto root = tmp.path;
        (void)exec_cli(binary, root, {"--help"});
        CHECK(exec_cli(binary, root, {"--version"}).stdout_text.find("0.3.0") != std::string::npos);

        const auto doctor = exec_cli(binary, root, {"doctor", "--json"}).stdout_text;
        CHECK(tu::json_string_field(doctor, "confirmation") == "appended");
        CHECK(doctor.find("\"queued\"") != std::string::npos);
        const bool has_fix44 = tu::json_bool_field(doctor, "fix44").value_or(false);
        const bool has_xml = tu::json_bool_field(doctor, "xml").value_or(false);

        const auto config_dir = root / "配置 with spaces";
        const auto created = exec_cli(binary, root, {"init", "--directory", config_dir.string(), "--json"}).stdout_text;
        const auto acc_path_str = tu::json_string_field(created, "acceptor");
        CHECK(acc_path_str.has_value() && std::filesystem::is_regular_file(*acc_path_str));
        const std::string original = tu::read_file_bytes(config_dir / "acceptor.toml");
        (void)exec_cli(binary, root, {"init", "--directory", config_dir.string()}, 2);
        CHECK(tu::read_file_bytes(config_dir / "acceptor.toml") == original);
        CHECK(tu::json_bool_field(
            exec_cli(binary, root, {"validate", "--config", (config_dir / "acceptor.toml").string(), "--json"}).stdout_text,
            "valid").value_or(false));
        CHECK(!std::filesystem::exists(config_dir / "state"));

        // Failover + ShardedEngine TOML validation
        const auto failover_toml = config_dir / "failover_initiator.toml";
        tu::write_file_bytes(failover_toml,
            "version = 1\n"
            "[engine]\n"
            "dispatch = \"inline\"\n"
            "io_backend = \"epoll\"\n"
            "idle_strategy = \"hybrid_backoff\"\n"
            "idle_spin_iterations = 5000\n"
            "idle_yield_iterations = 500\n"
            "shards = 4\n"
            "cpu_cores = [0, 1, 2, 3]\n"
            "[engine.storage]\n"
            "directory = \"state-failover\"\n"
            "[[sessions]]\n"
            "name = \"venue\"\n"
            "role = \"initiator\"\n"
            "sender_comp_id = \"BUY\"\n"
            "target_comp_id = \"SELL\"\n"
            "endpoints = [\"127.0.0.1:19876\", \"127.0.0.1:19877\"]\n"
            "[sessions.connection_strategy]\n"
            "policy = \"ordered\"\n"
            "initial_backoff_ms = 100\n"
            "max_backoff_ms = 5000\n"
            "backoff_multiplier = 2\n"
            "max_retries = 10\n"
            "reset_to_primary_on_success = true\n"
            "immediate_failover = true\n");
        CHECK(tu::json_bool_field(
            exec_cli(binary, root, {"validate", "--config", failover_toml.string(), "--json"}).stdout_text,
            "valid").value_or(false));

        if (has_fix44) {
            const auto typed = config_dir / "typed.toml";
            const std::string typed_content = replace_first(original, "schema = \"none\"", "schema = \"fix44\"");
            tu::write_file_bytes(typed, typed_content);
            (void)exec_cli(binary, root, {"validate", "--config", typed.string(), "--json"});
            tu::write_file_bytes(typed, replace_first(typed_content, "transport = \"FIX.4.4\"", "transport = \"FIX.4.2\""));
            const auto mismatch = exec_cli(binary, root, {"validate", "--config", typed.string(), "--json"}, 2);
            CHECK(mismatch.stderr_text.find("dictionary version") != std::string::npos);
            if (has_xml) {
                const auto dicts = root_repo / "schemas" / "standard";
                tu::write_file_bytes(typed, replace_first(typed_content, "[engine.storage]",
                    "dictionary = " + json_quote((dicts / "FIX44.xml").string()) + "\n[engine.storage]"));
                (void)exec_cli(binary, root, {"validate", "--config", typed.string(), "--json"});
                tu::write_file_bytes(typed, replace_first(typed_content, "[engine.storage]",
                    "dictionary = " + json_quote((dicts / "FIX42.xml").string()) + "\n[engine.storage]"));
                const auto fp_err = exec_cli(binary, root, {"validate", "--config", typed.string(), "--json"}, 2);
                CHECK(fp_err.stderr_text.find("fingerprint") != std::string::npos);
            }
        }

        (void)exec_cli(binary, root, {"run", "--config", (config_dir / "acceptor.toml").string(), "--duration-ms", "1x"}, 2);
        (void)exec_cli(binary, root, {"unknown-command"}, 2);

        const auto bad = config_dir / "bad.toml";
        tu::write_file_bytes(bad,
            "version = 2\n[engine]\ndispatch = \"mistyped\"\nunknown_key = true\n"
            "[engine.storage]\ndirectory = \"state\"\nconfirmation = \"synced\"\n"
            "[[sessions]]\nname = \"bad\"\nrole = \"initiator\"\nsender_comp_id = \"BUY\"\n"
            "target_comp_id = \"SELL\"\nendpoint = \"127.0.0.1:65537\"\n"
            "[sessions.logon]\npassword = \"SHOULD-NEVER-APPEAR\"\n");
        const auto bad_res = exec_cli(binary, root, {"validate", "--config", bad.string(), "--json"}, 2);
        CHECK(bad_res.stderr_text.find("\"field\"") != std::string::npos);
        CHECK((bad_res.stdout_text + bad_res.stderr_text).find("SHOULD-NEVER-APPEAR") == std::string::npos);

        // inspect frame credential redaction
        const std::string secret = "CLI-SECRET-DO-NOT-JOURNAL";
        const auto raw_path = root / "logon.fix";
        tu::write_file_bytes(raw_path, tu::make_fix_wire("FIX.4.4", 1, "A", "BUY", "SELL",
            {{98, "0"}, {108, "30"}, {553, "demo"}, {554, secret}}));
        const auto insp = exec_cli(binary, root, {"inspect", "frame", "--path", raw_path.string(), "--json"});
        CHECK((insp.stdout_text + insp.stderr_text).find(secret) == std::string::npos);
        CHECK(insp.stdout_text.find("\"tag\":554,\"value\":\"<redacted>\"") != std::string::npos);

        // Live server run + auth + sub-ms timestamp + telemetry + replay
        const auto config = config_dir / "acceptor.toml";
        const auto telemetry_file = config_dir / "telemetry.nfxt";
        std::string content = replace_first(original, "127.0.0.1:9876", "127.0.0.1:0");
        content = replace_first(content, "[engine.storage]",
            "telemetry_path = " + json_quote(telemetry_file.string()) + "\ntelemetry_stage_probes = true\n[engine.storage]");
        content += "\ntimestamp_precision = \"us\"\nverify_checksum = true\n"
                   "[sessions.authentication]\nmethod = \"credentials\"\nusername = \"demo\"\n"
                   "password_env = \"NIMBLEFIX_CLI_TEST_PASSWORD\"\nrequire_tls = false\n";
        tu::write_file_bytes(config, content);
        const std::map<std::string, std::string> env{{"NIMBLEFIX_CLI_TEST_PASSWORD", secret}};
        (void)exec_cli(binary, root, {"validate", "--config", config.string(), "--json"}, 0, env);

        auto server = tu::ChildProcess::spawn({binary.string(), "run", "--config", config.string(), "--json"}, root, env);
        std::uint16_t port = 0;
        for (;;) {
            const auto line = server.read_stdout_line(10'000);
            CHECK(line.has_value());
            if (!line) break;
            if (tu::json_string_field(*line, "event") == "listening") {
                port = static_cast<std::uint16_t>(tu::json_int_field(*line, "port").value_or(0));
                break;
            }
        }
        CHECK(port > 0);

        // Bad credentials rejected before opening journal
        {
            auto rej = tu::RawTcpSocket::connect_loopback(port);
            rej.send_all(tu::make_fix_wire("FIX.4.4", 1, "A", "BUY", "SELL",
                {{98, "0"}, {108, "30"}, {553, "demo"}, {554, "wrong"}}));
            CHECK(rej.recv_some(1024, 3'000).empty());
        }
        bool any_nfxj_before = false;
        if (std::filesystem::exists(config_dir / "state")) {
            for (const auto& e : std::filesystem::recursive_directory_iterator(config_dir / "state")) {
                if (e.path().extension() == ".nfxj") any_nfxj_before = true;
            }
        }
        CHECK(!any_nfxj_before);

        // Valid credentials + business order + SIGTERM clean logout
        {
            auto peer = tu::RawTcpSocket::connect_loopback(port);
            peer.send_all(tu::read_file_bytes(raw_path));
            auto lreply = peer.read_fix_frame(5'000);
            CHECK(lreply.has_value() && (*lreply)["35"] == "A");
            CHECK((*lreply)["52"].size() == 24); // microsecond SendingTime

            peer.send_all(tu::make_fix_wire("FIX.4.4", 2, "D", "BUY", "SELL",
                {{11, "CLI-ORDER"}, {55, "AAPL"}, {54, "1"}, {38, "100"}, {40, "1"}}));
            bool saw_msg = false;
            for (int tries = 0; tries < 10; ++tries) {
                const auto line = server.read_stdout_line(5'000);
                if (!line) break;
                if (tu::json_string_field(*line, "event") == "message") {
                    CHECK(tu::json_int_field(*line, "business_messages").value_or(0) == 1);
                    saw_msg = true;
                    break;
                }
            }
            CHECK(saw_msg);

            server.send_signal(SIGTERM);
            auto lo = peer.read_fix_frame(5'000);
            CHECK(lo.has_value() && (*lo)["35"] == "5");
            peer.send_all(tu::make_fix_wire("FIX.4.4", 3, "5", "BUY", "SELL"));
        }

        const auto done = server.wait_exit(10'000);
        CHECK(done.exit_code == 128 + SIGTERM);
        CHECK((done.stdout_text + done.stderr_text).find(secret) == std::string::npos);
        CHECK(done.stdout_text.find("\"event\":\"stopped\"") != std::string::npos);

        // Check no secret leaked to disk + find session.nfxj
        std::vector<std::filesystem::path> journals;
        std::size_t state_files = 0;
        for (const auto& e : std::filesystem::recursive_directory_iterator(config_dir / "state")) {
            if (!e.is_regular_file()) continue;
            ++state_files;
            CHECK(tu::read_file_bytes(e.path()).find(secret) == std::string::npos);
            if (e.path().filename() == "session.nfxj") journals.push_back(e.path());
        }
        CHECK(state_files > 0 && journals.size() == 1);

        // Telemetry CLI --json & --prometheus
        CHECK(std::filesystem::is_regular_file(telemetry_file));
        const auto tel_json = exec_cli(binary, root, {"telemetry", "--path", telemetry_file.string(), "--json"}).stdout_text;
        CHECK(tu::json_int_field(tel_json, "pid").value_or(0) > 0);
        CHECK(tu::json_int_field(tel_json, "business_deliveries").value_or(0) == 1);
        CHECK(tel_json.find("\"parse\":{\"count\":") != std::string::npos);
        const auto tel_prom = exec_cli(binary, root, {"telemetry", "--path", telemetry_file.string(), "--prometheus"}).stdout_text;
        CHECK(tel_prom.find("nimblefix_session_business_deliveries_total") != std::string::npos);
        CHECK(tel_prom.find("stage=\"parse\",quantile=\"0.99\"") != std::string::npos);

        // Replay CLI offline & TCP
        const auto rep_off = exec_cli(binary, root, {
            "replay", "--path", journals[0].string(),
            "--direction", "inbound", "--skip-admin", "--json"}).stdout_text;
        CHECK(tu::json_string_field(rep_off, "mode") == "offline");
        CHECK(tu::json_int_field(rep_off, "replayed_frames").value_or(0) == 1);
        CHECK(tu::json_int_field(rep_off, "invalid_frames").value_or(1) == 0);

        auto [sink_srv, sink_port] = tu::RawTcpSocket::listen_loopback();
        auto rep_proc = tu::ChildProcess::spawn({
            binary.string(), "replay", "--path", journals[0].string(),
            "--direction", "inbound", "--skip-admin",
            "--endpoint", "127.0.0.1:" + std::to_string(sink_port), "--json"}, root);
        auto conn = sink_srv.accept(5'000);
        std::string replayed_wire;
        for (;;) {
            const std::string chunk = conn.recv_some(4096, 5'000);
            if (chunk.empty()) break;
            replayed_wire += chunk;
        }
        const auto rep_done = rep_proc.wait_exit(10'000);
        CHECK(rep_done.ok());
        CHECK(tu::json_string_field(rep_done.stdout_text, "mode") == "tcp");
        CHECK(tu::json_int_field(rep_done.stdout_text, "replayed_frames").value_or(0) == 1);
        CHECK(replayed_wire.find("11=CLI-ORDER\x01") != std::string::npos);
        CHECK(replayed_wire.find("35=D\x01") != std::string::npos);
    } catch (const std::exception& e) {
        std::cerr << "cli test error: " << e.what() << '\n';
        return 1;
    }

    if (g_failures != 0) return 1;
    std::cout << "CLI offline validation, diagnostics, authentication, shutdown, telemetry, replay and secret redaction passed\n";
    return 0;
}
