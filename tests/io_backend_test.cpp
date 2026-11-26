#include <nimblefix/engine.hpp>
#include <nimblefix/fix44_sample.hpp>
#include <nimblefix/io_backend.hpp>
#include <nimblefix/message.hpp>
#include <nimblefix/sharded_engine.hpp>
#include "../src/runtime/io_backend.hpp"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sched.h>
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

std::vector<int> allowed_cpus() {
    std::vector<int> cpus;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (::sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &set)) cpus.push_back(cpu);
        }
    }
    if (cpus.empty()) cpus.push_back(0);
    return cpus;
}

template <class EngineT>
std::uint16_t wait_listener_port(const EngineT& engine, std::string_view name,
                                 std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto ep = engine.listener_endpoint(name);
        if (ep.ok() && ep.value().port != 0) return ep.value().port;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
}

template <class Policy>
void run_backend_roundtrip(nf::IoBackendKind backend_kind, std::size_t sessions_count, int orders_per_session,
                           const char* label) {
    TempDir dir_acc;
    TempDir dir_ini;

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
    CHECK(acceptor.add_listener({"main", {"127.0.0.1", 0}}).ok());

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

    CHECK(acceptor.on_error([&](const nf::ErrorEvent& ev) {
        std::cerr << "[" << label << " ACC ERR] " << ev.session_name << " op=" << ev.operation
                  << " code=" << static_cast<int>(ev.status.code) << " off=" << ev.status.offset << "\n";
    }).ok());

    std::thread acc_thread([&] { (void)acceptor.run(); });
    const std::uint16_t port = wait_listener_port(acceptor, "main");
    CHECK(port != 0);
    if (port == 0) {
        acceptor.request_stop();
        acc_thread.join();
        return;
    }

    nf::EngineConfig ini_cfg = acc_cfg;
    ini_cfg.storage.directory = dir_ini.path;
    auto ini_res = nf::Engine<fix::Schema, Policy>::create(ini_cfg);
    CHECK(ini_res.ok());
    if (!ini_res.ok()) {
        acceptor.request_stop();
        acc_thread.join();
        return;
    }
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

    CHECK(initiator.on_error([&](const nf::ErrorEvent& ev) {
        std::cerr << "[" << label << " INI ERR] " << ev.session_name << " op=" << ev.operation
                  << " code=" << static_cast<int>(ev.status.code) << " off=" << ev.status.offset << "\n";
    }).ok());

    std::thread ini_thread([&] { (void)initiator.run(); });

    const auto active_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::size_t active_n = 0;
    while (std::chrono::steady_clock::now() < active_deadline) {
        active_n = 0;
        for (std::size_t s = 0; s < handles.size(); ++s) {
            const auto snap = initiator.snapshot("ini-" + std::to_string(s));
            if (snap.ok() && snap.value().state == nf::SessionState::active) ++active_n;
        }
        if (active_n == sessions_count) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(active_n == sessions_count);

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
    const auto done_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
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
    ini_thread.join();
    acceptor.request_stop();
    acc_thread.join();

    if (g_failures == 0) {
        std::cout << "[PASS] " << label << " (" << sessions_count << " sessions x "
                  << orders_per_session << " orders)\n";
    }
}

void test_abrupt_disconnect_and_reconnect(nf::IoBackendKind backend_kind) {
    TempDir dir_acc;
    TempDir dir_ini;

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
    CHECK(acceptor.add_listener({"main", {"127.0.0.1", 0}}).ok());
    CHECK(acceptor.add_acceptor({"acc-0", "main", "SELL", "BUY-0"}).ok());
    CHECK(acceptor.on<fix::NewOrderSingle>([&](nf::SessionContext& ctx, const fix::NewOrderSingleView& req) {
        return ctx.send_stream<fix::ExecutionReport>([&](auto& w) {
            w.order_id("ORD-1").exec_id("EX-1").cl_ord_id(req.cl_ord_id().value())
             .exec_type('0').ord_status('0').symbol("AAPL").side('1')
             .leaves_qty(nf::Decimal{100, 0}).cum_qty(nf::Decimal{0, 0}).avg_px(nf::Decimal{0, 0});
        }).status();
    }).ok());

    std::thread acc_thread([&] { (void)acceptor.run(); });
    const std::uint16_t port = wait_listener_port(acceptor, "main");
    CHECK(port != 0);
    if (port == 0) {
        acceptor.request_stop();
        acc_thread.join();
        return;
    }

    // Run two consecutive initiator lifecycles against the same io_uring acceptor (verifies clean cancel_fd + sweep).
    for (int cycle = 0; cycle < 2; ++cycle) {
        if (cycle > 0) {
            const auto d_drop = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < d_drop) {
                const auto as = acceptor.snapshot("acc-0");
                if (as.ok() && as.value().state != nf::SessionState::active) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
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
        const auto d1 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
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
        CHECK(became_active);
        auto sender = h.value().sender<fix::Schema>();
        for (int i = 0; i < 10; ++i) {
            const auto cl_id = "CYC-" + std::to_string(cycle) + "-" + std::to_string(i);
            CHECK(sender.send_stream<fix::NewOrderSingle>([&](auto& w) {
                w.cl_ord_id(cl_id)
                 .symbol("AAPL").side('1').transact_time("20261123-14:00:00.000")
                 .order_qty(nf::Decimal{100, 0}).ord_type('1');
            }).ok());
        }
        const auto d2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
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

void test_sharded_reactor(nf::IoBackendConfig backend_cfg, const char* label) {
    // ShardedEngine: 4 shards (with core affinity + idle strategy), 1 shared port,
    // 8 Acceptor sessions & 8 Sharded Initiator sessions across 4 shards.
    TempDir dir_acc;
    TempDir dir_ini;

    constexpr std::size_t kShards = 4;
    constexpr std::size_t kSessions = 8;
    constexpr int kOrdersPerSession = 30;
    const auto cpus = allowed_cpus();

    nf::ShardedEngineConfig acc_scfg{};
    acc_scfg.shard_count = kShards;
    for (std::size_t i = 0; i < kShards; ++i) {
        acc_scfg.cpu_cores.push_back(cpus[i % cpus.size()]);
    }
    acc_scfg.base.storage.directory = dir_acc.path;
    acc_scfg.base.storage.journal_capacity_bytes = 2U * 1024U * 1024U;
    acc_scfg.base.storage.segment_bytes = 256U * 1024U;
    acc_scfg.base.storage.journal_max_records = 4096;
    acc_scfg.base.io_backend = backend_cfg;

    const std::filesystem::path acc_tel = std::filesystem::path{dir_acc.path} / "acceptor.nfxt";
    acc_scfg.base.telemetry_path = acc_tel;
    acc_scfg.base.telemetry_stage_probes = true;

    nf::ShardedEngineConfig ini_scfg = acc_scfg;
    ini_scfg.base.storage.directory = dir_ini.path;
    const std::filesystem::path ini_tel = std::filesystem::path{dir_ini.path} / "initiator.nfxt";
    ini_scfg.base.telemetry_path = ini_tel;
    ini_scfg.base.telemetry_stage_probes = true;

    auto acc_res = nf::ShardedEngine<fix::Schema, nf::Inline>::create(acc_scfg);
    auto ini_res = nf::ShardedEngine<fix::Schema, nf::Inline>::create(ini_scfg);
    CHECK(acc_res.ok() && ini_res.ok());
    if (!acc_res.ok() || !ini_res.ok()) return;
    auto acceptor = std::move(acc_res).value();
    auto initiator = std::move(ini_res).value();
    CHECK(acceptor.shard_count() == kShards);
    CHECK(initiator.shard_count() == kShards);

    CHECK(acceptor.add_listener({"main", {"127.0.0.1", 0}}).ok());

    for (std::size_t s = 0; s < kSessions; ++s) {
        const std::string acc_name = "acc-" + std::to_string(s);
        const std::string client_id = "SHARD-BUY-" + std::to_string(s);
        CHECK(acceptor.add_acceptor({acc_name, "main", "SELL", client_id}).ok());
        CHECK(acceptor.session_shard(acc_name) == (s % kShards));
    }

    std::atomic<int> acc_received{0};
    std::atomic<int> ini_received{0};
    std::mutex tid_mu;
    std::unordered_map<std::string, std::thread::id> session_tid;
    std::set<std::thread::id> distinct_acc_tids;

    CHECK(acceptor.on<fix::NewOrderSingle>([&](nf::SessionContext& ctx, const fix::NewOrderSingleView& req) {
        const auto tid = std::this_thread::get_id();
        {
            std::lock_guard lock(tid_mu);
            const std::string sname{ctx.session_name()};
            auto [it, inserted] = session_tid.emplace(sname, tid);
            if (!inserted) CHECK(it->second == tid); // Every session stays pinned to 1 shard thread!
            distinct_acc_tids.insert(tid);
        }
        acc_received.fetch_add(1, std::memory_order_relaxed);
        return ctx.send_stream<fix::ExecutionReport>([&](auto& w) {
            w.order_id("ORD-SHARD").exec_id("EX-SHARD").cl_ord_id(req.cl_ord_id().value())
             .exec_type('0').ord_status('0').symbol("AAPL").side('1')
             .leaves_qty(nf::Decimal{100, 0}).cum_qty(nf::Decimal{0, 0}).avg_px(nf::Decimal{0, 0});
        }).status();
    }).ok());

    CHECK(acceptor.validate().ok());
    std::thread acc_thread([&] { (void)acceptor.run(); });
    const std::uint16_t port = wait_listener_port(acceptor, "main");
    CHECK(port != 0);
    if (port == 0) {
        acceptor.request_stop();
        acc_thread.join();
        return;
    }

    std::vector<nf::SessionHandle> handles;
    handles.reserve(kSessions);
    for (std::size_t s = 0; s < kSessions; ++s) {
        const std::string ini_name = "ini-" + std::to_string(s);
        const std::string client_id = "SHARD-BUY-" + std::to_string(s);
        auto h = initiator.add_initiator({ini_name, client_id, "SELL", {"127.0.0.1", port}});
        CHECK(h.ok());
        CHECK(initiator.session_shard(ini_name) == (s % kShards));
        if (h.ok()) handles.push_back(std::move(h).value());
    }

    CHECK(initiator.on<fix::ExecutionReport>([&](nf::SessionContext&, const fix::ExecutionReportView&) {
        ini_received.fetch_add(1, std::memory_order_relaxed);
        return nf::Status::success();
    }).ok());

    CHECK(initiator.validate().ok());
    std::thread ini_thread([&] { (void)initiator.run(); });

    const auto active_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::size_t active_n = 0;
    while (std::chrono::steady_clock::now() < active_deadline) {
        active_n = 0;
        for (std::size_t s = 0; s < handles.size(); ++s) {
            const auto snap = initiator.snapshot("ini-" + std::to_string(s));
            if (snap.ok() && snap.value().state == nf::SessionState::active) ++active_n;
        }
        if (active_n == kSessions) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(active_n == kSessions);

    for (std::size_t s = 0; s < handles.size(); ++s) {
        auto sender = handles[s].sender<fix::Schema>();
        for (int i = 0; i < kOrdersPerSession; ++i) {
            const auto cl_id = "SH-" + std::to_string(s) + "-" + std::to_string(i);
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

    const int expected_total = static_cast<int>(kSessions) * kOrdersPerSession;
    const auto done_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (ini_received.load(std::memory_order_relaxed) < expected_total &&
           std::chrono::steady_clock::now() < done_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(acc_received.load() == expected_total);
    CHECK(ini_received.load() == expected_total);
    CHECK(distinct_acc_tids.size() == kShards);

    // Out-of-band read-only sidecar check on shared ShardedEngine mmap telemetry region.
    // Poll briefly so every shard finishes its next publish_snapshots() pass after the final callback.
    {
        auto acc_reader = nf::TelemetryReader::open(acc_tel);
        auto ini_reader = nf::TelemetryReader::open(ini_tel);
        CHECK(acc_reader.ok() && ini_reader.ok());
        if (acc_reader.ok() && ini_reader.ok()) {
            const auto tel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            std::size_t acc_sz = 0, ini_sz = 0;
            std::uint64_t acc_deliv = 0, ini_deliv = 0, acc_val = 0, ini_jrn = 0;
            while (std::chrono::steady_clock::now() < tel_deadline) {
                const auto acc_snaps = acc_reader.value().read_all();
                const auto ini_snaps = ini_reader.value().read_all();
                acc_sz = acc_snaps.size();
                ini_sz = ini_snaps.size();
                acc_deliv = ini_deliv = acc_val = ini_jrn = 0;
                for (const auto& s : acc_snaps) {
                    acc_deliv += s.business_deliveries;
                    acc_val += s.stages[static_cast<std::size_t>(nf::TelemetryStage::validate)].count;
                }
                for (const auto& s : ini_snaps) {
                    ini_deliv += s.business_deliveries;
                    ini_jrn += s.stages[static_cast<std::size_t>(nf::TelemetryStage::journal)].count;
                }
                if (acc_sz == kSessions && ini_sz == kSessions &&
                    acc_deliv == static_cast<std::uint64_t>(expected_total) &&
                    ini_deliv == static_cast<std::uint64_t>(expected_total) &&
                    acc_val == static_cast<std::uint64_t>(expected_total) &&
                    ini_jrn == static_cast<std::uint64_t>(expected_total)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            CHECK(acc_sz == kSessions);
            CHECK(ini_sz == kSessions);
            CHECK(acc_deliv == static_cast<std::uint64_t>(expected_total));
            CHECK(ini_deliv == static_cast<std::uint64_t>(expected_total));
            CHECK(acc_val == static_cast<std::uint64_t>(expected_total));
            CHECK(ini_jrn == static_cast<std::uint64_t>(expected_total));
        }
    }

    initiator.request_stop();
    ini_thread.join();
    acceptor.request_stop();
    acc_thread.join();

    if (g_failures == 0) {
        std::cout << "[PASS] ShardedEngine (" << kShards << " shards pinned, "
                  << kSessions << " sessions x " << kOrdersPerSession
                  << " orders, " << label << ", 1 shared port)\n";
    }
}

void test_core_affinity_idle_strategy_and_sharded_reactor() {
    // 1. CPU affinity helper contract (run on a throwaway thread so the main
    //    test thread's CPU mask is never narrowed for subsequent child threads).
    const int valid_cpu = allowed_cpus().front();
    std::thread affinity_probe([valid_cpu] {
        CHECK(nf::pin_current_thread(-1).ok());
        CHECK(nf::pin_current_thread(valid_cpu).ok());
        CHECK(nf::pin_current_thread(99999).code == nf::ErrorCode::invalid_config);
    });
    affinity_probe.join();

    // 2. ShardedEngine on epoll + hybrid_backoff
    test_sharded_reactor(nf::IoBackendConfig::hybrid(200, 50), "epoll+hybrid_backoff");
}

// Deterministic mock Proactor backend to verify R01 two-phase send semantics:
// delayed completion, partial send completion, -EPIPE failure after staging, and
// submit_send() returning false (backpressure retry without losing staged bytes).
struct MockProactorControl {
    enum class Action { normal, hold, release_partial, release_all, fail_epipe };
    std::mutex mu;
    bool intercept_orders{false};
    bool hold_next_send{false};
    int reject_next_order_submits{0};
    Action action{Action::normal};
    std::size_t partial_bytes{20};
    bool send_is_held{false};
    std::size_t held_total_bytes{0};
    std::size_t rejected_submit_count{0};
};

MockProactorControl* g_mock_proactor_ctl = nullptr;

class MockProactorBackend final : public nf::detail::IoBackend {
public:
    MockProactorBackend(std::unique_ptr<nf::detail::IoBackend> inner, MockProactorControl* ctl)
        : inner_(std::move(inner)), ctl_(ctl) {}

    [[nodiscard]] nf::IoBackendKind kind() const noexcept override { return nf::IoBackendKind::io_uring; }
    [[nodiscard]] bool supports_proactor() const noexcept override { return true; }

    void sync_readiness(nf::detail::IoTag& tag, int fd, std::uint32_t desired_events) noexcept override {
        inner_->sync_readiness(tag, fd, desired_events);
    }
    void remove(nf::detail::IoTag& tag) noexcept override {
        inner_->remove(tag);
    }

    [[nodiscard]] bool submit_recv(nf::detail::IoTag& tag, int fd, std::span<char> buffer) noexcept override {
        pending_recvs_.push_back({&tag, fd, buffer});
        return true;
    }

    [[nodiscard]] bool submit_send(nf::detail::IoTag& tag, int fd, std::span<const char> buffer) noexcept override {
        const std::string_view sv{buffer.data(), buffer.size()};
        const bool is_order = sv.find("35=D\x01") != std::string_view::npos;
        if (ctl_ != nullptr) {
            std::lock_guard lock(ctl_->mu);
            if (is_order && ctl_->reject_next_order_submits > 0) {
                --ctl_->reject_next_order_submits;
                ++ctl_->rejected_submit_count;
                return false;
            }
            if ((ctl_->hold_next_send || (is_order && ctl_->intercept_orders)) &&
                ctl_->action == MockProactorControl::Action::hold) {
                ctl_->hold_next_send = false;
                held_send_ = PendingSend{&tag, fd, buffer};
                has_held_send_ = true;
                ctl_->send_is_held = true;
                ctl_->held_total_bytes = buffer.size();
                return true;
            }
        }
        pending_sends_.push_back({&tag, fd, buffer});
        return true;
    }

    void cancel_fd(int fd) noexcept override {
        for (auto it = pending_recvs_.begin(); it != pending_recvs_.end();) {
            if (it->fd == fd) {
                ready_.push_back({it->tag, nf::detail::IoCompletionKind::recv_done, 0, -ECANCELED});
                it = pending_recvs_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = pending_sends_.begin(); it != pending_sends_.end();) {
            if (it->fd == fd) {
                ready_.push_back({it->tag, nf::detail::IoCompletionKind::send_done, 0, -ECANCELED});
                it = pending_sends_.erase(it);
            } else {
                ++it;
            }
        }
        if (has_held_send_ && held_send_.fd == fd) {
            ready_.push_back({held_send_.tag, nf::detail::IoCompletionKind::send_done, 0, -ECANCELED});
            has_held_send_ = false;
            if (ctl_ != nullptr) {
                std::lock_guard lock(ctl_->mu);
                ctl_->send_is_held = false;
            }
        }
    }

    [[nodiscard]] int wait(std::span<nf::detail::IoCompletion> out, std::uint32_t wait_ms) noexcept override {
        std::size_t count = 0;
        auto push_comp = [&](nf::detail::IoCompletion c) -> bool {
            if (count >= out.size()) return false;
            out[count++] = c;
            return true;
        };

        // 1. Drain already queued completions (e.g. from cancel_fd).
        while (!ready_.empty() && count < out.size()) {
            out[count++] = ready_.front();
            ready_.erase(ready_.begin());
        }

        // 2. Process held send if test changed action.
        if (has_held_send_ && ctl_ != nullptr && count < out.size()) {
            MockProactorControl::Action act = MockProactorControl::Action::hold;
            std::size_t partial_n = 0;
            {
                std::lock_guard lock(ctl_->mu);
                act = ctl_->action;
                partial_n = ctl_->partial_bytes;
            }
            if (act == MockProactorControl::Action::fail_epipe) {
                auto s = held_send_;
                has_held_send_ = false;
                {
                    std::lock_guard lock(ctl_->mu);
                    ctl_->send_is_held = false;
                    ctl_->action = MockProactorControl::Action::normal;
                }
                push_comp({s.tag, nf::detail::IoCompletionKind::send_done, 0, -EPIPE});
            } else if (act == MockProactorControl::Action::release_partial) {
                auto s = held_send_;
                has_held_send_ = false;
                const std::size_t to_send = std::min(partial_n, s.buf.size());
                const ssize_t n = ::send(s.fd, s.buf.data(), to_send, MSG_NOSIGNAL | MSG_DONTWAIT);
                const std::int32_t res = n >= 0 ? static_cast<std::int32_t>(n) : -errno;
                {
                    std::lock_guard lock(ctl_->mu);
                    ctl_->send_is_held = false;
                    // Hold the subsequent send for the remainder until test releases it!
                    ctl_->hold_next_send = true;
                    ctl_->action = MockProactorControl::Action::hold;
                }
                push_comp({s.tag, nf::detail::IoCompletionKind::send_done, 0, res});
            } else if (act == MockProactorControl::Action::release_all ||
                       act == MockProactorControl::Action::normal) {
                auto s = held_send_;
                has_held_send_ = false;
                const ssize_t n = ::send(s.fd, s.buf.data(), s.buf.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
                const std::int32_t res = n >= 0 ? static_cast<std::int32_t>(n) : -errno;
                {
                    std::lock_guard lock(ctl_->mu);
                    ctl_->send_is_held = false;
                    ctl_->action = MockProactorControl::Action::normal;
                }
                push_comp({s.tag, nf::detail::IoCompletionKind::send_done, 0, res});
            }
        }

        // 3. Process normal pending sends.
        for (auto it = pending_sends_.begin(); it != pending_sends_.end() && count < out.size();) {
            const ssize_t n = ::send(it->fd, it->buf.data(), it->buf.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                ++it;
                continue;
            }
            const std::int32_t res = n >= 0 ? static_cast<std::int32_t>(n) : -errno;
            push_comp({it->tag, nf::detail::IoCompletionKind::send_done, 0, res});
            it = pending_sends_.erase(it);
        }

        // 4. Poll pending recvs non-blockingly.
        for (auto it = pending_recvs_.begin(); it != pending_recvs_.end() && count < out.size();) {
            const ssize_t n = ::recv(it->fd, it->buf.data(), it->buf.size(), MSG_DONTWAIT);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                ++it;
                continue;
            }
            const std::int32_t res = n >= 0 ? static_cast<std::int32_t>(n) : -errno;
            push_comp({it->tag, nf::detail::IoCompletionKind::recv_done, 0, res});
            it = pending_recvs_.erase(it);
        }

        if (count > 0) return static_cast<int>(count);
        const std::uint32_t slice_ms = (!pending_recvs_.empty() || !pending_sends_.empty() || has_held_send_)
            ? std::min<std::uint32_t>(wait_ms, 1U) : wait_ms;
        return inner_->wait(out, slice_ms);
    }

private:
    struct PendingRecv { nf::detail::IoTag* tag; int fd; std::span<char> buf; };
    struct PendingSend { nf::detail::IoTag* tag; int fd; std::span<const char> buf; };

    std::unique_ptr<nf::detail::IoBackend> inner_;
    MockProactorControl* ctl_{};
    std::vector<PendingRecv> pending_recvs_;
    std::vector<PendingSend> pending_sends_;
    std::vector<nf::detail::IoCompletion> ready_;
    PendingSend held_send_{};
    bool has_held_send_{false};
};

nf::Result<std::unique_ptr<nf::detail::IoBackend>> mock_proactor_factory(
    const nf::IoBackendConfig& config) noexcept {
    if (config.kind != nf::IoBackendKind::io_uring) {
        nf::detail::set_io_backend_test_factory(nullptr);
        auto inner = nf::detail::create_io_backend(config);
        nf::detail::set_io_backend_test_factory(&mock_proactor_factory);
        return inner;
    }
    nf::detail::set_io_backend_test_factory(nullptr);
    auto inner = nf::detail::create_io_backend(nf::IoBackendConfig::epoll());
    nf::detail::set_io_backend_test_factory(&mock_proactor_factory);
    if (!inner.ok()) return inner.status();
    return std::unique_ptr<nf::detail::IoBackend>(
        new (std::nothrow) MockProactorBackend(std::move(inner).value(), g_mock_proactor_ctl));
}

void test_proactor_send_commit_contract() {
    struct FactoryReset {
        ~FactoryReset() {
            nf::detail::set_io_backend_test_factory(nullptr);
            g_mock_proactor_ctl = nullptr;
        }
    } reset_guard;

    TempDir dir_acc;
    TempDir dir_ini;

    // Acceptor runs on normal epoll; Initiator runs on MockProactorBackend.
    nf::EngineConfig acc_cfg{};
    acc_cfg.storage.directory = dir_acc.path;
    acc_cfg.storage.journal_capacity_bytes = 2U * 1024U * 1024U;
    acc_cfg.storage.segment_bytes = 256U * 1024U;
    acc_cfg.storage.journal_max_records = 4096;
    acc_cfg.io_backend = nf::IoBackendConfig::epoll();
    auto acc_res = nf::Engine<fix::Schema, nf::Inline>::create(acc_cfg);
    CHECK(acc_res.ok());
    if (!acc_res.ok()) return;
    auto acceptor = std::move(acc_res).value();
    CHECK(acceptor.add_listener({"main", {"127.0.0.1", 0}}).ok());
    CHECK(acceptor.add_acceptor({"acc-0", "main", "SELL", "BUY"}).ok());

    std::mutex ord_mu;
    std::vector<std::string> acc_orders;
    CHECK(acceptor.on<fix::NewOrderSingle>([&](nf::SessionContext&, const fix::NewOrderSingleView& req) {
        std::lock_guard lock(ord_mu);
        acc_orders.emplace_back(req.cl_ord_id().value());
        return nf::Status::success();
    }).ok());

    std::thread acc_thread([&] { (void)acceptor.run(); });
    const std::uint16_t port = wait_listener_port(acceptor, "main");
    CHECK(port != 0);
    if (port == 0) {
        acceptor.request_stop();
        acc_thread.join();
        return;
    }

    MockProactorControl ctl{};
    g_mock_proactor_ctl = &ctl;
    nf::detail::set_io_backend_test_factory(&mock_proactor_factory);

    nf::EngineConfig ini_cfg{};
    ini_cfg.storage.directory = dir_ini.path;
    ini_cfg.storage.journal_capacity_bytes = 2U * 1024U * 1024U;
    ini_cfg.storage.segment_bytes = 256U * 1024U;
    ini_cfg.storage.journal_max_records = 4096;
    ini_cfg.reconnect_delay_ms = 60'000;
    ini_cfg.io_backend = nf::IoBackendConfig::io_uring();
    auto ini_res = nf::Engine<fix::Schema, nf::Inline>::create(ini_cfg);
    CHECK(ini_res.ok());
    if (!ini_res.ok()) {
        acceptor.request_stop();
        acc_thread.join();
        return;
    }
    auto initiator = std::move(ini_res).value();
    auto h = initiator.add_initiator({"ini-0", "BUY", "SELL", {"127.0.0.1", port}});
    CHECK(h.ok());
    auto lane_res = h.value().producer_lane<fix::Schema>({.capacity_messages = 64,
        .capacity_bytes = 65536, .receipt_capacity = 64});
    CHECK(lane_res.ok());
    auto lane = std::move(lane_res).value();

    std::thread ini_thread([&] { (void)initiator.run(); });

    const auto d0 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < d0) {
        auto s = initiator.snapshot("ini-0");
        if (s.ok() && s.value().state == nf::SessionState::active) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(initiator.snapshot("ini-0").value().state == nf::SessionState::active);

    // Case 1: Delayed partial SEND -> receipt stays `appended` & written_bytes unchanged
    //         until final byte completes!
    const auto written_before = initiator.snapshot("ini-0").value().written_bytes;
    {
        std::lock_guard lock(ctl.mu);
        ctl.intercept_orders = true;
        ctl.action = MockProactorControl::Action::hold;
        ctl.partial_bytes = 20;
    }
    const auto send1 = lane.send_stream<fix::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("PROACTOR-PARTIAL-1").symbol("AAPL").side('1')
         .transact_time("20261123-14:00:00.000").order_qty(nf::Decimal{100, 0}).ord_type('1');
    });
    CHECK(send1.ok());

    const auto d1 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < d1) {
        {
            std::lock_guard lock(ctl.mu);
            if (ctl.send_is_held) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        std::lock_guard lock(ctl.mu);
        CHECK(ctl.send_is_held);
        CHECK(ctl.held_total_bytes > 40);
    }
    // Query receipt while SEND is still in flight: MUST be `appended`, NOT `written`!
    {
        auto rec = lane.receipt(send1.value().id);
        const auto d_rec = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < d_rec && (!rec.ok() || rec.value().state == nf::ReceiptState::accepted)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            rec = lane.receipt(send1.value().id);
        }
        CHECK(rec.ok() && rec.value().state == nf::ReceiptState::appended);
    }
    CHECK(initiator.snapshot("ini-0").value().written_bytes == written_before);

    // Release ONLY the first 20 bytes -> receipt MUST STILL be `appended`, written_bytes += 20!
    {
        std::lock_guard lock(ctl.mu);
        ctl.action = MockProactorControl::Action::release_partial;
    }
    const auto d2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < d2) {
        if (initiator.snapshot("ini-0").value().written_bytes == written_before + 20) {
            std::lock_guard lock(ctl.mu);
            if (ctl.send_is_held) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(initiator.snapshot("ini-0").value().written_bytes == written_before + 20);
    {
        const auto rec = lane.receipt(send1.value().id);
        CHECK(rec.ok() && rec.value().state == nf::ReceiptState::appended);
    }

    // Now release the remainder -> receipt transitions to `written` and Acceptor receives full order!
    {
        std::lock_guard lock(ctl.mu);
        ctl.action = MockProactorControl::Action::release_all;
        ctl.intercept_orders = false;
    }
    bool saw_written = false;
    const auto d3 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < d3 && !saw_written) {
        const auto rec = lane.receipt(send1.value().id);
        if (rec.ok() && rec.value().state == nf::ReceiptState::written) saw_written = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(saw_written);
    const auto d3b = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < d3b) {
        {
            std::lock_guard lock(ord_mu);
            if (!acc_orders.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        std::lock_guard lock(ord_mu);
        CHECK(acc_orders.size() == 1 && acc_orders[0] == "PROACTOR-PARTIAL-1");
    }

    // Case 2: submit_send() returns false twice before succeeding -> staged bytes MUST be preserved and delivered!
    {
        std::lock_guard lock(ctl.mu);
        ctl.reject_next_order_submits = 2;
        ctl.rejected_submit_count = 0;
        ctl.intercept_orders = false;
    }
    const auto send2 = lane.send_stream<fix::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("PROACTOR-RETRY-2").symbol("MSFT").side('2')
         .transact_time("20261123-14:00:01.000").order_qty(nf::Decimal{50, 0}).ord_type('1');
    });
    CHECK(send2.ok());
    bool saw_written2 = false;
    const auto d4 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < d4) {
        const auto rec = lane.receipt(send2.value().id);
        if (rec.ok() && rec.value().state == nf::ReceiptState::written) saw_written2 = true;
        {
            std::lock_guard lock(ord_mu);
            if (saw_written2 && acc_orders.size() >= 2) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(saw_written2);
    {
        std::lock_guard lock(ctl.mu);
        CHECK(ctl.rejected_submit_count == 2);
    }
    {
        std::lock_guard lock(ord_mu);
        CHECK(acc_orders.size() == 2 && acc_orders[1] == "PROACTOR-RETRY-2");
    }

    // Case 3: Delayed SEND fails with -EPIPE -> receipt MUST NEVER become `written`!
    const auto written_before_fail = initiator.snapshot("ini-0").value().written_bytes;
    {
        std::lock_guard lock(ctl.mu);
        ctl.intercept_orders = true;
        ctl.action = MockProactorControl::Action::hold;
    }
    const auto send3 = lane.send_stream<fix::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("PROACTOR-EPIPE-3").symbol("NVDA").side('1')
         .transact_time("20261123-14:00:02.000").order_qty(nf::Decimal{10, 0}).ord_type('1');
    });
    CHECK(send3.ok());
    const auto d5 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < d5) {
        {
            std::lock_guard lock(ctl.mu);
            if (ctl.send_is_held) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        std::lock_guard lock(ctl.mu);
        CHECK(ctl.send_is_held);
        ctl.action = MockProactorControl::Action::fail_epipe;
        ctl.intercept_orders = false;
    }
    // Wait for -EPIPE disconnect to process (reconnect_delay_ms is 60s so no reconnect races written_bytes).
    bool saw_written3 = false;
    bool saw_disconnected3 = false;
    const auto d6 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < d6) {
        const auto rec = lane.receipt(send3.value().id);
        if (rec.ok() && rec.value().state == nf::ReceiptState::written) saw_written3 = true;
        const auto snap = initiator.snapshot("ini-0");
        if (snap.ok() && snap.value().state != nf::SessionState::active) {
            saw_disconnected3 = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(saw_disconnected3);
    CHECK(!saw_written3);
    CHECK(initiator.snapshot("ini-0").value().written_bytes == written_before_fail);

    initiator.request_stop();
    ini_thread.join();
    acceptor.request_stop();
    acc_thread.join();

    if (g_failures == 0) {
        std::cout << "[PASS] deterministic Proactor two-phase send contract (partial, retry, -EPIPE)\n";
    }
}

} // namespace

int main() {
    test_proactor_send_commit_contract();
    run_backend_roundtrip<nf::Inline>(nf::IoBackendKind::epoll, 2, 25, "epoll Inline");
    run_backend_roundtrip<nf::Queued>(nf::IoBackendKind::epoll, 2, 25, "epoll Queued");
    run_backend_roundtrip<nf::Inline>(nf::IoBackendKind::busy_poll, 2, 25, "busy_poll Inline");
    test_core_affinity_idle_strategy_and_sharded_reactor();

    if (nf::io_uring_available()) {
        run_backend_roundtrip<nf::Inline>(nf::IoBackendKind::io_uring, 1, 50, "io_uring Inline 1-session");
        run_backend_roundtrip<nf::Inline>(nf::IoBackendKind::io_uring, 4, 25, "io_uring Inline 4-session");
        run_backend_roundtrip<nf::Queued>(nf::IoBackendKind::io_uring, 2, 25, "io_uring Queued 2-session");
        test_sharded_reactor(
            nf::IoBackendConfig::io_uring(256, false, 500, nf::IdleStrategy::hybrid_backoff, 200, 50),
            "io_uring+hybrid_backoff");
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
