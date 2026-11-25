#include <nimblefix/engine.hpp>
#include <nimblefix/fix44_sample.hpp>
#include <nimblefix/io_backend.hpp>
#include <nimblefix/message.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nf = nimblefix;
namespace fix = nimblefix::fix44_sample;

namespace {

int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << __FILE__ << ":" << __LINE__ << ": failed: " #cond "\n"; ++g_failures; } } while (0)

struct TempDir {
    std::string path;
    TempDir() {
        char pat[] = "/tmp/nimblefix-iobackend-XXXXXX";
        char* p = ::mkdtemp(pat);
        if (!p) std::abort();
        path = p;
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

std::uint16_t pick_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { ::close(fd); return 0; }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) { ::close(fd); return 0; }
    const std::uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

template <class Policy>
void run_backend_roundtrip(nf::IoBackendKind backend_kind, std::size_t sessions_count, int orders_per_session,
                           const char* label) {
    TempDir dir_acc;
    TempDir dir_ini;
    const std::uint16_t port = pick_free_port();
    CHECK(port != 0);

    nf::EngineConfig acc_cfg{};
    acc_cfg.storage.directory = dir_acc.path;
    acc_cfg.storage.journal_capacity_bytes = 4U * 1024U * 1024U;
    acc_cfg.storage.segment_bytes = 512U * 1024U;
    acc_cfg.storage.journal_max_records = 16'384;
    acc_cfg.io_backend = backend_kind == nf::IoBackendKind::io_uring
        ? nf::IoBackendConfig::io_uring()
        : (backend_kind == nf::IoBackendKind::busy_poll
            ? nf::IoBackendConfig::busy_poll()
            : nf::IoBackendConfig::epoll());
    acc_cfg.heartbeat_ms = 30'000;

    auto acc_res = nf::Engine<fix::Schema, Policy>::create(acc_cfg);
    if (!acc_res.ok()) {
        std::cerr << "[" << label << "] acceptor create failed: code="
                  << static_cast<int>(acc_res.status().code)
                  << " errno=" << acc_res.status().offset << "\n";
    }
    CHECK(acc_res.ok());
    if (!acc_res.ok()) return;
    auto acceptor = std::move(acc_res).value();
    CHECK(acceptor.add_listener({"main", {"127.0.0.1", port}}).ok());

    for (std::size_t s = 0; s < sessions_count; ++s) {
        nf::AcceptorConfig ac{};
        ac.name = "acc-" + std::to_string(s);
        ac.listener = "main";
        ac.local_comp_id = "SELL";
        ac.remote_comp_id = "BUY-" + std::to_string(s);
        CHECK(acceptor.add_acceptor(ac).ok());
    }

    std::atomic<int> acc_received{0};
    CHECK(acceptor.template on<fix::NewOrderSingle>(
        [&](nf::SessionContext& ctx, const fix::NewOrderSingleView& req) -> nf::Status {
            acc_received.fetch_add(1, std::memory_order_relaxed);
            return ctx.send_stream<fix::ExecutionReport>([&](auto& w) {
                w.order_id("ORD-1")
                 .exec_id("EX-1")
                 .cl_ord_id(req.cl_ord_id().value())
                 .exec_type('0')
                 .ord_status('0')
                 .symbol(req.symbol().value())
                 .side(req.side().value())
                 .leaves_qty(nf::Decimal{100, 0})
                 .cum_qty(nf::Decimal{0, 0})
                 .avg_px(nf::Decimal{0, 0});
            }).status();
        }).ok());

    nf::EngineConfig ini_cfg = acc_cfg;
    ini_cfg.storage.directory = dir_ini.path;
    auto ini_res = nf::Engine<fix::Schema, Policy>::create(ini_cfg);
    CHECK(ini_res.ok());
    if (!ini_res.ok()) return;
    auto initiator = std::move(ini_res).value();

    std::vector<nf::SessionHandle> handles;
    for (std::size_t s = 0; s < sessions_count; ++s) {
        nf::InitiatorConfig ic{};
        ic.name = "ini-" + std::to_string(s);
        ic.sender_comp_id = "BUY-" + std::to_string(s);
        ic.target_comp_id = "SELL";
        ic.endpoint = {"127.0.0.1", port};
        auto h = initiator.add_initiator(ic);
        CHECK(h.ok());
        if (h.ok()) handles.push_back(std::move(h).value());
    }

    std::atomic<int> ini_received{0};
    CHECK(initiator.template on<fix::ExecutionReport>(
        [&](nf::SessionContext&, const fix::ExecutionReportView&) -> nf::Status {
            ini_received.fetch_add(1, std::memory_order_relaxed);
            return nf::Status::success();
        }).ok());

    CHECK(acceptor.on_error([&](const nf::ErrorEvent& ev) {
        std::cerr << "[" << label << " ACC ERR] " << ev.session_name << " op=" << ev.operation
                  << " code=" << static_cast<int>(ev.status.code) << " off=" << ev.status.offset << "\n";
    }).ok());
    CHECK(initiator.on_error([&](const nf::ErrorEvent& ev) {
        std::cerr << "[" << label << " INI ERR] " << ev.session_name << " op=" << ev.operation
                  << " code=" << static_cast<int>(ev.status.code) << " off=" << ev.status.offset << "\n";
    }).ok());

    std::thread acc_thread([&] { (void)acceptor.run(); });
    std::thread ini_thread([&] { (void)initiator.run(); });

    const auto active_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < active_deadline) {
        std::size_t active_n = 0;
        for (std::size_t s = 0; s < handles.size(); ++s) {
            const auto snap = initiator.snapshot("ini-" + std::to_string(s));
            if (snap.ok() && snap.value().state == nf::SessionState::active) ++active_n;
        }
        if (active_n == sessions_count) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    for (std::size_t s = 0; s < handles.size(); ++s) {
        auto sender = handles[s].sender<fix::Schema>();
        for (int i = 0; i < orders_per_session; ++i) {
            const auto cl_id = "IO-" + std::to_string(s) + "-" + std::to_string(i);
            CHECK(sender.send_stream<fix::NewOrderSingle>([&](auto& w) {
                w.cl_ord_id(cl_id)
                 .symbol("AAPL")
                 .side('1')
                 .transact_time("20261123-14:00:00.000")
                 .order_qty(nf::Decimal{100, 0})
                 .ord_type('1');
            }).ok());
        }
    }

    const int expected_total = static_cast<int>(sessions_count) * orders_per_session;
    const auto done_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ini_received.load(std::memory_order_relaxed) < expected_total &&
           std::chrono::steady_clock::now() < done_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(acc_received.load() == expected_total);
    CHECK(ini_received.load() == expected_total);
    if (acc_received.load() != expected_total || ini_received.load() != expected_total) {
        std::cerr << "[" << label << "] acc_received=" << acc_received.load()
                  << " ini_received=" << ini_received.load()
                  << " expected=" << expected_total << "\n";
    }

    initiator.request_stop();
    acceptor.request_stop();
    ini_thread.join();
    acc_thread.join();

    if (g_failures == 0) {
        std::cout << "[PASS] " << label << " (" << sessions_count << " sessions x "
                  << orders_per_session << " orders)\n";
    }
}

void test_abrupt_disconnect_and_reconnect(nf::IoBackendKind backend_kind) {
    TempDir dir_acc;
    TempDir dir_ini;
    const std::uint16_t port = pick_free_port();
    CHECK(port != 0);

    nf::EngineConfig acc_cfg{};
    acc_cfg.storage.directory = dir_acc.path;
    acc_cfg.storage.journal_capacity_bytes = 2U * 1024U * 1024U;
    acc_cfg.storage.segment_bytes = 256U * 1024U;
    acc_cfg.storage.journal_max_records = 4096;
    acc_cfg.io_backend = backend_kind == nf::IoBackendKind::io_uring
        ? nf::IoBackendConfig::io_uring()
        : (backend_kind == nf::IoBackendKind::busy_poll
            ? nf::IoBackendConfig::busy_poll()
            : nf::IoBackendConfig::epoll());
    acc_cfg.reconnect_delay_ms = 20;

    auto acc_res = nf::Engine<fix::Schema, nf::Inline>::create(acc_cfg);
    if (!acc_res.ok()) {
        std::cerr << "[reconnect] acceptor create failed: code="
                  << static_cast<int>(acc_res.status().code)
                  << " errno=" << acc_res.status().offset << "\n";
    }
    CHECK(acc_res.ok());
    if (!acc_res.ok()) return;
    auto acceptor = std::move(acc_res).value();
    CHECK(acceptor.add_listener({"main", {"127.0.0.1", port}}).ok());
    CHECK(acceptor.add_acceptor({"acc-0", "main", "SELL", "BUY-0"}).ok());
    CHECK(acceptor.on<fix::NewOrderSingle>([&](nf::SessionContext& ctx, const fix::NewOrderSingleView& req) {
        return ctx.send_stream<fix::ExecutionReport>([&](auto& w) {
            w.order_id("ORD-1").exec_id("EX-1").cl_ord_id(req.cl_ord_id().value())
             .exec_type('0').ord_status('0').symbol("AAPL").side('1')
             .leaves_qty(nf::Decimal{100, 0}).cum_qty(nf::Decimal{0, 0}).avg_px(nf::Decimal{0, 0});
        }).status();
    }).ok());

    std::thread acc_thread([&] { (void)acceptor.run(); });

    // Run two consecutive initiator lifecycles against the same io_uring acceptor (verifies clean cancel_fd + sweep).
    for (int cycle = 0; cycle < 2; ++cycle) {
        nf::EngineConfig ini_cfg = acc_cfg;
        ini_cfg.storage.directory = dir_ini.path;
        auto ini_res = nf::Engine<fix::Schema, nf::Inline>::create(ini_cfg);
        CHECK(ini_res.ok());
        auto initiator = std::move(ini_res).value();
        auto h = initiator.add_initiator({"ini-0", "BUY-0", "SELL", {"127.0.0.1", port}});
        CHECK(h.ok());
        std::atomic<int> got{0};
        CHECK(initiator.on<fix::ExecutionReport>([&](nf::SessionContext&, const fix::ExecutionReportView&) {
            got.fetch_add(1, std::memory_order_relaxed);
            return nf::Status::success();
        }).ok());
        std::thread ini_thread([&] { (void)initiator.run(); });
        const auto d1 = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool became_active = false;
        while (std::chrono::steady_clock::now() < d1) {
            auto s = initiator.snapshot("ini-0");
            if (s.ok() && s.value().state == nf::SessionState::active) {
                became_active = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!became_active) {
            auto s = initiator.snapshot("ini-0");
            auto as = acceptor.snapshot("acc-0");
            std::cerr << "[reconnect cycle " << cycle << "] NOT active! ini_state="
                      << (s.ok() ? static_cast<int>(s.value().state) : -1)
                      << " acc_state=" << (as.ok() ? static_cast<int>(as.value().state) : -1) << "\n";
        }
        auto sender = h.value().sender<fix::Schema>();
        for (int i = 0; i < 10; ++i) {
            const auto cl_id = "CYC-" + std::to_string(cycle) + "-" + std::to_string(i);
            CHECK(sender.send_stream<fix::NewOrderSingle>([&](auto& w) {
                w.cl_ord_id(cl_id)
                 .symbol("AAPL").side('1').transact_time("20261123-14:00:00.000")
                 .order_qty(nf::Decimal{100, 0}).ord_type('1');
            }).ok());
        }
        const auto d2 = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (got.load() < 10 && std::chrono::steady_clock::now() < d2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(got.load() == 10);
        initiator.request_stop();
        ini_thread.join();
    }

    acceptor.request_stop();
    acc_thread.join();
    if (g_failures == 0) std::cout << "[PASS] disconnect/reconnect lifecycle on backend\n";
}

} // namespace

int main() {
    run_backend_roundtrip<nf::Inline>(nf::IoBackendKind::epoll, 2, 25, "epoll Inline");
    run_backend_roundtrip<nf::Queued>(nf::IoBackendKind::epoll, 2, 25, "epoll Queued");
    run_backend_roundtrip<nf::Inline>(nf::IoBackendKind::busy_poll, 2, 25, "busy_poll Inline");

    if (nf::io_uring_available()) {
        run_backend_roundtrip<nf::Inline>(nf::IoBackendKind::io_uring, 1, 50, "io_uring Inline 1-session");
        run_backend_roundtrip<nf::Inline>(nf::IoBackendKind::io_uring, 4, 25, "io_uring Inline 4-session");
        run_backend_roundtrip<nf::Queued>(nf::IoBackendKind::io_uring, 2, 25, "io_uring Queued 2-session");
        test_abrupt_disconnect_and_reconnect(nf::IoBackendKind::io_uring);
    } else {
        TempDir dir;
        nf::EngineConfig cfg{};
        cfg.storage.directory = dir.path;
        cfg.io_backend.kind = nf::IoBackendKind::io_uring;
        const auto res = nf::Engine<fix::Schema, nf::Inline>::create(cfg);
        CHECK(!res.ok() && res.status().code == nf::ErrorCode::unsupported);
        std::cout << "[PASS] io_uring unavailable contract\n";
    }

    return g_failures == 0 ? 0 : 1;
}
