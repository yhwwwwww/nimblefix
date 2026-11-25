#include "../tests/test_process_utils.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace tu = nimblefix::test_util;

namespace {

std::uint64_t steady_now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void smoke_journal(const fs::path& binary) {
    tu::TempDir tmp("nimblefix-journal-smoke-");
    const auto r = tu::run_command(
        {binary.string(), tmp.path.string(), "100", "2", "8192", "32", "128"},
        {}, {}, {}, false, 60'000);
    if (!r.ok()) {
        throw std::runtime_error("journal-bench exited " + std::to_string(r.exit_code) + ": " + r.stderr_text);
    }
    std::istringstream iss(r.stdout_text);
    std::string header;
    if (!std::getline(iss, header) || header.find("operation,iteration,kind,elapsed_ns") != 0) {
        throw std::runtime_error("journal-bench unexpected CSV header: " + header);
    }
    std::map<std::string, std::size_t> counts;
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        const auto c1 = line.find(',');
        const auto c2 = (c1 != std::string::npos) ? line.find(',', c1 + 1) : std::string::npos;
        const auto c3 = (c2 != std::string::npos) ? line.find(',', c2 + 1) : std::string::npos;
        if (c3 == std::string::npos) continue;
        const std::string key = line.substr(0, c1) + "/" + line.substr(c2 + 1, c3 - c2 - 1);
        ++counts[key];
    }
    for (const char* kind : {"inbound", "outbound", "delivery"}) {
        if (counts[std::string{"append/"} + kind] != 100 ||
            counts[std::string{"reader_next/"} + kind] != 100) {
            throw std::runtime_error(std::string{"journal-bench row count mismatch for "} + kind);
        }
    }
    if (counts["reopen/normal_checkpoint_index_recovery"] != 2 ||
        counts["fresh_open/new_store"] != 1 ||
        counts["checkpoint/checkpoint_and_outbound_index"] != 1) {
        throw std::runtime_error("journal-bench lifecycle row mismatch");
    }
    if (tu::json_int_field(r.stderr_text, "format_version") != 2 ||
        tu::json_int_field(r.stderr_text, "records_committed") != 300 ||
        tu::json_bool_field(r.stderr_text, "checkpoint_index_recovered") != true ||
        tu::json_int_field(r.stderr_text, "per_message_syncs_requested") != 0) {
        throw std::runtime_error("journal-bench JSON summary check failed: " + r.stderr_text);
    }
    std::cout << "journal-bench smoke passed (300 records committed, 2 reopens)\n";
}

void run_runtime_case(const fs::path& binary, std::string_view mode, std::string_view send_source) {
    tu::TempDir tmp("nimblefix-runtime-smoke-");
    constexpr std::size_t samples = 50;
    constexpr std::size_t warmup = 10;
    auto child = tu::ChildProcess::spawn({
        binary.string(), std::string{mode}, std::string{send_source},
        std::to_string(samples), std::to_string(warmup),
        "2", "32", "5000", "timed", tmp.path.string(),
        "65536", "8", "512",
    });

    const auto ready = child.read_stdout_line(15'000);
    if (!ready || !ready->starts_with("READY ")) {
        throw std::runtime_error("runtime-bench did not emit READY; stderr=" + child.stderr_text());
    }
    const auto port = static_cast<std::uint16_t>(std::stoul(ready->substr(6)));
    auto peer = tu::RawTcpSocket::connect_loopback(port, 10'000);

    std::uint64_t in_seq = 1;
    std::uint64_t srv_seq = 1;
    peer.send_all(tu::make_fix_wire("FIX.4.4", in_seq++, "A", "BUY", "SELL",
                                    {{98, "0"}, {108, "30"}, {369, "0"}}));
    for (;;) {
        const auto frame = peer.read_fix_frame(10'000);
        if (!frame) throw std::runtime_error("timeout waiting for Logon ack");
        if (std::stoull(frame->at("34")) != srv_seq++) throw std::runtime_error("bad server seq on Logon");
        if (frame->at("35") == "A") break;
    }

    auto run_phase = [&](std::size_t count, std::size_t offset) {
        child.write_line("RUN " + std::to_string(count) + " " + std::to_string(offset));
        if (send_source == "callback") {
            const auto rr = child.read_stdout_line(10'000);
            if (!rr || !rr->starts_with("RUN_READY")) {
                throw std::runtime_error("expected RUN_READY, got: " + (rr ? *rr : "<eof>"));
            }
            const std::string payload(32, 'X');
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint64_t now = steady_now_ns();
                const std::size_t id = offset + i;
                peer.send_all(tu::make_fix_wire("FIX.4.4", in_seq++, "D", "BUY", "SELL", {
                    {11, std::to_string(id)},
                    {55, "AAPL"},
                    {58, payload},
                    {900001, std::to_string(now)},
                    {900003, std::to_string(now)},
                    {369, std::to_string(srv_seq - 1)},
                }));
                for (;;) {
                    const auto f = peer.read_fix_frame(10'000);
                    if (!f) throw std::runtime_error("timeout waiting for FIX 8");
                    if (std::stoull(f->at("34")) != srv_seq++) throw std::runtime_error("bad server seq");
                    const std::string& mt = f->at("35");
                    if (mt == "0") continue;
                    if (mt == "1") {
                        peer.send_all(tu::make_fix_wire("FIX.4.4", in_seq++, "0", "BUY", "SELL",
                                                        {{112, f->at("112")}, {369, std::to_string(srv_seq - 1)}}));
                        continue;
                    }
                    if (mt != "8") throw std::runtime_error("unexpected msg type " + mt);
                    break;
                }
            }
        } else {
            std::size_t received = 0;
            while (received < count) {
                const auto f = peer.read_fix_frame(15'000);
                if (!f) throw std::runtime_error("timeout waiting for FIX D from server");
                if (std::stoull(f->at("34")) != srv_seq++) throw std::runtime_error("bad server seq");
                const std::string& mt = f->at("35");
                if (mt == "0") continue;
                if (mt == "1") {
                    peer.send_all(tu::make_fix_wire("FIX.4.4", in_seq++, "0", "BUY", "SELL",
                                                    {{112, f->at("112")}, {369, std::to_string(srv_seq - 1)}}));
                    continue;
                }
                if (mt != "D") throw std::runtime_error("unexpected server msg type " + mt);
                const std::uint64_t resp_ns = steady_now_ns();
                peer.send_all(tu::make_fix_wire("FIX.4.4", in_seq++, "8", "BUY", "SELL", {
                    {11, f->at("11")},
                    {17, "EXEC-1"},
                    {58, f->count("58") ? f->at("58") : ""},
                    {900001, f->at("900001")},
                    {900002, std::to_string(resp_ns)},
                    {369, std::to_string(srv_seq - 1)},
                }));
                ++received;
            }
        }
        const auto done = child.read_stdout_line(15'000);
        if (!done || !done->starts_with("RUN_DONE ")) {
            throw std::runtime_error("expected RUN_DONE, got: " + (done ? *done : "<eof>"));
        }
    };

    run_phase(warmup, 0);
    run_phase(samples, warmup);

    peer.send_all(tu::make_fix_wire("FIX.4.4", in_seq++, "5", "BUY", "SELL",
                                    {{369, std::to_string(srv_seq - 1)}}));
    for (;;) {
        const auto f = peer.read_fix_frame(10'000);
        if (!f) throw std::runtime_error("timeout waiting for Logout ack");
        if (std::stoull(f->at("34")) != srv_seq++) throw std::runtime_error("bad server seq on Logout");
        if (f->at("35") == "5") break;
    }
    child.write_line("STOP");
    peer.close();
    child.close_stdin();
    const auto res = child.wait_exit(15'000);
    if (!res.ok() || res.stderr_text.find("\"measured_elapsed_ns\"") == std::string::npos) {
        throw std::runtime_error("runtime-bench failed (" + std::string{mode} + "/" +
                                 std::string{send_source} + "): " + res.stderr_text);
    }
}

void smoke_runtime(const fs::path& binary) {
    for (const char* mode : {"inline", "queued-journal", "queued-ring"}) {
        for (const char* src : {"callback", "mpsc", "lane"}) {
            run_runtime_case(binary, mode, src);
        }
    }
    std::cout << "runtime-bench smoke passed (inline/queued-journal/queued-ring x callback/mpsc/lane)\n";
}

void smoke_schema(const fs::path& binary) {
    const auto r = tu::run_command({binary.string(), "100", "16"}, {}, {}, {}, false, 60'000);
    if (!r.ok()) {
        throw std::runtime_error("schema-bench exited " + std::to_string(r.exit_code) + ": " + r.stderr_text);
    }
    if (r.stdout_text.find("case,iteration,elapsed_ns") != 0 ||
        r.stderr_text.find("\"case\":") == std::string::npos ||
        r.stderr_text.find("\"schema\":") == std::string::npos) {
        throw std::runtime_error("schema-bench output validation failed");
    }
    std::cout << "schema-bench smoke passed\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        fs::path journal_bench;
        fs::path runtime_bench;
        fs::path schema_bench;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--journal-bench" && i + 1 < argc) journal_bench = argv[++i];
            else if (arg == "--runtime-bench" && i + 1 < argc) runtime_bench = argv[++i];
            else if (arg == "--schema-bench" && i + 1 < argc) schema_bench = argv[++i];
            else throw std::runtime_error("unknown argument: " + std::string{arg});
        }
        if (journal_bench.empty() && runtime_bench.empty() && schema_bench.empty()) {
            throw std::runtime_error("provide at least one of --journal-bench, --runtime-bench, --schema-bench");
        }
        if (!journal_bench.empty()) smoke_journal(journal_bench);
        if (!runtime_bench.empty()) smoke_runtime(runtime_bench);
        if (!schema_bench.empty()) smoke_schema(schema_bench);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "bench-smoke-test failed: " << e.what() << '\n';
        return 1;
    }
}
