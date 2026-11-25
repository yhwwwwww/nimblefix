# NimbleFIX 0.3 User Guide

**English** | [简体中文](user-guide.zh-CN.md)

This manual is the practical handbook for building production FIX clients, gateways, and multi-core servers with NimbleFIX 0.3. It covers:

1. [Library Philosophy & Packaging](#1-library-philosophy--packaging)
2. [Writing & Reading Messages (Zero-Alloc Streaming API)](#2-writing--reading-messages-zero-alloc-streaming-api)
3. [Scenario-Based Recommended Patterns](#3-scenario-based-recommended-patterns)
4. [Threading Models in Detail](#4-threading-models-in-detail)
5. [Responsibility Matrix: User vs. NimbleFIX](#5-responsibility-matrix-user-vs-nimblefix)
6. [Operations: TLS, Failover, Schema Codegen, Telemetry & Replay](#6-operations-tls-failover-schema-codegen-telemetry--replay)

> Related docs: [Architecture](architecture.md) · [Support Matrix](session-support.md) · [Benchmarks](benchmarks.md) · [Roadmap](roadmap.md)

---

## 1. Library Philosophy & Packaging

### Pure C++ Configuration API (Zero Config-File Format Preference)

NimbleFIX is a **library**, not an opinionated application framework. The library targets (`NimbleFIX::nimblefix_core`, `NimbleFIX::nimblefix_core_tls`, `NimbleFIX::nimblefix`) **never read configuration files** and have **no preference** for TOML, JSON, YAML, INI, SQL, or CLI flags.

Instead, every component accepts plain, self-validating C++20 structs:
- `DirectInitiatorConfig` (`<nimblefix/direct_initiator.hpp>`)
- `EngineConfig`, `ListenerConfig`, `AcceptorSessionConfig`, `InitiatorSessionConfig` (`<nimblefix/engine.hpp>`)
- `ShardedEngineConfig` (`<nimblefix/sharded_engine.hpp>`)
- `TlsConfig` (`<nimblefix/tls_config.hpp>`)
- `ConnectionStrategy` (`<nimblefix/connection_strategy.hpp>`)
- `ReplayOptions` (`<nimblefix/replay.hpp>`)

Your application owns configuration parsing, secret injection, and hot-reload policies. The standalone `nimblefix` CLI executable in `tools/nimblefix` parses TOML purely as an optional application-layer utility.

### Picking the Right Layer

| Your Use Case | Recommended Layer & Type | CMake / xmake Target | Extra Dependencies |
| :--- | :--- | :--- | :--- |
| Ultra-low-latency single-venue trading client (1 or 2 threads) | **Layer 0** `DirectInitiator` (+ optional `DirectSendQueue`) | `NimbleFIX::nimblefix_core` / `nimblefix-core` | C++20 + `pthread` only |
| Single-venue trading client over TLS / mTLS | **Layer 0 + TLS** `DirectInitiator<..., DirectTlsTransport>` | `NimbleFIX::nimblefix_core_tls` / `nimblefix-core-tls` | OpenSSL 3+ |
| Multi-session client/server gateway, Acceptor, or worker-isolated callbacks | **Layer 1/2** `Engine<Schema, Inline\|Queued>` | `NimbleFIX::nimblefix` / `nimblefix` | Optional `liburing`, `libxml2` |
| Multi-core high-concurrency Acceptor / multi-venue gateway | **Layer 1/2** `ShardedEngine<Schema, Inline\|Queued>` | `NimbleFIX::nimblefix` / `nimblefix` | Optional `liburing`, `libxml2` |

---

## 2. Writing & Reading Messages (Zero-Alloc Streaming API)

**Message construction** and **network transport** are completely orthogonal.

### Outbound: `StreamWriter` (Default, 0 Heap Allocs, ~130 ns)

`StreamWriter` streams strongly typed FIX fields directly into the destination buffer (or directly into a zero-copy mmap Journal slot) with no intermediate message object, no slot sorting, and 0 `malloc`:

```cpp
#include <optional>
#include <nimblefix/fix44.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

const bool is_limit = true;
const std::optional<nf::Decimal> opt_px =
    is_limit ? std::optional{nf::Decimal{15025, 2}} : std::nullopt;

// Works identically with DirectInitiator::send_stream, enqueue_stream,
// DirectSendQueue::try_push_stream, SessionContext::send_stream,
// Sender::send_stream, ProducerLane::send_stream, and encode_fix44_stream:
auto write_order = [&](auto& w) {
    // 1. Optional per-message dynamic standard header fields (MUST precede body fields).
    //    Engine manages 8/9/35/49/56/34/52/43/122/369/10 automatically;
    //    dynamic per-message header fields like SenderSubID (50) or TargetSubID (57) go here:
    // w.header().sender_sub_id("DESK-A").target_sub_id("EXCH-1");

    // 2. Strongly typed body fields — native C++ if/for and std::optional<T>:
    w.cl_ord_id("ORD-0001")
     .symbol("AAPL")
     .side('1')
     .transact_time("20261125-09:30:00.000")
     .order_qty(nf::Decimal{100, 0})
     .ord_type(is_limit ? '2' : '1')
     .price(opt_px); // automatically skipped when std::nullopt

    // 3. Repeating groups (supports arbitrary runtime loops & nested groups, 0 alloc):
    w.no_party_i_ds(1, [](auto& g) {
        g.entry()
         .party_id("TRADER-1")
         .party_id_source('D')
         .party_role(1);
    });
};
```

> **Alternative outbound APIs**:
> - **`Fixed<ByteCap, fix::field::...>`**: Use when you want to hold a reusable, in-place-mutable message template across sends (`reset()` + overwrite fields).
> - **`nf::message<"D">()` / `DynamicMessage`**: Use for untyped flat mode (`FlatSchema`) or runtime-determined custom tags not known at compile time.

### Inbound: Zero-Copy Typed Views (O(1) Indexed Field Access)

```cpp
// Inside DirectInitiator handler (receives FlatMessageView):
nf::Status on_message(const nf::FlatMessageView& msg,
                      const nf::DeliveryInfo& info) noexcept {
    if (msg.is<fix::ExecutionReport>()) {
        const auto er = msg.as<fix::ExecutionReport>();
        const std::string_view exec_id = er.exec_id().value_or("");
        const auto cum_qty = er.cum_qty(); // std::optional<nf::Decimal>
        // Views borrow the parser/journal buffer for this callback only!
        // Copy any strings you need to store past callback return.
        (void)exec_id; (void)cum_qty; (void)info;
    }
    return nf::Status::success();
}

// Inside Engine / ShardedEngine (typed per-message callback):
(void)engine.on<fix::ExecutionReport>(
    [](nf::SessionContext& ctx, const fix::ExecutionReportView& er,
       const nf::DeliveryInfo& info) -> nf::Status {
        const auto cl_ord_id = er.cl_ord_id();
        (void)ctx; (void)cl_ord_id; (void)info;
        return nf::Status::success();
    });
```

---

## 3. Scenario-Based Recommended Patterns

### Scenario A: Ultra-Low-Latency Single-Venue Client — 1 Thread (`DirectInitiator`)

**When to choose**: Co-located strategy or execution client talking to one venue where single-order latency (~4.7 µs 1-way / ~9 µs RTT) and minimal binary/runtime footprint matter most.

```cpp
#include <atomic>
#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/fix44.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

struct TradingHandler {
    nf::Status on_message(const nf::FlatMessageView& msg,
                          const nf::DeliveryInfo& info) noexcept {
        if (info.is_recovery) {
            // Replay from local crash recovery: check idempotency before acting!
        }
        if (msg.is<fix::ExecutionReport>()) {
            const auto er = msg.as<fix::ExecutionReport>();
            (void)er.cl_ord_id();
        }
        return nf::Status::success();
    }
};

int main() {
    (void)nf::pin_current_thread(2); // Optional: pin trading/I/O thread to isolated core 2

    nf::DirectInitiatorConfig cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = 9876;
    cfg.storage_directory = "./state/buy";
    cfg.local_comp_id = "BUY";
    cfg.remote_comp_id = "SELL";
    cfg.timestamp_precision = nf::TimestampPrecision::microseconds; // ms | us | ns

    auto init = nf::DirectInitiator<fix::Schema, TradingHandler>::open(std::move(cfg)).value();
    (void)init.connect();

    while (!init.is_active()) {
        (void)init.poll_once();
        (void)init.reconnect_if_due();
    }

    // Mode A1 — Lowest single-order latency: direct encode into mmap Journal + ::send()
    auto seq = init.send_stream<fix::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("ORD-1").symbol("AAPL").side('1')
         .transact_time("20261125-09:30:00.000000")
         .order_qty(nf::Decimal{100, 0}).ord_type('1');
    });
    (void)seq;

    // Mode A2 — Same-thread burst batching (e.g., sweeping 10 levels): 1 sendmsg() syscall
    for (int i = 0; i < 10; ++i) {
        (void)init.enqueue_stream<fix::NewOrderSingle>([&](auto& w) {
            w.cl_ord_id("BURST").symbol("AAPL").side('1')
             .transact_time("20261125-09:30:00.000000")
             .order_qty(nf::Decimal{10, 0}).ord_type('1');
        });
    }
    (void)init.flush();

    // Main loop: caller drives I/O, heartbeats, reconnects, and off-hot-path journal maintenance
    std::atomic<bool> running{true};
    while (running.load(std::memory_order_relaxed)) {
        (void)init.poll_once();
        (void)init.reconnect_if_due();
        (void)init.poll_maintenance(); // pre-allocates next journal segment off hot path
    }
    (void)init.logout("shutdown");
}
```

### Scenario B: Compute-Heavy Strategy + Burst Sending — 2 Threads (`DirectInitiator` + `DirectSendQueue`)

**When to choose**: Your strategy computes quotes/signals (e.g., >1 µs per order) or fires dense bursts, and you want zero head-of-line blocking between strategy compute and TCP/Journal I/O (1.43M ord/s peak burst, 2× lower CO-Free latency under compute load).

```cpp
#include <atomic>
#include <thread>
#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/fix44.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

// Pre-allocated cache-line-aligned lock-free SPSC ring: 512 slots x 512 bytes each.
nf::DirectSendQueue send_queue{512, 512};
std::atomic<bool> stop{false};

// Thread 1 (I/O Owner — exclusively owns `init`):
std::thread io_thread([&] {
    (void)nf::pin_current_thread(2);
    auto init = nf::DirectInitiator<fix::Schema, TradingHandler>::open(std::move(cfg)).value();
    (void)init.connect();
    while (!stop.load(std::memory_order_relaxed)) {
        (void)init.drain(send_queue); // Batch-pops up to 64 orders -> stamps seq/time -> Journal -> 1 sendmsg()
        (void)init.poll_once();       // Reads inbound fills, drives Heartbeat/TestRequest/Resend
        (void)init.reconnect_if_due();
        (void)init.poll_maintenance();
    }
    (void)init.logout("shutdown");
});

// Thread 2 (Strategy Producer — single producer for `send_queue`):
(void)nf::pin_current_thread(3);
while (!stop.load(std::memory_order_relaxed)) {
    // 0 mutex, 0 malloc: streams FIX body directly into the SPSC ring slot.
    // Returns ErrorCode::queue_full if the ring is full (backpressure).
    const nf::Status st = send_queue.try_push_stream<fix::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("STRAT-1").symbol("AAPL").side('1')
         .transact_time("20261125-09:30:00.000")
         .order_qty(nf::Decimal{100, 0}).ord_type('1');
    });
    (void)st;
}
```

### Scenario C: Multi-Session Gateway or Acceptor (`Engine<Schema, Inline>` vs `Engine<Schema, Queued>`)

**When to choose**:
- **`Engine<Schema, nf::Inline>`**: Best when callbacks are fast and non-blocking (matching engine feed, risk check in microseconds, immediate `ExecutionReport` reply). Zero extra thread hops.
- **`Engine<Schema, nf::Queued>`**: Best when business callbacks may block (database writes, downstream RPC, slow risk checks) — isolates slow callbacks on bounded worker threads so Heartbeats and TCP reads never stall.

```cpp
#include <nimblefix/engine.hpp>
#include <nimblefix/fix44.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

int main() {
    nf::EngineConfig cfg{};
    cfg.storage.directory = "./state/gateway";
    cfg.io_backend = nf::IoBackendConfig::epoll(); // ::epoll() | ::busy_poll() | ::io_uring()
    cfg.idle_strategy = nf::IdleStrategy::sleep;   // sleep | busy_spin | hybrid_backoff
    // For Queued policy only:
    // cfg.queued.worker_threads = 4;

    auto engine = nf::Engine<fix::Schema, nf::Inline>::create(cfg).value();

    (void)engine.on<fix::NewOrderSingle>(
        [](nf::SessionContext& ctx, const fix::NewOrderSingleView& req) -> nf::Status {
            return ctx.send_stream<fix::ExecutionReport>([&](auto& w) {
                w.order_id("ORD-1").exec_id("EX-1")
                 .cl_ord_id(req.cl_ord_id().value())
                 .exec_type('0').ord_status('0')
                 .symbol(req.symbol().value()).side(req.side().value())
                 .leaves_qty(req.order_qty().value())
                 .cum_qty(nf::Decimal{0, 0}).avg_px(nf::Decimal{0, 0});
            }).status();
        });

    (void)engine.add_listener({.name = "main", .endpoint = {"0.0.0.0", 9876}});
    auto h = engine.add_acceptor({
        .name = "client-a", .listener = "main",
        .local_comp_id = "EXCH", .remote_comp_id = "CLIENT_A",
    }).value();

    // Cross-thread sending options (obtain handles BEFORE engine.run()):
    auto mpsc_sender = h.sender<fix::Schema>();       // Safe from ANY number of external threads
    auto spsc_lane   = h.producer_lane<fix::Schema>(); // Lock-free SPSC for ONE dedicated producer thread
    (void)mpsc_sender; (void)spsc_lane;

    // Option 1: Blocking loop on current thread (other threads call engine.request_stop()).
    return engine.run().ok() ? 0 : 1;

    // Option 2: Embed into your own event loop on the same thread:
    // (void)engine.start();
    // while (running) (void)engine.poll_once(/*max_wait_ms=*/1);
    // (void)engine.stop();
}
```

### Scenario D: Multi-Core High-Concurrency Server (`ShardedEngine<Schema, Policy>`)

**When to choose**: Serving 8–64+ active concurrent FIX sessions (broker gateway, exchange order entry, multi-venue router) where a single reactor core saturates. All Acceptors share **one listening port**; Shard 0 accepts connections, reads Logon (`35=A`), routes by `(SenderCompID, TargetCompID, BeginString)`, and hands the socket directly to the owning shard's pinned CPU core.

```cpp
#include <nimblefix/sharded_engine.hpp>
#include <nimblefix/fix44.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

int main() {
    nf::ShardedEngineConfig cfg{};
    cfg.shard_count = 4;
    cfg.cpu_cores = {2, 3, 4, 5}; // Pin Shard 0..3 to physical cores 2..5
    cfg.base.storage.directory = "./state/exchange";
    cfg.base.io_backend = nf::IoBackendConfig::epoll();
    cfg.base.idle_strategy = nf::IdleStrategy::hybrid_backoff;
    cfg.base.telemetry_path = "/dev/shm/exchange.nfxt"; // Single shared mmap telemetry file across all 4 shards

    auto engine = nf::ShardedEngine<fix::Schema, nf::Inline>::create(cfg).value();

    // IMPORTANT: Callbacks run concurrently across DIFFERENT shards,
    // and strictly sequentially for any GIVEN session!
    (void)engine.on<fix::NewOrderSingle>(
        [](nf::SessionContext& ctx, const fix::NewOrderSingleView& req) -> nf::Status {
            return ctx.send_stream<fix::ExecutionReport>([&](auto& w) {
                w.order_id("ORD-1").exec_id("EX-1")
                 .cl_ord_id(req.cl_ord_id().value())
                 .exec_type('0').ord_status('0')
                 .symbol(req.symbol().value()).side(req.side().value())
                 .leaves_qty(req.order_qty().value())
                 .cum_qty(nf::Decimal{0, 0}).avg_px(nf::Decimal{0, 0});
            }).status();
        });

    (void)engine.add_listener({.name = "main", .endpoint = {"0.0.0.0", 9876}});
    // Auto round-robin across shards (or pass explicit shard index 0..3):
    for (int i = 0; i < 16; ++i) {
        (void)engine.add_acceptor({
            .name = "client-" + std::to_string(i),
            .listener = "main",
            .local_comp_id = "EXCH",
            .remote_comp_id = "BUY_" + std::to_string(i),
        });
    }

    // Spawns K-1 threads for Shards 1..K-1 and runs Shard 0 on the calling thread.
    // Call engine.request_stop() from any signal handler / control thread to shut down cleanly.
    return engine.run().ok() ? 0 : 1;
}
```

### Decision Matrix: Which Mode Should I Use?

| Workload Profile | Recommended Setup | Why |
| :--- | :--- | :--- |
| 1 venue, ping-pong / low-latency single orders | `DirectInitiator::send_stream()` (1 thread) | Lowest single-order 1-way latency (**4.69 µs**), 0 thread handoff, ~336 KB binary |
| 1 venue, single thread emitting bursts | `DirectInitiator::enqueue_stream()` + `flush()` | Coalesces bursts into 1 `sendmsg()` (**1.27M ord/s**, 19.8 µs CO-Free p99) |
| 1 venue, heavy strategy compute + bursts | `DirectInitiator` + `DirectSendQueue` (2 threads) | Overlaps strategy compute with I/O + journal (**1.43M ord/s**, 2× lower CO-Free latency) |
| Multiple sessions (2–8), fast non-blocking callbacks | `Engine<Schema, Inline>` (`epoll` / `io_uring`) | Single protocol thread, zero locks, simple single-threaded user state |
| Multiple sessions, blocking DB / RPC in callbacks | `Engine<Schema, Queued>` | Keeps Heartbeats & TCP reads responsive while workers handle slow callbacks |
| High-concurrency server (8–64+ sessions) | `ShardedEngine<Schema, Inline>` (`hybrid_backoff`, pinned cores) | Linear multi-core scaling on 1 port (**471k ord/s**, **7.8×** lower p99.9) |

---

## 4. Threading Models in Detail

Every NimbleFIX runtime obeys one fundamental invariant: **Single-Writer Protocol Ownership**. For any given FIX session, exactly one OS thread (its **Protocol Owner Thread**) mutates `SessionCore`, sequence numbers, TCP/TLS sockets, and the segmented mmap Journal — guaranteeing **zero mutexes on the protocol hot path**.

### Mode 1: `DirectInitiator` (Single-Threaded, Zero Hidden Threads)

```text
[ Caller / Trading Thread (Protocol Owner) ]
   ├── send_stream() / enqueue_stream()+flush()  ──▶ mmap Journal ──▶ TcpTransport/TLS
   ├── poll_once() ──▶ read socket ──▶ parse/validate ──▶ mmap Journal ──▶ Handler::on_message() (inline)
   ├── reconnect_if_due() ──▶ FailoverController
   └── poll_maintenance() ──▶ pre-allocate/prune journal segments
```

- **Threads created by NimbleFIX**: **0**.
- **Callback thread**: `Handler::on_message()` runs synchronously inside `poll_once()` on the caller's thread.
- **Rules**: Never call methods on the same `DirectInitiator` concurrently from two threads.

### Mode 2: `DirectInitiator` + `DirectSendQueue` (2-Thread Lock-Free SPSC)

```text
[ Strategy Thread (Single Producer) ]                [ I/O Thread (Protocol Owner / Single Consumer) ]
   └── queue.try_push_stream<M>(fn)                     ├── init.drain(queue) ──▶ Journal ──▶ sendmsg()
             │                                          ├── init.poll_once() ──▶ Handler::on_message()
             ▼  (lock-free SPSC ring, acquire/release)  └── init.poll_maintenance()
     [ DirectSendQueue ] ─────────────────────────────▶
```

- **Threads created by NimbleFIX**: **0** (user creates and pins both threads).
- **Callback thread**: Inbound `Handler::on_message()` runs on the **I/O Thread** (never the Strategy Thread). If your strategy needs inbound fills, pass them back from `Handler::on_message()` over your own SPSC ring or atomic state.
- **Rules**: Strictly **one** producer thread may call `try_push*()` on a given `DirectSendQueue`, and strictly **one** consumer thread may call `init.drain(queue)`.

### Mode 3: `Engine<Schema, Inline>` (1 Protocol Thread + Optional External Senders)

```text
[ External Thread(s) ]
   ├── Sender::send_stream()        ──▶ (bounded MPSC command queue) ──┐
   ├── ProducerLane::send_stream()  ──▶ (lock-free SPSC ring, 1 thr) ──┼──▶ wake eventfd
   └── engine.request_stop() / snapshot()                              │
                                                                       ▼
                                             [ Protocol Owner Thread: engine.run() ]
                                                ├── IoBackend (epoll / busy_poll / io_uring)
                                                ├── All SessionCores + mmap Journals (0 locks)
                                                └── User callbacks on<M>() run INLINE here
```

- **Threads created by NimbleFIX**: **0** (`engine.run()` runs on the calling thread).
- **Callback thread**: All registered callbacks (`on<M>()`, `on_message()`, `on_session_event()`, `on_logon_auth()`) execute **sequentially on the `engine.run()` thread**. User state touched only inside callbacks needs no mutex.
- **Rules**: Keep `Inline` callbacks fast and non-blocking (never sleep or block on slow DB/RPC inside `Inline`, or Heartbeats and other sessions on the same `Engine` will stall).

### Mode 4: `Engine<Schema, Queued>` (1 Protocol Thread + N Worker Threads)

```text
[ Protocol Owner Thread: engine.run() ]          [ Worker Pool: cfg.queued.worker_threads ]
   ├── Socket I/O, parse, validate, Heartbeat       ├── Worker 1 ──┐
   ├── Commit inbound frame to mmap Journal         ├── Worker 2 ──┼─▶ on<M>(ctx, view)
   └── Dispatch to per-session serial queue ───────▶└── Worker N ──┘     └── ctx.send_stream() (queued back to owner)
         (<= 1 active callback per session!)
```

- **Threads created by NimbleFIX**: `cfg.queued.worker_threads` worker threads (default: 2).
- **Callback ordering & concurrency**:
  - **Same session**: Strictly serialized (at most **1** callback in flight per session at any time, in exact `MsgSeqNum` order). However, consecutive callbacks for the same session may execute on *different* worker threads (with happens-before synchronization between them).
  - **Different sessions**: May execute **in parallel** across worker threads — any shared application state touched by multiple sessions must be synchronized.
- **Admin/Auth callbacks**: Protocol-critical operations (`on_logon_auth()`) still run on the protocol owner thread.

### Mode 5: `ShardedEngine<Schema, Policy>` (K Pinned Reactor Shards)

```text
                                 [ Shared Listening Port (e.g. :9876) ]
                                                   │
                                        (accept + Logon 35=A)
                                                   ▼
       ┌───────────────────────────────────[ Shard 0 (Calling Thread, Core c0) ]
       │         (socket handoff via fd queue)     ├── Sessions pinned to Shard 0
       ├───────────────────────────┐               └── (optional Queued pool 0)
       ▼                           ▼
[ Shard 1 (Thread 1, Core c1) ]  [ Shard K-1 (Thread K-1, Core cK-1) ]
  ├── Sessions pinned to Shard 1   ├── Sessions pinned to Shard K-1
  └── (optional Queued pool 1)     └── (optional Queued pool K-1)
```

- **Threads created by NimbleFIX**: `K - 1` shard reactor threads (Shard 0 runs on the calling thread of `engine.run()`), plus `K × worker_threads` if `Policy == Queued`.
- **Callback concurrency**:
  - Every session is statically pinned to one shard — callbacks for a given session run strictly sequentially on its owning shard.
  - Callbacks for **different sessions on different shards run concurrently in parallel** on different CPU cores! Any shared application state (order book, global risk counter) touched across shards MUST be thread-safe (synchronized, atomic, or shard-partitioned/`thread_local`).

### Object & Handle Thread-Safety Summary

| Object / Handle | Thread Safety | Usage Rule |
| :--- | :--- | :--- |
| `DirectInitiator` | **Not thread-safe** (single owner) | Call all methods from one I/O thread only |
| `DirectSendQueue` | **Lock-free SPSC** (2 threads max) | Exactly 1 producer thread (`try_push*`) + 1 consumer thread (`init.drain`) |
| `Engine` / `ShardedEngine` setup (`on`, `add_*`) | **Before `run()`/`start()` only** | Configure completely before starting the event loop |
| `Engine::request_stop()`, `snapshot()`, `receipt()` | **Thread-safe** | Safe to call from any control/monitoring thread while running |
| `Sender<Schema>` (`handle.sender()`) | **Thread-safe (MPSC)** | Safe to clone/share across any number of producer threads |
| `ProducerLane<Schema>` (`handle.producer_lane()`) | **Lock-free SPSC** (1 producer thread) | Bound to its first calling thread; calling from a second thread returns `ErrorCode::wrong_thread` |
| `SessionContext` & `FlatMessageView` / `{Msg}View` | **Callback-scoped only** | Never store pointers/views or `SessionContext&` beyond callback return; copy strings/fields if retaining |
| `TelemetryReader` | **Read-only, out-of-process safe** | Maps `.nfxt` `O_RDONLY`/`PROT_READ`; zero locks or interference with trading threads |
| `JournalReplayer` | **Safe on live or offline journals** | Uses lease-protected `JournalReader` (`create_reader()`) — safe alongside active writer |

---

## 5. Responsibility Matrix: User vs. NimbleFIX

Understanding the exact boundary between what your application owns and what NimbleFIX guarantees prevents subtle production bugs:

| Domain | What **NimbleFIX** Manages Automatically | What the **User / Application** Must Manage |
| :--- | :--- | :--- |
| **Configuration & Secrets** | Validates all C++ config structs (`EngineConfig`, `DirectInitiatorConfig`, `TlsConfig`, `ConnectionStrategy`) and scrubs Logon passwords from persisted journal payloads. | Loading config from your chosen format (JSON/YAML/TOML/DB/env), managing secret rotation, and populating the C++ structs. |
| **Event Loop & Threads** | `Engine::run()` / `ShardedEngine::run()` drive `IoBackend`, timers, shard threads, CPU pinning (when `cpu_core`/`cpu_cores` set), and `Queued` worker pools. | In `DirectInitiator` (or manual `Engine::start()` + `poll_once()`): driving `poll_once()`, `drain()`, `reconnect_if_due()`, and `poll_maintenance()` continuously; pinning your own strategy/I/O threads; calling `request_stop()` on `SIGINT`/`SIGTERM`. |
| **Callback Safety & Lifetimes** | Guarantees per-session sequential delivery in exact `MsgSeqNum` order and zero-copy views valid for the callback duration. | Copying any `std::string_view` / view data needed after callback return; synchronizing shared app state across sessions (in `Queued` or `ShardedEngine`); keeping `Inline` callbacks non-blocking. |
| **Crash Recovery & Idempotency** | Persists inbound frames before delivery and outbound frames before socket write; on restart after crash/`SIGKILL`, rebuilds sequences, re-delivers unfinished inbound messages with `DeliveryInfo::is_recovery == true`, and reuses committed dependent replies. | Making external side effects (order routing, DB updates, risk deductions) **idempotent** when `info.is_recovery == true` (delivery is at-least-once across process crashes). |
| **FIX Session Protocol** | Full FIX 4.2/4.3/4.4/FIXT.1.1 state machine: Logon, Heartbeat, TestRequest, `MsgSeqNum` tracking, `ResendRequest`, `GapFill`, business `Replay` (`43=Y`, `122=OrigSendingTime`), tag `369` (`LastMsgSeqNumProcessed`), Session Reject (`35=3`), graceful Logout. | Application-level business logic, order state machine, `BusinessMessageReject (35=j)` policy, and checking `on_logon_auth()` credentials if acting as an Acceptor. |
| **TCP / TLS & Failover** | Nonblocking TCP (`TCP_NODELAY`, `sendmsg` batching), OpenSSL 3 TLS 1.2/1.3 + CA/SAN/mTLS + `TlsContext` reuse, and deterministic multi-endpoint failover (`ordered` / `round_robin`, exponential backoff, retry limits). | Providing numeric IPv4/IPv6 endpoints (DNS resolution, if needed, is done by the app), valid CA/certificate/key files, and handling `FailoverController::exhausted()` if a finite `max_retries` cap is configured. |
| **Wall Clock & `SendingTime`** | Formats UTC `52=SendingTime` (`ms`/`us`/`ns`) via cached `FastUtcClock` and validates peer clock skew (`max_sending_time_skew_ms`, default 120 s). | Keeping host OS clocks synchronized via NTP/PTP across trading hosts. |
| **Storage & Durability** | Pre-allocated segmented `MAP_SHARED` mmap Journal with hardware SSE4.2 CRC32C, atomic `.tmp` + `rename` segment creation, rotation, reader leases, and fail-closed capacity protection. | Provisioning sufficient disk/filesystem space (`capacity_bytes`, `segment_bytes`), monitoring disk health, and recognizing that `appended` covers **same-host process crashes** (no hot-path `fsync` for sudden host power loss). |
| **Observability & Replay** | Writes lock-free counters and 5-stage HDR histograms (`read`/`parse`/`validate`/`journal`/`write`) to `.nfxt` when `telemetry_path` is set; provides `TelemetryReader` & `JournalReplayer`. | Deploying your sidecar scraper / Prometheus exporter (`TelemetryReader` or `nimblefix telemetry`) and archiving rotated journal segments if long-term compliance retention is required. |

---

## 6. Operations: TLS, Failover, Schema Codegen, Telemetry & Replay

### 6.1 TLS / mTLS

- **Layer 0 (`DirectInitiator`)**: Link `NimbleFIX::nimblefix_core_tls` and pass `nf::DirectTlsTransport`:
  ```cpp
  nf::TlsConfig tls{};
  tls.enabled = true;
  tls.ca_file = "/etc/certs/ca.pem";
  tls.server_name = "fix.venue.example";
  tls.minimum_version = 13; // TLS 1.3 (or 12 for TLS 1.2+)
  // Optional mTLS:
  // tls.certificate_file = "/etc/certs/client.pem";
  // tls.private_key_file = "/etc/certs/client.key";

  auto transport = nf::DirectTlsTransport::create(std::move(tls)).value();
  auto init = nf::DirectInitiator<fix::Schema, TradingHandler, nf::DirectTlsTransport>::open(
      std::move(cfg), TradingHandler{}, std::move(transport)).value();
  ```
- **Layer 1/2 (`Engine` / `ShardedEngine`)**: Set `ListenerConfig::tls` (server) or `InitiatorSessionConfig::tls` (client).

### 6.2 Multi-Endpoint Failover (`ConnectionStrategy`)

Supported identically on `DirectInitiatorConfig` and `InitiatorSessionConfig`:

```cpp
cfg.endpoints = {
    {"10.0.1.10", 9876}, // Primary
    {"10.0.2.10", 9876}, // DR / Standby
};
// Primary-first: immediately fail over (0 ms) to standby on failure; exponential backoff
// 250 ms -> 500 ms -> ... -> 10,000 ms after wrapping around; return to primary after recovery.
cfg.connection_strategy = nf::ConnectionStrategy::ordered(
    /*initial_ms=*/250, /*max_ms=*/10'000, /*multiplier=*/2,
    /*retries=*/0, /*reset_primary=*/true, /*immediate=*/true);
// Or round-robin: nf::ConnectionStrategy::round_robin(250, 10'000, 2);
```

### 6.3 Pure C++20 Schema Codegen (`nimblefix-schema-gen`)

```sh
# 1. Full FIX 4.4 header:
nimblefix-schema-gen --output generated/fix44.hpp --namespace my::fix44 schemas/standard/FIX44.xml

# 2. Message-subset pruning (faster builds, smaller binaries):
nimblefix-schema-gen --output generated/venue.hpp --namespace my::venue \
  --messages NewOrderSingle,OrderCancelRequest,ExecutionReport schemas/standard/FIX44.xml

# 3. Log-driven pruning from real SOH / pipe ('|') / .nfxj journal files:
nimblefix-schema-gen --output generated/venue.hpp --namespace my::venue \
  --from-fix-log ./captures/session.log schemas/standard/FIX44.xml
```

### 6.4 Out-of-Band mmap Telemetry (`TelemetryReader`)

Enable in `DirectInitiatorConfig` or `EngineConfig` (`telemetry_path = "/dev/shm/app.nfxt"`, `telemetry_stage_probes = true`), then inspect from a separate monitoring process with zero trading-path overhead:

```cpp
#include <iostream>
#include <nimblefix/telemetry.hpp>

auto reader = nimblefix::TelemetryReader::open("/dev/shm/app.nfxt").value();
for (const auto& s : reader.snapshot()) {
    const auto parse_lat = s.stage_latency(nimblefix::TelemetryStage::parse);
    std::cout << s.session_key << " rx=" << s.rx_messages
              << " tx=" << s.tx_messages
              << " parse_p99_ns=" << parse_lat.p99_ns << '\n';
}
// Or export Prometheus metrics directly: std::cout << reader.to_prometheus();
```

### 6.5 Deterministic Historical Replay (`JournalReplayer`)

```cpp
#include <nimblefix/replay.hpp>

nimblefix::ReplayOptions opts{};
opts.journal_path = "./state/buy/FIX.4.4_BUY_SELL.nfxj";
opts.direction = nimblefix::ReplayDirection::inbound;
opts.timing = nimblefix::ReplayTiming::scaled;
opts.speed_multiplier = 10.0; // 10x faster than original 52=SendingTime spacing
opts.skip_admin = true;

// Option 1: In-process callback sink (backtesting / debugging)
auto stats = nimblefix::JournalReplayer::replay(opts,
    [](std::span<const char> raw_fix, const nimblefix::JournalRecordHeader& hdr) {
        (void)raw_fix; (void)hdr;
        return nimblefix::Status::success();
    }).value();

// Option 2: Stream verbatim over TCP to a staging acceptor
// auto stats = nimblefix::JournalReplayer::replay_to_tcp(opts, {"127.0.0.1", 9876}).value();
```

### 6.6 Optional Standalone CLI (`nimblefix`)

When built with `-DNIMBLEFIX_BUILD_CLI=ON` (or `--cli=y`), the optional `nimblefix` executable demonstrates application-layer TOML configuration and provides operational subcommands:

```sh
nimblefix doctor --json
nimblefix init --directory ./demo
nimblefix validate --config ./demo/acceptor.toml --json
nimblefix run --config ./demo/acceptor.toml
nimblefix telemetry --path ./demo/telemetry.nfxt --prometheus
nimblefix replay --path ./demo/state/.../session.nfxj --direction inbound --timing original --endpoint 127.0.0.1:9876
nimblefix schema generate --xml schemas/standard/FIX44.xml --namespace venue --output venue.hpp
```
