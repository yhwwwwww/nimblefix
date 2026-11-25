#include "test_process_utils.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace tu = nimblefix::test_util;

namespace {

int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #cond << '\n'; ++g_failures; } \
} while (false)

std::map<std::string, std::string> expect_frame(tu::RawTcpSocket& peer, std::string_view msg_type, std::uint64_t seq) {
    auto msg = peer.read_fix_frame(8'000);
    if (!msg) throw std::runtime_error("timed out waiting for FIX frame");
    CHECK((*msg)["35"] == msg_type);
    CHECK(std::stoull((*msg)["34"]) == seq);
    CHECK((*msg)["49"] == "BUY" && (*msg)["56"] == "SELL");
    return std::move(*msg);
}

std::size_t count_nfxj(const std::filesystem::path& dir) {
    std::size_t n = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".nfxj") ++n;
    }
    return n;
}

void run_inbound_half_frame_reconnect(const std::filesystem::path& binary) {
    tu::TempDir dir("nimblefix-raw-peer-");
    auto [listener, port] = tu::RawTcpSocket::listen_loopback();
    auto child = tu::ChildProcess::spawn({binary.string(), std::to_string(port), dir.path.string()});
    std::uint16_t p1 = 0, p2 = 0;
    {
        auto conn = listener.accept(8'000, &p1);
        (void)expect_frame(conn, "A", 1);
        conn.send_all(tu::make_fix_wire("FIX.4.4", 1, "A", "SELL", "BUY", {{98, "0"}, {108, "30"}}));
        CHECK(expect_frame(conn, "D", 2)["11"] == "1");
        const std::string incomplete = tu::make_fix_wire("FIX.4.4", 2, "8", "SELL", "BUY", {{11, "1"}});
        conn.send_all(std::string_view{incomplete}.substr(0, incomplete.size() / 2));
        // Close with half frame in flight
    }
    {
        auto conn = listener.accept(8'000, &p2);
        (void)expect_frame(conn, "A", 3);
        conn.send_all(tu::make_fix_wire("FIX.4.4", 3, "A", "SELL", "BUY", {{98, "0"}, {108, "30"}}));
        auto req = expect_frame(conn, "2", 4);
        CHECK(req["7"] == "2" && req["16"] == "2");
        const std::string orig = tu::utc_now_fix_ms();
        conn.send_all(tu::make_fix_wire("FIX.4.4", 2, "8", "SELL", "BUY", {{11, "1"}}, orig));
        CHECK(expect_frame(conn, "D", 5)["11"] == "2");
        conn.send_all(tu::make_fix_wire("FIX.4.4", 4, "8", "SELL", "BUY", {{11, "2"}}));
        (void)expect_frame(conn, "5", 6);
        conn.send_all(tu::make_fix_wire("FIX.4.4", 5, "5", "SELL", "BUY"));
    }
    const auto res = child.wait_exit(8'000);
    CHECK(res.ok());
    CHECK(tu::json_int_field(res.stdout_text, "active").value_or(0) == 2);
    CHECK(tu::json_int_field(res.stdout_text, "reports").value_or(0) == 2);
    CHECK(count_nfxj(dir.path) == 1);
    CHECK(p1 != p2);
}

void run_outbound_partial_loss_reconnect(const std::filesystem::path& binary) {
    tu::TempDir dir("nimblefix-outbound-reconnect-");
    auto [listener, port] = tu::RawTcpSocket::listen_loopback();
    auto child = tu::ChildProcess::spawn({binary.string(), std::to_string(port), dir.path.string()});
    {
        auto conn = listener.accept(8'000);
        (void)expect_frame(conn, "A", 1);
        conn.send_all(tu::make_fix_wire("FIX.4.4", 1, "A", "SELL", "BUY", {{98, "0"}, {108, "30"}}));
        std::string partial;
        while (std::count(partial.begin(), partial.end(), '\x01') < 2) {
            const std::string ch = conn.recv_some(1, 5'000);
            CHECK(!ch.empty());
            partial += ch;
        }
        const auto first = partial.find('\x01');
        const std::size_t body_len = std::stoull(partial.substr(first + 3, partial.size() - first - 4));
        const std::size_t total = partial.size() + body_len + 7;
        while (partial.size() < total / 2) {
            const std::string ch = conn.recv_some(total / 2 - partial.size(), 5'000);
            CHECK(!ch.empty());
            partial += ch;
        }
    }
    {
        auto conn = listener.accept(8'000);
        (void)expect_frame(conn, "A", 3);
        conn.send_all(
            tu::make_fix_wire("FIX.4.4", 2, "A", "SELL", "BUY", {{98, "0"}, {108, "30"}}) +
            tu::make_fix_wire("FIX.4.4", 3, "2", "SELL", "BUY", {{7, "2"}, {16, "2"}}));
        std::map<std::uint64_t, std::map<std::string, std::string>> orders;
        while (orders.size() != 2) {
            auto msg = conn.read_fix_frame(8'000);
            CHECK(msg.has_value() && (*msg)["35"] == "D");
            if (!msg) break;
            orders[std::stoull((*msg)["34"])] = std::move(*msg);
        }
        CHECK(orders.contains(2) && orders.contains(4));
        CHECK(orders[2]["11"] == "1" && orders[2]["43"] == "Y");
        CHECK(orders[2]["122"] <= orders[2]["52"]);
        CHECK(orders[4]["11"] == "2" && !orders[4].contains("43"));
        conn.send_all(
            tu::make_fix_wire("FIX.4.4", 4, "8", "SELL", "BUY", {{11, "1"}}) +
            tu::make_fix_wire("FIX.4.4", 5, "8", "SELL", "BUY", {{11, "2"}}));
        (void)expect_frame(conn, "5", 5);
        conn.send_all(tu::make_fix_wire("FIX.4.4", 6, "5", "SELL", "BUY"));
    }
    const auto res = child.wait_exit(8'000);
    CHECK(res.ok());
    CHECK(tu::json_int_field(res.stdout_text, "active").value_or(0) == 2);
    CHECK(tu::json_int_field(res.stdout_text, "reports").value_or(0) == 2);
    CHECK(count_nfxj(dir.path) == 1);
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path binary;
    std::string mode = "both";
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        if (a == "--binary" && i + 1 < argc) binary = std::filesystem::absolute(argv[++i]);
        else if (a == "--mode" && i + 1 < argc) mode = argv[++i];
    }
    if (binary.empty()) {
        std::cerr << "usage: initiator_reconnect-test --binary PATH [--mode inbound|outbound|both]\n";
        return 2;
    }
    try {
        if (mode == "inbound" || mode == "both") run_inbound_half_frame_reconnect(binary);
        if (mode == "outbound" || mode == "both") run_outbound_partial_loss_reconnect(binary);
    } catch (const std::exception& e) {
        std::cerr << "initiator_reconnect test error: " << e.what() << '\n';
        return 1;
    }
    if (g_failures != 0) return 1;
    std::cout << "independent TCP half-frame EOF, partial outbound loss, reconnect, high Logon and PossDup replay passed\n";
    return 0;
}
