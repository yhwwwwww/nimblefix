#include "test_process_utils.hpp"

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace tu = nimblefix::test_util;

namespace {

int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #cond << '\n'; ++g_failures; } \
} while (false)

std::string wait_event(tu::ChildProcess& proc, std::string_view kind) {
    for (int i = 0; i < 64; ++i) {
        const auto line = proc.read_stdout_line(10'000);
        if (!line) throw std::runtime_error("missing CLI event: " + std::string{kind});
        if (tu::json_string_field(*line, "event") == kind) return *line;
    }
    throw std::runtime_error("missing CLI event: " + std::string{kind});
}

void exchange(const std::filesystem::path& binary, const std::filesystem::path& parent,
              std::string_view version, std::string_view role, std::string_view dispatch) {
    const auto dir = parent / (std::string{version} + "-" + std::string{role} + "-" + std::string{dispatch});
    std::filesystem::create_directories(dir);
    auto [listener, lport] = tu::RawTcpSocket::listen_loopback();
    const auto cfg_path = dir / "session.toml";
    std::string cfg =
        "version = 1\n[engine]\ndispatch = \"" + std::string{dispatch} + "\"\nschema = \"none\"\n"
        "logout_timeout_ms = 1000\n[engine.storage]\ndirectory = \"./state\"\nconfirmation = \"appended\"\n";
    if (role == "acceptor") {
        cfg += "[[listeners]]\nname = \"main\"\nendpoint = \"127.0.0.1:0\"\n"
               "[[sessions]]\nname = \"peer\"\nrole = \"acceptor\"\nlistener = \"main\"\n"
               "local_comp_id = \"SELL\"\nremote_comp_id = \"BUY\"\ntransport = \"" + std::string{version} + "\"\n";
    } else {
        cfg += "[[sessions]]\nname = \"peer\"\nrole = \"initiator\"\n"
               "sender_comp_id = \"SELL\"\ntarget_comp_id = \"BUY\"\n"
               "endpoint = \"127.0.0.1:" + std::to_string(lport) + "\"\ntransport = \"" + std::string{version} + "\"\n";
    }
    tu::write_file_bytes(cfg_path, cfg);

    auto proc = tu::ChildProcess::spawn({binary.string(), "run", "--config", cfg_path.string(), "--json"}, dir);
    {
        tu::RawTcpSocket peer;
        if (role == "acceptor") {
            const auto ep = wait_event(proc, "listening");
            peer = tu::RawTcpSocket::connect_loopback(static_cast<std::uint16_t>(tu::json_int_field(ep, "port").value_or(0)));
        } else {
            peer = listener.accept(10'000);
        }
        std::vector<std::pair<int, std::string>> lf{{98, "0"}, {108, "30"}};
        if (version == "FIXT.1.1") lf.emplace_back(1137, "9");
        if (role == "acceptor") peer.send_all(tu::make_fix_wire(version, 1, "A", "BUY", "SELL", lf));
        auto logon = peer.read_fix_frame(5'000);
        CHECK(logon.has_value() && (*logon)["8"] == version && (*logon)["35"] == "A");
        CHECK((*logon)["49"] == "SELL" && (*logon)["56"] == "BUY");
        if (version == "FIXT.1.1") CHECK((*logon)["1137"] == "9");
        if (role == "initiator") peer.send_all(tu::make_fix_wire(version, 1, "A", "BUY", "SELL", lf));
        (void)wait_event(proc, "active");

        peer.send_all(tu::make_fix_wire(version, 2, "D", "BUY", "SELL", {{11, "VERSION-ORDER"}, {55, "AAPL"}}));
        const auto m = wait_event(proc, "message");
        CHECK(tu::json_int_field(m, "business_messages").value_or(0) == 1);

        peer.send_all(tu::make_fix_wire(version, 3, "1", "BUY", "SELL", {{112, "INDEPENDENT-TEST"}}));
        auto hb = peer.read_fix_frame(5'000);
        CHECK(hb.has_value() && (*hb)["35"] == "0" && (*hb)["112"] == "INDEPENDENT-TEST");
        CHECK(std::stoull((*hb)["369"]) >= 2);

        peer.send_all(tu::make_fix_wire(version, 4, "2", "BUY", "SELL", {{7, "1"}, {16, "2"}}));
        auto gf = peer.read_fix_frame(5'000);
        CHECK(gf.has_value() && (*gf)["35"] == "4" && (*gf)["43"] == "Y");
        CHECK((*gf)["123"] == "Y" && (*gf)["34"] == "1" && (*gf)["36"] == "3");

        proc.send_signal(SIGTERM);
        auto lo = peer.read_fix_frame(5'000);
        CHECK(lo.has_value() && (*lo)["35"] == "5");
        peer.send_all(tu::make_fix_wire(version, 5, "5", "BUY", "SELL"));
    }
    const auto done = proc.wait_exit(10'000);
    CHECK(done.exit_code == 128 + SIGTERM);
    CHECK(done.stdout_text.find("\"event\":\"stopped\"") != std::string::npos);
    CHECK(done.stdout_text.find("\"code\":0") != std::string::npos);
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
        std::cerr << "usage: protocol_versions-test --binary PATH\n";
        return 2;
    }
    try {
        tu::TempDir tmp("nimblefix-versions-");
        for (const char* v : {"FIX.4.2", "FIX.4.3", "FIX.4.4", "FIXT.1.1"}) {
            for (const char* role : {"initiator", "acceptor"}) {
                for (const char* disp : {"inline", "queued"}) {
                    exchange(binary, tmp.path, v, role, disp);
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "protocol_versions test error: " << e.what() << '\n';
        return 1;
    }
    if (g_failures != 0) return 1;
    std::cout << "16 transport/role/dispatch exchanges passed: Logon, delivery, 369, heartbeat, replay, Logout\n";
    return 0;
}
