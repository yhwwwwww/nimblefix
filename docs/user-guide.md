# NimbleFIX User Guide

**English** | [简体中文](user-guide.zh-CN.md)

This guide walks through building FIX clients, multi-session gateways, and multi-core servers with NimbleFIX.

1. [Getting Started & Target Selection](#1-getting-started--target-selection)
2. [Encoding & Reading Messages](#2-encoding--reading-messages)
3. [Recommended Patterns by Scenario](#3-recommended-patterns-by-scenario)
4. [Threading Models](#4-threading-models)
5. [Responsibilities: Application vs. Library](#5-responsibilities-application-vs-library)
6. [TLS, Failover, Schema Codegen, Telemetry & Replay](#6-tls-failover-schema-codegen-telemetry--replay)

> Related docs: [Architecture](architecture.md) · [Support Matrix](session-support.md) · [Benchmarks](benchmarks.md)

---

## 1. Getting Started & Target Selection

### Choose a Link Target

NimbleFIX splits its runtime into three layers so you only link what your application uses:

| Use Case | Primary API | CMake / xmake Target | Extra Dependencies |
| :--- | :--- | :--- | :--- |
| Low-latency single-venue trading client (1 or 2 threads) | `DirectInitiator` (+ optional `DirectSendQueue`) | `NimbleFIX::nimblefix_core` / `nimblefix-core` | C++20 + `pthread` |
| Single-venue trading client over TLS / mTLS | `DirectInitiator<..., DirectTlsTransport>` | `NimbleFIX::nimblefix_core_tls` / `nimblefix-core-tls` | OpenSSL 3+ |
| Multi-session client/server gateway, Acceptor, or worker-isolated callbacks | `Engine<Schema, Inline\|Queued>` | `NimbleFIX::nimblefix` / `nimblefix` | Optional `liburing`, `libxml2` |
| Multi-core high-concurrency Acceptor or multi-venue gateway | `ShardedEngine<Schema, Inline\|Queued>` | `NimbleFIX::nimblefix` / `nimblefix` | Optional `liburing`, `libxml2` |

### Configuration

All components are configured through plain C++ structs (`DirectInitiatorConfig`, `EngineConfig`, `ShardedEngineConfig`, `TlsConfig`, `ConnectionStrategy`, `ReplayOptions`). Populate them directly from whatever configuration source your application uses (JSON, YAML, TOML, database, CLI flags, or environment variables).

---

## 2. Encoding & Reading Messages

Message encoding and network transport are orthogonal: the same encoding lambda works across `DirectInitiator`, `DirectSendQueue`, `Engine`, and `ShardedEngine`.

### Outbound: Streaming Encoder (`StreamWriter`)

`StreamWriter` writes strongly typed FIX fields directly into the wire buffer or mmap journal with zero heap allocations and zero intermediate message objects:

```cpp
#include <optional>
#include <nimblefix/fix44.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

const bool is_limit = true;
const std::optional<nf::Decimal> opt_px =
    is_limit ? std::optional{nf::Decimal{15025, 2}} : std::nullopt;

auto write_order = [&](auto& w) {
    // 1. Optional per-message standard header fields (must precede body fields).
    //    Session-managed tags (8, 9, 35, 49, 56, 34, 52, 43, 122, 369, 10) are set by the engine.
    //    Dynamic per-message header tags such as SenderSubID (50) or TargetSubID (57) go here:
    // w.header().sender_sub_id("DESK-A").target_sub_id("EXCH-1");

    // 2. Strongly typed body fields (native if/for and std::optional<T>):
    w.cl_ord_id("ORD-0001")
     .symbol("AAPL")
     .side('1')
     .transact_time("20261125-09:30:00.000")
     .order_qty(nf::Decimal{100, 0})
     .ord_type(is_limit ? '2' : '1')
     .price(opt_px); // automatically omitted when std::nullopt

    // 3. Repeating groups (supports runtime loops and nested groups, 0 heap allocs):
    w.no_party_i_ds(1, [](auto& g) {
        g.entry()
         .party_id("TRADER-1")
         .party_id_source('D')
         .party_role(1);
    });
};
```

> **Other outbound options**:
> - **`Fixed<ByteCap, fix::field::...>`**: Holds a reusable, in-place-mutable message template across sends (`reset()` + overwrite fields).
> - **`nf::message<"D">()` / `DynamicMessage`**: For untyped `FlatSchema` mode or runtime-determined custom tags.

### Inbound: Zero-Copy Typed Views

```cpp
// DirectInitiator handler (receives FlatMessageView):
nf::Status on_message(const nf::FlatMessageView& msg,
                      const nf::DeliveryInfo& info) noexcept {
    if (msg.is<fix::ExecutionReport>()) {
        const auto er = msg.as<fix::ExecutionReport>();
        const std::string_view exec_id = er.exec_id().value_or("");
        const auto cum_qty = er.cum_qty(); // std::optional<nf::Decimal>
        // Views borrow the parser/journal buffer for this callback only.
        // Copy any string_view values you need to retain past callback return.
        (void)exec_id; (void)cum_qty; (void)info;
    }
    return nf::Status::success();
}

// Engine / ShardedEngine typed per-message callback:
(void)engine.on<fix::ExecutionReport>(
    [](nf::SessionContext& ctx, const fix::ExecutionReportView& er,
       const nf::DeliveryInfo& info) -> nf::Status {
        const auto cl_ord_id = er.cl_ord_id();
        (void)ctx; (void)cl_ord_id; (void)info;
        return nf::Status::success();
    });
```

---

## 3. Recommended Patterns by Scenario

### Scenario A: Single-Venue Low-Latency Client — 1 Thread (`DirectInitiator`)

Best for co-located execution clients talking to one venue where minimum single-order latency (~4.7 µs 1-way / ~9 µs RTT) and smallest runtime footprint matter most.

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
            // Re-delivered from local journal after crash recovery — guard side effects!
        }
        if (msg.is<fix::ExecutionReport>()) {
            const auto er = msg.as<fix::ExecutionReport>();
            (void)er.cl_ord_id();
        }
        return nf::Status::success();
    }
};

int main() {
    (void)nf::pin_current_thread(2); // Optional: pin trading/I/O thread to core 2

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

    // Mode A1 — Lowest single-order latency: direct stream into mmap journal + ::send()
    (void)init.send_stream<fix::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("ORD-1").symbol("AAPL").side('1')
         .transact_time("20261125-09:30:00.000000")
         .order_qty(nf::Decimal{100, 0}).ord_type('1');
    });

    // Mode A2 — Same-thread burst batching (e.g., sweeping 10 levels): 1 sendmsg() syscall
    for (int i = 0; i < 10; ++i) {
        (void)init.enqueue_stream<fix::NewOrderSingle>([&](auto& w) {
            w.cl_ord_id("BURST").symbol("AAPL").side('1')
             .transact_time("20261125-09:30:00.000000")
             .order_qty(nf::Decimal{10, 0}).ord_type('1');
        });
    }
    (void)init.flush();

    std::atomic<bool> running{true};
    while (running.load(std::memory_order_relaxed)) {
        (void)init.poll_once();
        (void)init.reconnect_if_due();
        (void)init.poll_maintenance(); // pre-allocates/prunes journal segments off hot path
    }
    (void)init.logout("shutdown");
}
```

### Scenario B: Compute-Heavy Strategy + Burst Sending — 2 Threads (`DirectInitiator` + `DirectSendQueue`)

Best when your strategy computes quotes/signals (e.g., >1 µs/order) or fires dense order bursts and you want strategy compute overlapped with TCP and journal I/O on a second core (up to 1.43M ord/s peak burst).

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

// Thread 2 (Strategy Producer — sole producer for `send_queue`):
(void)nf::pin_current_thread(3);
while (!stop.load(std::memory_order_relaxed)) {
    // 0 mutex, 0 malloc: streams FIX body directly into the SPSC slot.
    // Returns ErrorCode::queue_full under backpressure.
    const nf::Status st = send_queue.try_push_stream<fix::NewOrderSingle>([](auto& w) {
        w.cl_ord_id("STRAT-1").symbol("AAPL").side('1')
         .transact_time("20261125-09:30:00.000")
         .order_qty(nf::Decimal{100, 0}).ord_type('1');
    });
    (void)st;
}
```

### Scenario C: Multi-Session Gateway or Acceptor (`Engine<Schema, Inline>` vs `Engine<Schema, Queued>`)

- **`Engine<Schema, nf::Inline>`**: Callbacks are fast and non-blocking (in-memory matching, risk check, immediate `ExecutionReport`). Lowest latency and single-threaded user state.
- **`Engine<Schema, nf::Queued>`**: Callbacks may block (database writes, downstream RPC). Offloads callbacks onto bounded worker threads so Heartbeats and TCP reads never stall.

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
    // For Queued policy:
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

    // Cross-thread send handles (obtain before engine.run()):
    auto mpsc_sender = h.sender<fix::Schema>();        // Thread-safe MPSC from any number of threads
    auto spsc_lane   = h.producer_lane<fix::Schema>(); // Lock-free SPSC for one dedicated thread
    (void)mpsc_sender; (void)spsc_lane;

    // Option 1: Blocking loop on current thread (stop from another thread with engine.request_stop()):
    return engine.run().ok() ? 0 : 1;

    // Option 2: Drive inside your own event loop on the same thread:
    // (void)engine.start();
    // while (running) (void)engine.poll_once(/*max_wait_ms=*/1);
    // (void)engine.stop();
}
```

### Scenario D: Multi-Core High-Concurrency Server (`ShardedEngine<Schema, Policy>`)

Best when serving 8–64+ active concurrent sessions on a single listening port. Shard 0 accepts incoming connections, reads `Logon (35=A)`, routes by `(SenderCompID, TargetCompID, BeginString)`, and hands the socket directly to the owning shard's pinned CPU core.

```cpp
#include <nimblefix/sharded_engine.hpp>
#include <nimblefix/fix44.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44;

int main() {
    nf::ShardedEngineConfig cfg{};
    cfg.shard_count = 4;
    cfg.cpu_cores = {2, 3, 4, 5}; // Pin Shards 0..3 to cores 2..5
    cfg.base.storage.directory = "./state/exchange";
    cfg.base.io_backend = nf::IoBackendConfig::epoll();
    cfg.base.idle_strategy = nf::IdleStrategy::hybrid_backoff;
    cfg.base.telemetry_path = "/dev/shm/exchange.nfxt"; // Shared telemetry across all 4 shards

    auto engine = nf::ShardedEngine<fix::Schema, nf::Inline>::create(cfg).value();

    // Callbacks run concurrently across different shards,
    // and strictly sequentially for any single session.
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
    for (int i = 0; i < 16; ++i) {
        (void)engine.add_acceptor({
            .name = "client-" + std::to_string(i),
            .listener = "main",
            .local_comp_id = "EXCH",
            .remote_comp_id = "BUY_" + std::to_string(i),
        });
    }

    // Spawns K-1 shard threads and runs Shard 0 on the calling thread.
    return engine.run().ok() ? 0 : 1;
}
```

### Quick Selection Matrix

| Workload | Recommended Setup | Why |
| :--- | :--- | :--- |
| 1 venue, ping-pong / single orders | `DirectInitiator::send_stream()` (1 thread) | Lowest 1-way latency (**4.69 µs**), 0 thread handoff |
| 1 venue, single thread emitting order bursts | `DirectInitiator::enqueue_stream()` + `flush()` | Coalesces burst into 1 `sendmsg()` (**1.27M ord/s**) |
| 1 venue, heavy strategy compute + bursts | `DirectInitiator` + `DirectSendQueue` (2 threads) | Overlaps compute and I/O (**1.43M ord/s**, 2× lower CO-Free latency) |
| 2–8 sessions, fast non-blocking callbacks | `Engine<Schema, Inline>` (`epoll` / `io_uring`) | Single protocol thread, lock-free user state |
| Multiple sessions, blocking DB/RPC callbacks | `Engine<Schema, Queued>` | Isolates slow callbacks from heartbeats and TCP reads |
| 8–64+ concurrent sessions | `ShardedEngine<Schema, Inline>` (`hybrid_backoff`, pinned cores) | Multi-core scaling on 1 port (**471k ord/s**, 7.8× lower p99.9) |

---

## 4. Threading Models

Every NimbleFIX runtime follows **Single-Writer Protocol Ownership**: for any FIX session, exactly one OS thread (its **Protocol Owner Thread**) mutates `SessionCore`, sequence numbers, sockets, and the mmap journal — eliminating locks on the protocol hot path.

### 4.1 `DirectInitiator` (1 Thread, No Background Threads)

```text
[ Caller / Trading Thread (Protocol Owner) ]
   ├── send_stream() / enqueue_stream()+flush() ──▶ mmap Journal ──▶ TcpTransport / TLS
   ├── poll_once() ──▶ read socket ──▶ parse/validate ──▶ mmap Journal ──▶ Handler::on_message()
   ├── reconnect_if_due() ──▶ FailoverController
   └── poll_maintenance() ──▶ pre-allocate/prune journal segments
```

- **Library-created threads**: `0`.
- **Callback execution**: `Handler::on_message()` runs synchronously inside `poll_once()` on the caller's thread.
- **Rule**: Never access the same `DirectInitiator` concurrently from two threads.

### 4.2 `DirectInitiator` + `DirectSendQueue` (2-Thread Lock-Free SPSC)

```text
[ Strategy Thread (Single Producer) ]                [ I/O Thread (Protocol Owner / Single Consumer) ]
   └── queue.try_push_stream<M>(fn)                     ├── init.drain(queue) ──▶ Journal ──▶ sendmsg()
             │                                          ├── init.poll_once()  ──▶ Handler::on_message()
             ▼  (lock-free SPSC ring, acquire/release)  └── init.poll_maintenance()
     [ DirectSendQueue ] ─────────────────────────────▶
```

- **Library-created threads**: `0` (application creates and pins both threads).
- **Callback execution**: `Handler::on_message()` runs on the **I/O Thread**. If the strategy thread needs inbound fills, forward them via your own SPSC ring or atomic state.
- **Rule**: Strictly one producer thread may push to a given `DirectSendQueue`, and strictly one consumer thread may call `init.drain(queue)`.

### 4.3 `Engine<Schema, Inline>` (1 Protocol Thread + Optional External Senders)

```text
[ External Thread(s) ]
   ├── Sender::send_stream()        ──▶ (bounded MPSC queue) ──────────┐
   ├── ProducerLane::send_stream()  ──▶ (lock-free SPSC, 1 thread) ────┼──▶ wake eventfd
   └── engine.request_stop() / snapshot()                              │
                                                                       ▼
                                             [ Protocol Owner Thread: engine.run() ]
                                                ├── IoBackend (epoll / busy_poll / io_uring)
                                                ├── All SessionCores + mmap Journals (0 locks)
                                                └── All callbacks run INLINE here
```

- **Library-created threads**: `0` (`engine.run()` executes on the calling thread).
- **Callback execution**: All callbacks (`on<M>()`, `on_message()`, `on_session_event()`, `on_logon_auth()`) run sequentially on the `engine.run()` thread.
- **Rule**: Keep `Inline` callbacks non-blocking; sleeping or blocking on DB/RPC stalls heartbeats and other sessions on the same `Engine`.

### 4.4 `Engine<Schema, Queued>` (1 Protocol Thread + N Worker Threads)

```text
[ Protocol Owner Thread: engine.run() ]          [ Worker Pool: cfg.queued.worker_threads ]
   ├── Socket I/O, parse, validate, Heartbeat       ├── Worker 1 ──┐
   ├── Persist inbound frame to mmap Journal        ├── Worker 2 ──┼─▶ on<M>(ctx, view)
   └── Enqueue to per-session serial queue ────────▶└── Worker N ──┘     └── ctx.send_stream() (queued back)
         (<= 1 active callback per session)
```

- **Library-created threads**: `cfg.queued.worker_threads` worker threads (default `2`).
- **Callback execution**:
  - **Within one session**: Strictly serialized in exact `MsgSeqNum` order (at most 1 callback in flight per session), with happens-before memory visibility between consecutive callbacks even if dispatched to different workers.
  - **Across different sessions**: Run **in parallel** on worker threads; synchronize any shared application state.
  - `on_logon_auth()` still runs on the protocol owner thread.

### 4.5 `ShardedEngine<Schema, Policy>` (K Pinned Reactor Shards)

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

- **Library-created threads**: `K - 1` shard threads (Shard 0 runs on the thread calling `engine.run()`), plus `K × worker_threads` when `Policy == Queued`.
- **Callback execution**: Each session is statically pinned to one shard (strictly sequential per session). Different sessions on different shards execute **concurrently across CPU cores** — synchronize or shard-partition any shared application state.

### 4.6 Thread-Safety Reference

| Object / Handle | Thread Safety | Rule |
| :--- | :--- | :--- |
| `DirectInitiator` | Not thread-safe | Single I/O owner thread only |
| `DirectSendQueue` | Lock-free SPSC (2 threads max) | 1 producer (`try_push*`) + 1 consumer (`init.drain`) |
| `Engine` / `ShardedEngine` setup (`on`, `add_*`) | Pre-start only | Call before `run()` / `start()` |
| `Engine::request_stop()`, `snapshot()`, `receipt()` | Thread-safe | Safe from any thread while running |
| `Sender<Schema>` (`handle.sender()`) | Thread-safe (MPSC) | Safe to share across any number of producer threads |
| `ProducerLane<Schema>` (`handle.producer_lane()`) | Lock-free SPSC (1 producer) | Bound to its first calling thread; other threads get `ErrorCode::wrong_thread` |
| `SessionContext`, `FlatMessageView`, `{Msg}View` | Callback-scoped only | Do not retain past callback return; copy strings/fields if needed |
| `TelemetryReader` | Read-only, out-of-process safe | Maps `.nfxt` `O_RDONLY`/`PROT_READ` with zero locks |
| `JournalReplayer` | Safe on live or offline journals | Uses lease-protected `JournalReader` |

---

## 5. Responsibilities: Application vs. Library

| Area | Managed by **NimbleFIX** | Managed by **Your Application** |
| :--- | :--- | :--- |
| **Configuration & Secrets** | Validates all C++ config structs; scrubs Logon passwords from persisted journal payloads. | Loading configuration from files/DB/env, rotating credentials, and populating the config structs. |
| **Event Loop & Threads** | `Engine::run()` / `ShardedEngine::run()` drive `IoBackend`, timers, shard threads, CPU pinning (`cpu_core`/`cpu_cores`), and `Queued` worker pools. | For `DirectInitiator` (or manual `Engine::start()` + `poll_once()`): calling `poll_once()`, `drain()`, `reconnect_if_due()`, and `poll_maintenance()`; spawning/pinning your own threads; calling `request_stop()` on `SIGINT`/`SIGTERM`. |
| **Callbacks & Data Lifetimes** | Per-session sequential delivery in `MsgSeqNum` order; zero-copy views valid for the callback duration. | Copying any `std::string_view` needed after callback return; synchronizing shared state across sessions (`Queued` / `ShardedEngine`); keeping `Inline` callbacks non-blocking. |
| **Crash Recovery & Idempotency** | Persists inbound frames before delivery and outbound frames before socket write; rebuilds sequences after crash/`SIGKILL`, re-delivers unfinished inbound messages (`DeliveryInfo::is_recovery == true`), and reuses committed dependent replies. | Making external side effects (order routing, DB updates, risk updates) **idempotent** when `info.is_recovery == true` (at-least-once delivery across crashes). |
| **FIX Session Protocol** | FIX 4.2/4.3/4.4/FIXT.1.1 Logon, Heartbeat, TestRequest, `MsgSeqNum`, `ResendRequest`, `GapFill`, business `Replay` (`43=Y`, `122=OrigSendingTime`), tag `369` (`LastMsgSeqNumProcessed`), Session Reject (`35=3`), graceful Logout. | Application order state machine, `BusinessMessageReject (35=j)` rules, and `on_logon_auth()` credential checks for Acceptors. |
| **TCP / TLS & Failover** | Nonblocking TCP (`TCP_NODELAY`, `sendmsg` batching), OpenSSL 3 TLS 1.2/1.3 + CA/SAN/mTLS + `TlsContext` reuse, deterministic multi-endpoint failover (`ordered` / `round_robin`, exponential backoff, retry limits). | Supplying numeric IPv4/IPv6 endpoints (performing DNS resolution if needed), maintaining valid certs/keys, and handling `FailoverController::exhausted()` if a finite `max_retries` limit is configured. |
| **Wall Clock & `SendingTime`** | Formats UTC `52=SendingTime` (`ms`/`us`/`ns`) and validates peer clock skew (`max_sending_time_skew_ms`, default 120 s). | Keeping host OS clocks synchronized via NTP/PTP. |
| **Storage & Durability** | Pre-allocated segmented `MAP_SHARED` mmap journal with SSE4.2 CRC32C, atomic segment creation, rotation, reader leases, and fail-closed capacity checks. | Provisioning disk capacity (`capacity_bytes`, `segment_bytes`), monitoring disk health, and understanding that `appended` covers **same-host process crashes** (no hot-path `fsync` for sudden host power loss). |
| **Observability & Replay** | Writes lock-free counters and 5-stage HDR histograms (`read`/`parse`/`validate`/`journal`/`write`) to `.nfxt` when `telemetry_path` is set; provides `TelemetryReader` and `JournalReplayer`. | Running sidecar scrapers / Prometheus exporters and archiving rotated journal segments for compliance retention. |

---

## 6. TLS, Failover, Schema Codegen, Telemetry & Replay

### 6.1 TLS / mTLS

- **`DirectInitiator`**: Link `NimbleFIX::nimblefix_core_tls` and pass `nf::DirectTlsTransport`:
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
- **`Engine` / `ShardedEngine`**: Configure `ListenerConfig::tls` (server) or `InitiatorSessionConfig::tls` (client).

### 6.2 Multi-Endpoint Failover (`ConnectionStrategy`)

Configured identically on `DirectInitiatorConfig` and `InitiatorSessionConfig`:

```cpp
cfg.endpoints = {
    {"10.0.1.10", 9876}, // Primary
    {"10.0.2.10", 9876}, // Standby / DR
};
// Primary-first: fail over immediately (0 ms) to standby; exponential backoff
// 250 ms -> 500 ms -> ... -> 10,000 ms after wrap-around; return to primary after recovery.
cfg.connection_strategy = nf::ConnectionStrategy::ordered(
    /*initial_ms=*/250, /*max_ms=*/10'000, /*multiplier=*/2,
    /*retries=*/0, /*reset_primary=*/true, /*immediate=*/true);
// Or round-robin: nf::ConnectionStrategy::round_robin(250, 10'000, 2);
```

### 6.3 Schema Codegen (`nimblefix-schema-gen`)

Compile standard or venue-specific FIX XML dictionaries into strongly typed C++20 headers:

```sh
# 1. Full dictionary:
nimblefix-schema-gen --output generated/fix44.hpp --namespace my::fix44 schemas/standard/FIX44.xml

# 2. Message-subset pruning (faster builds, smaller headers; admin messages & referenced groups kept):
nimblefix-schema-gen --output generated/venue.hpp --namespace my::venue \
  --messages NewOrderSingle,OrderCancelRequest,ExecutionReport schemas/standard/FIX44.xml

# 3. Log-driven pruning from recorded SOH / pipe ('|') / .nfxj logs:
nimblefix-schema-gen --output generated/venue.hpp --namespace my::venue \
  --from-fix-log ./captures/session.log schemas/standard/FIX44.xml
```

### 6.4 Out-of-Band mmap Telemetry (`TelemetryReader`)

Set `telemetry_path = "/dev/shm/app.nfxt"` and `telemetry_stage_probes = true` on `DirectInitiatorConfig` or `EngineConfig`, then read metrics from an external monitoring process without touching trading threads:

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
// Prometheus text format export:
// std::cout << reader.to_prometheus();
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

// Option 1: In-process callback sink (backtesting / reproduction)
auto stats = nimblefix::JournalReplayer::replay(opts,
    [](std::span<const char> raw_fix, const nimblefix::JournalRecordHeader& hdr) {
        (void)raw_fix; (void)hdr;
        return nimblefix::Status::success();
    }).value();

// Option 2: Stream verbatim over TCP to a staging acceptor
// auto stats = nimblefix::JournalReplayer::replay_to_tcp(opts, {"127.0.0.1", 9876}).value();
```

### 6.6 Optional `nimblefix` CLI Utility

When built with `-DNIMBLEFIX_BUILD_CLI=ON` (`--cli=y`), the standalone `nimblefix` executable provides quick operational commands:

```sh
nimblefix doctor --json
nimblefix init --directory ./demo
nimblefix validate --config ./demo/acceptor.toml --json
nimblefix run --config ./demo/acceptor.toml
nimblefix telemetry --path ./demo/telemetry.nfxt --prometheus
nimblefix replay --path ./demo/state/.../session.nfxj --direction inbound --timing original --endpoint 127.0.0.1:9876
nimblefix schema generate --xml schemas/standard/FIX44.xml --namespace venue --output venue.hpp
```
