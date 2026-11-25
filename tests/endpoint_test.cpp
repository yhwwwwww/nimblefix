#include "test_process_utils.hpp"

#include <csignal>
#include <filesystem>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

namespace tu = nimblefix::test_util;

namespace {

int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #cond << '\n'; ++g_failures; } \
} while (false)

std::string wait_matching(tu::ChildProcess& proc, const std::regex& re, std::string_view desc, int timeout_ms = 15'000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) throw std::runtime_error("timed out waiting for " + std::string{desc});
        const int rem = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        const auto line = proc.read_stdout_line(rem);
        if (!line) throw std::runtime_error("process exited before " + std::string{desc});
        if (std::regex_search(*line, re)) return *line;
    }
}

void validate_cli(const std::filesystem::path& acceptor, const std::filesystem::path& initiator,
                  const std::filesystem::path& dir) {
    for (const auto& bin : {acceptor, initiator}) {
        CHECK(tu::run_command({bin.string(), "--help"}, dir, {}, {}, false, 5'000).ok());
        const std::vector<std::vector<std::string>> bad_args = {
            {"--port", "1x"}, {"--port", "65536"}, {"--port", "-1"},
            {"--timeout-ms", "0"}, {"--host", "localhost"}, {"--unknown", "x"},
        };
        for (const auto& extra : bad_args) {
            std::vector<std::string> cmd{bin.string(), "--state", (dir / "invalid").string(), "--port", "12345"};
            cmd.insert(cmd.end(), extra.begin(), extra.end());
            CHECK(tu::run_command(cmd, dir, {}, {}, false, 5'000).exit_code == 2);
        }
    }
    CHECK(tu::run_command({initiator.string(), "--state", (dir / "old-cli").string(),
                           "--port", "12345", "--cl-ord-id", "NO-LONGER-SUPPORTED"},
                          dir, {}, {}, false, 5'000).exit_code == 2);

    // Bound non-listening socket guarantees refusal within timeout
    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    CHECK(::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const auto res = tu::run_command({initiator.string(), "--state", (dir / "unavail").string(),
                                      "--port", std::to_string(::ntohs(addr.sin_port)), "--timeout-ms", "250"},
                                     dir, {}, "", false, 8'000);
    ::close(sock);
    CHECK(res.exit_code == 1);
}

std::uint64_t wait_order_flow(tu::ChildProcess& client, const std::string& id,
                              const std::string& sym, const std::string& side, bool send_status) {
    const std::regex re_acc("ACCEPTED\\s+ClOrdID=" + id + "\\s+receipt=[0-9]+");
    const std::regex re_er("EXECUTION_REPORT\\s+ClOrdID=" + id + "\\s+Symbol=" + sym + "\\s+Side=" + side);
    const std::regex re_rcpt("RECEIPT\\s+ClOrdID=" + id + "\\s+state=written\\s+sequence=([0-9]+)");
    bool got_acc = false, got_er = false, got_rcpt = false;
    std::uint64_t seq = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!(got_acc && got_er && got_rcpt)) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) throw std::runtime_error("timed out waiting for order events of " + id);
        const int rem = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        const auto line = client.read_stdout_line(rem);
        if (!line) throw std::runtime_error("initiator exited early for " + id);
        if (std::regex_search(*line, re_acc)) got_acc = true;
        if (std::regex_search(*line, re_er)) {
            got_er = true;
            if (send_status && !got_rcpt) client.write_line("status");
        }
        std::smatch m;
        if (std::regex_search(*line, m, re_rcpt)) {
            got_rcpt = true;
            seq = std::stoull(m[1].str());
        }
    }
    return seq;
}

void exercise_paired_processes(const std::filesystem::path& acceptor,
                               const std::filesystem::path& initiator,
                               const std::filesystem::path& dir) {
    validate_cli(acceptor, initiator, dir);
    auto server = tu::ChildProcess::spawn({acceptor.string(), "--state", (dir / "sell-state").string(),
                                           "--port", "0", "--timeout-ms", "1000"}, dir);
    const std::string lline = wait_matching(server, std::regex(R"(LISTENING 127\.0\.0\.1:([0-9]+))"), "listening");
    std::smatch lm;
    CHECK(std::regex_search(lline, lm, std::regex(R"(LISTENING 127\.0\.0\.1:([0-9]+))")));
    const std::uint16_t port = static_cast<std::uint16_t>(std::stoi(lm[1].str()));

    // Bind conflict fails fast
    CHECK(tu::run_command({acceptor.string(), "--state", (dir / "conflict").string(),
                           "--port", std::to_string(port), "--timeout-ms", "200"},
                          dir, {}, {}, false, 5'000).exit_code == 1);

    const auto client_state = dir / "buy-state";
    std::uint64_t seq1 = 0, seq2 = 0, seq3 = 0;
    {
        auto client = tu::ChildProcess::spawn({initiator.string(), "--state", client_state.string(),
                                               "--host", "127.0.0.1", "--port", std::to_string(port),
                                               "--timeout-ms", "8000"}, dir);
        (void)wait_matching(client, std::regex("SESSION_ACTIVE"), "session active");
        client.write_line("order MULTI-1 AAPL buy 100 150.25");
        seq1 = wait_order_flow(client, "MULTI-1", "AAPL", "buy", true);
        client.write_line("order MULTI-2 MSFT sell 2 market");
        seq2 = wait_order_flow(client, "MULTI-2", "MSFT", "sell", true);
        CHECK(seq2 > seq1);
        client.write_line("quit");
        CHECK(client.wait_exit(10'000).ok());
    }
    // Restart with same state + immediate EOF after order
    {
        auto restarted = tu::ChildProcess::spawn({initiator.string(), "--state", client_state.string(),
                                                  "--host", "127.0.0.1", "--port", std::to_string(port),
                                                  "--timeout-ms", "8000"}, dir);
        (void)wait_matching(restarted, std::regex("SESSION_ACTIVE"), "restarted active");
        restarted.write_line("order PERSIST-1 NVDA buy 0.5 900.00");
        restarted.close_stdin();
        seq3 = wait_order_flow(restarted, "PERSIST-1", "NVDA", "buy", false);
        CHECK(seq3 > seq2);
        CHECK(restarted.wait_exit(10'000).ok());
        std::size_t nfxj = 0;
        for (const auto& e : std::filesystem::recursive_directory_iterator(client_state)) {
            if (e.is_regular_file() && e.path().extension() == ".nfxj") ++nfxj;
        }
        CHECK(nfxj == 1);
    }
    // Idle SIGTERM
    {
        auto idle = tu::ChildProcess::spawn({initiator.string(), "--state", client_state.string(),
                                             "--host", "127.0.0.1", "--port", std::to_string(port),
                                             "--timeout-ms", "8000"}, dir);
        (void)wait_matching(idle, std::regex("SESSION_ACTIVE"), "idle active");
        idle.send_signal(SIGTERM);
        const auto done = idle.wait_exit(10'000);
        CHECK(done.exit_code == 0 || done.exit_code == 128 + SIGTERM || done.term_signal == SIGTERM);
    }

    server.send_signal(SIGTERM);
    CHECK(server.wait_exit(10'000).ok());
}

void exercise_raw_peer(const std::filesystem::path& initiator, const std::filesystem::path& dir) {
    auto [listener, port] = tu::RawTcpSocket::listen_loopback();
    auto client = tu::ChildProcess::spawn({initiator.string(), "--state", (dir / "raw-peer-state").string(),
                                           "--host", "127.0.0.1", "--port", std::to_string(port),
                                           "--timeout-ms", "12000"}, dir);
    std::uint64_t next_in = 1;
    std::uint64_t first_out_seq = 0;
    {
        auto conn = listener.accept(10'000);
        auto logon = conn.read_fix_frame(8'000);
        CHECK(logon.has_value() && (*logon)["35"] == "A" && (*logon)["34"] == "1");
        conn.send_all(tu::make_fix_wire("FIX.4.4", next_in++, "A", "SELL", "BUY", {{98, "0"}, {108, "30"}}));
        (void)wait_matching(client, std::regex("SESSION_ACTIVE"), "raw active");

        // Invalid command must not crash or send order
        client.write_line("order BAD-1 AAPL hold 1 12.50");
        CHECK(!conn.read_fix_frame(200).has_value());
        CHECK(client.poll_alive());

        client.write_line("order RAW-1 AAPL buy 5 12.50");
        auto ord1 = conn.read_fix_frame(8'000);
        CHECK(ord1.has_value() && (*ord1)["35"] == "D" && (*ord1)["11"] == "RAW-1" && (*ord1)["55"] == "AAPL" && (*ord1)["40"] == "2");
        first_out_seq = std::stoull((*ord1)["34"]);
        conn.send_all(tu::make_fix_wire("FIX.4.4", next_in++, "8", "SELL", "BUY",
            {{37, "ORD-RAW-1"}, {17, "EXEC-RAW-1"}, {11, "RAW-1"}, {150, "0"}, {39, "0"},
             {55, "AAPL"}, {54, "1"}, {151, "5"}, {14, "0"}, {6, "0"}, {60, tu::utc_now_fix_ms()}}));
        (void)wait_order_flow(client, "RAW-1", "AAPL", "buy", true);
        // Force disconnect -> reconnect
    }
    {
        auto conn = listener.accept(12'000);
        auto logon2 = conn.read_fix_frame(8'000);
        CHECK(logon2.has_value() && (*logon2)["35"] == "A");
        const std::uint64_t reconn_seq = std::stoull((*logon2)["34"]);
        CHECK(reconn_seq == first_out_seq + 1);
        conn.send_all(tu::make_fix_wire("FIX.4.4", next_in++, "A", "SELL", "BUY", {{98, "0"}, {108, "30"}}));
        CHECK(!conn.read_fix_frame(200).has_value());

        client.write_line("order RAW-2 MSFT sell 0.5 market");
        auto ord2 = conn.read_fix_frame(8'000);
        CHECK(ord2.has_value() && (*ord2)["35"] == "D" && (*ord2)["11"] == "RAW-2" && (*ord2)["40"] == "1" && !ord2->contains("44"));
        const std::uint64_t ord2_seq = std::stoull((*ord2)["34"]);
        CHECK(ord2_seq == reconn_seq + 1);

        client.write_line("quit");
        // Quit must wait for pending report before sending Logout
        CHECK(!conn.read_fix_frame(200).has_value());
        conn.send_all(tu::make_fix_wire("FIX.4.4", next_in++, "8", "SELL", "BUY",
            {{37, "ORD-RAW-2"}, {17, "EXEC-RAW-2"}, {11, "RAW-2"}, {150, "0"}, {39, "0"},
             {55, "MSFT"}, {54, "2"}, {151, "0.5"}, {14, "0"}, {6, "0"}, {60, tu::utc_now_fix_ms()}}));
        (void)wait_order_flow(client, "RAW-2", "MSFT", "sell", false);
        auto lo = conn.read_fix_frame(8'000);
        CHECK(lo.has_value() && (*lo)["35"] == "5" && std::stoull((*lo)["34"]) == ord2_seq + 1);
        conn.send_all(tu::make_fix_wire("FIX.4.4", next_in++, "5", "SELL", "BUY", {{58, "shutdown"}}));
    }
    CHECK(client.wait_exit(10'000).ok());
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path acceptor, initiator;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        if (a == "--acceptor" && i + 1 < argc) acceptor = std::filesystem::absolute(argv[++i]);
        else if (a == "--initiator" && i + 1 < argc) initiator = std::filesystem::absolute(argv[++i]);
    }
    if (acceptor.empty() || initiator.empty()) {
        std::cerr << "usage: endpoint-test --acceptor PATH --initiator PATH\n";
        return 2;
    }
    try {
        tu::TempDir tmp("nimblefix-endpoints-");
        exercise_paired_processes(acceptor, initiator, tmp.path);
        exercise_raw_peer(initiator, tmp.path);
    } catch (const std::exception& e) {
        std::cerr << "endpoint test error: " << e.what() << '\n';
        return 1;
    }
    if (g_failures != 0) return 1;
    std::cout << "interactive orders, persistence, reconnect, independent raw peer, and graceful shutdown passed\n";
    return 0;
}
