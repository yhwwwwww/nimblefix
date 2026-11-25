#include "test_process_utils.hpp"

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>

namespace tu = nimblefix::test_util;

namespace {

int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #cond << '\n'; ++g_failures; } \
} while (false)

void run_strategy(const std::filesystem::path& binary, const std::filesystem::path& root, std::string_view strategy) {
    std::int64_t saved_input_id = 0;
    {
        auto child = tu::ChildProcess::spawn({binary.string(), root.string(), "first", std::string{strategy}});
        const auto ready_line = child.read_stdout_line(10'000);
        CHECK(ready_line.has_value() && tu::json_string_field(*ready_line, "event") == "listening");
        const auto port = static_cast<std::uint16_t>(tu::json_int_field(*ready_line, "port").value_or(0));
        auto peer = tu::RawTcpSocket::connect_loopback(port);
        peer.send_all(tu::make_fix_wire("FIX.4.4", 1, "A", "BUY", "SELL", {{98, "0"}, {108, "30"}}));
        auto rep = peer.read_fix_frame(5'000);
        CHECK(rep.has_value() && (*rep)["35"] == "A");

        peer.send_all(tu::make_fix_wire("FIX.4.4", 2, "D", "BUY", "SELL", {{11, "RECOVER-ME"}, {55, "AAPL"}}));
        const auto cb_line = child.read_stdout_line(10'000);
        CHECK(cb_line.has_value() && tu::json_string_field(*cb_line, "event") == "callback");
        CHECK(tu::json_bool_field(*cb_line, "recovery").value_or(true) == false);
        saved_input_id = tu::json_int_field(*cb_line, "input_id").value_or(0);

        auto resp = peer.read_fix_frame(5'000);
        CHECK(resp.has_value() && (*resp)["35"] == "8" && (*resp)["34"] == "2" && (*resp)["11"] == "RECOVER-ME");

        child.send_signal(SIGKILL);
        const auto killed = child.wait_exit(5'000);
        CHECK(killed.term_signal == SIGKILL);
    }

    {
        auto child = tu::ChildProcess::spawn({binary.string(), root.string(), "recover", std::string{strategy}});
        const auto ready_line = child.read_stdout_line(10'000);
        CHECK(ready_line.has_value() && tu::json_string_field(*ready_line, "event") == "listening");
        const auto port = static_cast<std::uint16_t>(tu::json_int_field(*ready_line, "port").value_or(0));
        auto peer = tu::RawTcpSocket::connect_loopback(port);
        peer.send_all(tu::make_fix_wire("FIX.4.4", 3, "A", "BUY", "SELL", {{98, "0"}, {108, "30"}, {369, "2"}}));
        auto rep = peer.read_fix_frame(5'000);
        CHECK(rep.has_value() && (*rep)["35"] == "A");

        const auto rec_line = child.read_stdout_line(10'000);
        CHECK(rec_line.has_value() && tu::json_string_field(*rec_line, "event") == "callback");
        CHECK(tu::json_bool_field(*rec_line, "recovery").value_or(false) == true);
        CHECK(tu::json_int_field(*rec_line, "input_id").value_or(-1) == saved_input_id);
        CHECK(tu::json_int_field(*rec_line, "sequence").value_or(0) == 2);

        peer.send_all(tu::make_fix_wire("FIX.4.4", 4, "2", "BUY", "SELL", {{7, "2"}, {16, "2"}}));
        auto replay = peer.read_fix_frame(5'000);
        CHECK(replay.has_value() && (*replay)["35"] == "8" && (*replay)["34"] == "2" && (*replay)["43"] == "Y");
        CHECK((*replay)["11"] == "RECOVER-ME" && (*replay)["17"] == "DURABLE-REPLY" && replay->contains("122"));

        child.send_signal(SIGTERM);
        auto lo = peer.read_fix_frame(5'000);
        CHECK(lo.has_value() && (*lo)["35"] == "5");
        peer.send_all(tu::make_fix_wire("FIX.4.4", 5, "5", "BUY", "SELL"));

        const auto done = child.wait_exit(10'000);
        CHECK(done.ok());
        bool found_verified = false;
        std::size_t p = 0;
        while (p < done.stdout_text.size()) {
            const auto nl = done.stdout_text.find('\n', p);
            const std::string line = done.stdout_text.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
            if (tu::json_string_field(line, "event") == "verified") {
                found_verified = true;
                CHECK(tu::json_bool_field(line, "delivered").value_or(false) == true);
                CHECK(tu::json_int_field(line, "replies").value_or(0) == 1);
            }
            if (nl == std::string::npos) break;
            p = nl + 1;
        }
        CHECK(found_verified);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path binary;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--binary" && i + 1 < argc) {
            binary = std::filesystem::absolute(argv[++i]);
        }
    }
    if (binary.empty()) {
        std::cerr << "usage: queued_crash-test --binary PATH\n";
        return 2;
    }
    try {
        tu::TempDir tmp("nimblefix-queued-crash-");
        for (const char* strat : {"journal", "ring"}) {
            const auto dir = tmp.path / strat;
            std::filesystem::create_directories(dir);
            run_strategy(binary, dir, strat);
        }
    } catch (const std::exception& e) {
        std::cerr << "queued_crash test error: " << e.what() << '\n';
        return 1;
    }
    if (g_failures != 0) return 1;
    std::cout << "Queued journal/ring SIGKILL recovery preserved input id, one durable reply and completion ordering\n";
    return 0;
}
