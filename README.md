# NimbleFIX 0.3

**English** | [简体中文](README.zh-CN.md)

A zero-dependency, C++20 FIX engine designed around **pay only for what you need**: start with a zero-overhead Layer 0 **`DirectInitiator`** (`NimbleFIX::nimblefix_core` — 0 `malloc` / 0 `mutex` / 0 `fsync` steady state, full restart recovery & GapFill/Replay), opt into Layer 0 OpenSSL 3 TLS (`NimbleFIX::nimblefix_core_tls`), or scale up to the full Layer 1/2 multi-session **`Engine`** and multi-core **`ShardedEngine`** (`NimbleFIX::nimblefix`) with pluggable **`IoBackend`** (`epoll`, `busy_poll`, optional Linux `io_uring`), Acceptor, `Inline`/`Queued` dispatch, MPSC/SPSC senders, out-of-band mmap telemetry, and deterministic replay.

- **Pure C++ Library — Zero Config-File Format Preference**: The library exposes strongly typed C++ configuration structs only (`DirectInitiatorConfig`, `EngineConfig`, `ShardedEngineConfig`, `TlsConfig`, `ConnectionStrategy`, ...). Your application decides how to load configuration (JSON, YAML, TOML, database, CLI flags, environment variables). The optional standalone `nimblefix` CLI app (`tools/nimblefix`) parses TOML purely as an application-layer example.
- **100% Pure C++20 Toolchain — Zero Python Dependency**: The schema compiler (`nimblefix-schema-gen`), all integration/contract/install tests, and benchmark harnesses are native C++20 executables. Building, testing, and installing NimbleFIX requires **zero Python**.

[User Guide (EN)](docs/user-guide.md) · [使用手册 (中文)](docs/user-guide.zh-CN.md) · [Architecture](docs/architecture.md) · [Support Matrix](docs/session-support.md) · [Benchmarks](docs/benchmarks.md) · [Roadmap](docs/roadmap.md)

---

## 3-Layer Modular Architecture

| Layer | CMake Target / xmake Target | Included Capabilities | Extra Dependencies |
| :--- | :--- | :--- | :--- |
| **Layer 0 (Core)** | `NimbleFIX::nimblefix_core` / `nimblefix-core` | Zero-intermediate-copy **Streaming Encoder (`StreamWriter`)**, `Fixed<...>`, compiled Schemas (`nimblefix-schema-gen`), segmented mmap Journal (SSE4.2 CRC32C, `reserve()`/`commit()`), `SessionCore`, nonblocking `TcpTransport` (`write_batch`/`sendmsg`), **`DirectInitiator`**, multi-endpoint **`ConnectionStrategy`** failover, lock-free SPSC **`DirectSendQueue`**, mmap **`TelemetryWriter`/`TelemetryReader`**, **`JournalReplayer`** | C++20 + `pthread` only |
| **Layer 0 + TLS** | `NimbleFIX::nimblefix_core_tls` / `nimblefix-core-tls` | Everything in Layer 0 + nonblocking OpenSSL 3 **`DirectTlsTransport`** (TLS 1.2/1.3, CA/SAN verification, mTLS, `TlsContext` reuse on reconnect) | OpenSSL 3+ (optional) |
| **Layer 1 / 2 (Full)** | `NimbleFIX::nimblefix` / `nimblefix` | Everything above + `SessionDriver`, pluggable `IoBackend` (`epoll` / `busy_poll` / optional `io_uring`), multi-session **`Engine<Schema, Inline\|Queued>`**, multi-core **`ShardedEngine`** (single-port Logon handoff, CPU core pinning, `IdleStrategy`), Acceptor, MPSC `Sender`, SPSC `ProducerLane`, optional runtime libxml2 XML (`NimbleFIX::nimblefix_xml`) | Optional `liburing`, `libxml2` |

*(Optional standalone application: `nimblefix` CLI in `tools/nimblefix` — not linked into library targets.)*

---

## Quickstart: Layer 0 `DirectInitiator`

See [examples/direct_initiator.cpp](examples/direct_initiator.cpp) for a runnable example and the [User Guide](docs/user-guide.md) for complete scenario patterns, threading models, and responsibility boundaries.

```cpp
#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/fix44_sample.hpp>

namespace nf  = nimblefix;
namespace fix = nimblefix::fix44_sample;

struct MyHandler {
    [[nodiscard]] nf::Status on_message(const nf::FlatMessageView& msg,
                                        const nf::DeliveryInfo& info) noexcept {
        if (msg.is<fix::ExecutionReport>()) {
            const auto er = msg.as<fix::ExecutionReport>();
            (void)er.exec_id();
        }
        (void)info; // info.is_recovery == true for crash-recovered deliveries
        return nf::Status::success();
    }
};

// Pure C++ configuration struct — populate from your app's own config source.
auto init = nf::DirectInitiator<fix::Schema, MyHandler>::open({
    .host = "127.0.0.1",
    .port = 9876,
    .storage_directory = "./state/buy",
    .local_comp_id = "BUY",
    .remote_comp_id = "SELL",
}).value();
(void)init.connect();
while (!init.is_active()) (void)init.poll_once();

// Zero-intermediate-copy streaming directly into the mmap Journal (0 heap allocs):
const bool is_limit = true;
const std::optional<nf::Decimal> opt_px = nf::Decimal{15025, 2};
auto write_order = [&](auto& w) {
    // Optional per-message dynamic standard header fields (must precede body fields):
    // w.header().sender_sub_id("DESK-A").target_sub_id("EXCH-1");
    w.cl_ord_id("ORD-001").symbol("AAPL").side('1')
     .transact_time("20261123-00:00:00.000")
     .order_qty(nf::Decimal{100, 0})
     .ord_type(is_limit ? '2' : '1')
     .price(opt_px); // std::nullopt skipped automatically
    // Repeating groups (supports nested groups & runtime loops, 0 alloc):
    // w.no_party_i_ds(1, [](auto& g) {
    //     g.entry().party_id("CLIENT1").party_id_source('D').party_role(1);
    // });
};

// 3 composable send modes on DirectInitiator:
(void)init.send_stream<fix::NewOrderSingle>(write_order);      // 1) Direct immediate send (lowest single-order latency)
// (void)init.enqueue_stream<fix::NewOrderSingle>(write_order);// 2) Same-thread burst batching -> 1 sendmsg/writev
// (void)init.flush();
// nf::DirectSendQueue q{256, 512};                            // 3) 2-thread lock-free SPSC:
// (void)q.try_push_stream<fix::NewOrderSingle>(write_order);  //    strategy streams body into SPSC ring,
// (void)init.drain(q);                                        //    I/O thread stamps seq/time -> Journal + 1 sendmsg
```

### Native C++20 Schema Compiler (`nimblefix-schema-gen`)

Generate strongly typed, zero-allocation C++20 headers from any standard or custom FIX XML dictionary — zero Python required:

```sh
# Full dictionary:
./build-cmake/nimblefix-schema-gen --output generated/venue.hpp --namespace venue \
  schemas/standard/FIX44.xml

# Explicit business message subset (admin messages & referenced groups kept automatically):
./build-cmake/nimblefix-schema-gen --output generated/venue.hpp --namespace venue \
  --messages NewOrderSingle,OrderCancelRequest,ExecutionReport schemas/standard/FIX44.xml

# Or auto-prune unused messages & optional fields directly from sample FIX logs (.log / .fix / .nfxj):
./build-cmake/nimblefix-schema-gen --output generated/venue.hpp --namespace venue \
  --from-fix-log ./logs/venue.log schemas/standard/FIX44.xml
```

CMake (`nimblefix_generate_schema(...)`) and xmake (`add_rules("nimblefix.schema")`) both invoke the installed `nimblefix-schema-gen` executable automatically with full incremental rebuild tracking.

---

## Performance Summary (Release, AMD Ryzen 7 7840HS, GCC 16.2.1, Linux 7.2)

Full methodology, boundary disclosures, and reproducible commands are in [docs/benchmarks.md](docs/benchmarks.md). All hot-path numbers run with real **segmented mmap Journal + SSE4.2 hardware CRC32C** persistence and `-Wl,--wrap=malloc` verifying **0 heap allocations**.

### 1. `DirectInitiator` Layered Breakdown (`direct-initiator-bench`, full 912-field FIX 4.4 + `NoPartyIDs[3]` + mmap Journal)

| Workload Boundary | p50 | p95 | p99 | Throughput | Heap Allocs |
| :--- | ---: | ---: | ---: | ---: | :---: |
| **User Encode (`StreamWriter`)** (`encode_fix44_stream`) | **140.0 ns** | 150.0 ns | **151.0 ns** | **7,089,686 msgs/s** | **0** |
| **Pre-TCP Session Outbound** (`DirectInitiator::send_stream` + mmap Journal) | **211.0 ns** | 280.0 ns | **421.0 ns** | **4,420,157 msgs/s** | **0** |
| **Pre-TCP Session Inbound** (frame + parse + 912-field Schema + dual Journal + callback) | **721.0 ns** | 822.0 ns | **1,062.0 ns** | **1,392,046 msgs/s** | **0** |
| **Pre-TCP Combined** (total pure FIX engine round-trip budget) | **942.0 ns** | 1,091.0 ns | **1,442.0 ns** | **1,058,645 rt/s** | **0** |
| **Real TCP Loopback RTT** (kernel TCP + full FIX 4.4 + mmap Journal) | **9.67 µs** | 10.67 µs | 13.28 µs | 103,837 rt/s | **0** |

### 2. Initiator Send Modes (`initiator-burst-bench`, real loopback TCP + FIX 4.4 + mmap Journal)

| Initiator Send Mode | Ping-Pong 1-Way p50 (RTT p50) | Burst 32×0 µs CO-Free p50 / p99 (Rate) | Burst 32×2 µs Compute CO-Free p50 / p99 (Rate) |
| :--- | ---: | ---: | ---: |
| **1. `DirectInitiator::send_stream()`** (1-thr, no queue) | **4.69 µs** (**8.86 µs**) | 49.93 µs / 98.19 µs (335k ord/s) | 86.87 µs / 228.77 µs (179k ord/s) |
| **2. `DirectInitiator::enqueue_stream()+flush()`** (1-thr `sendmsg`) | 5.42 µs (10.39 µs) | 14.58 µs / **19.83 µs** (1.27M ord/s) | 80.85 µs / 217.01 µs (320k ord/s) |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`** (2-thr SPSC) | 5.27 µs (9.09 µs) | **12.86 µs** / 22.64 µs (**1.43M ord/s**) | **42.62 µs** / **76.08 µs** (**422k ord/s**) |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`** (2-thr SPSC) | 7.69 µs (12.27 µs) | 67.28 µs / 117.28 µs (278k ord/s) | 64.77 µs / 118.97 µs (280k ord/s) |

### 3. Multi-Core Sharded Reactor Scaling (`acceptor-bench`, 1 shared port, full 912-field FIX 4.4 + mmap Journal)

| Configuration | Sessions | p50 | p99 | p99.9 | Aggregate Rate | Speedup vs 1-Core |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **1-core `Engine` (`epoll`)** | 8 | 32.99 µs | 76.91 µs | 227.25 µs | 176,387 ord/s | 1.00× |
| **4-shard `ShardedEngine` (`hybrid_backoff`, pinned)** | 8 | **16.39 µs** | **21.27 µs** | **28.98 µs** | **443,302 ord/s** | **2.51×** (**7.84×** lower p99.9) |
| **1-core `Engine` (`epoll`)** | 16 | 75.64 µs | 161.11 µs | 346.42 µs | 176,184 ord/s | 1.00× |
| **4-shard `ShardedEngine` (`hybrid_backoff`, pinned)** | 16 | **31.19 µs** | **49.99 µs** | **96.74 µs** | **470,973 ord/s** | **2.67×** (**3.22×** lower p99) |

### 4. NimbleFIX 0.3 vs QuickFIX 1.16.0 (`quickfix-parity-bench`, Same-Methodology)

| Suite (`1000` warmup / `5000` samples) | NimbleFIX 0.3 (p50 / p99 / alloc) | QuickFIX 1.16.0 (p50 / p99 / alloc) | Speedup (p50) |
| :--- | :--- | :--- | ---: |
| **Codec Encode** (`StreamWriter` / `Fixed`) | **170 ns** / **371 ns** (**0.00** alloc) | 1,463 ns / 1,523 ns / 19.00 | **8.61×** / **3.94×** |
| **Parse + Full `FIX44.xml` Strict + Read** | **421 ns** / **431 ns** / **0.00** | 2,795 ns / 3,386 ns / 15.00 | **6.64×** |
| **TCP Initiator RTT** (`DirectInitiator` / `Engine busy_poll`) | **10.21 µs** / **14.39 µs** (**0.00** alloc) | 23.72 µs / p99 34.56 µs / 35.06 | **2.32×** / 1.65× |
| **TCP Acceptor RTT** (`Engine busy_poll` / `epoll`) | **10.59 µs** / **11.57 µs** (**0.00** alloc) | 23.20 µs / p99 33.92 µs / 37.00 | **2.19×** / 2.01× |
| **TCP Dense Burst 32×** (`enqueue_stream` + `sendmsg`, CO-Free 1-way) | **12.63 µs** / **23.51 µs** / **0.00** (**1.34M/s**) | 198.47 µs / 399.32 µs / 28.06 (80k/s) | **15.71×** (**16.6×** rate) |

---

## Build, Test & Install (Zero Python)

Clone with submodules (`third_party/tomlplusplus` for the optional CLI app and `third_party/quickfix` for optional oracle tests/parity benchmarks):

```sh
git clone --recurse-submodules <repo-url>
# Or in an existing clone:
git submodule update --init --recursive
```

### xmake

```sh
xmake f -m release --tls=y --xml=y --benchmarks=y --quickfix=y -y
xmake check

# Optional Linux io_uring Proactor support:
xmake f -m release --tls=y --xml=y --io_uring=y -y
xmake build acceptor-example initiator-example direct-initiator-example nimblefix-cli
```

### CMake

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release \
  -DNIMBLEFIX_ENABLE_TLS=ON -DNIMBLEFIX_ENABLE_XML=ON \
  -DNIMBLEFIX_BUILD_BENCHMARKS=ON -DNIMBLEFIX_ENABLE_QUICKFIX=ON
cmake --build build-cmake -j
ctest --test-dir build-cmake --output-on-failure

# Relocatable install verification (pure C++ binary, zero Python):
./build-cmake/install-test --root . --features --examples \
  --endpoint-test ./build-cmake/endpoint-test \
  --xmake-schema-test ./build-cmake/xmake_schema-test
./build-cmake/install-test --root . --core-only --examples \
  --endpoint-test ./build-cmake/endpoint-test \
  --xmake-schema-test ./build-cmake/xmake_schema-test
```

### Consuming via Installed CMake Package

```cmake
find_package(NimbleFIX 0.3 CONFIG REQUIRED COMPONENTS core)

# Layer 0 ultra-low-latency initiator only (C++20 + pthread):
target_link_libraries(my_trader PRIVATE NimbleFIX::nimblefix_core)
# Layer 0 + OpenSSL 3 TLS:
# target_link_libraries(my_trader PRIVATE NimbleFIX::nimblefix_core_tls)
# Layer 1/2 full Engine, ShardedEngine, Acceptor:
# target_link_libraries(my_gateway PRIVATE NimbleFIX::nimblefix)

# Optional compile-time XML -> C++20 schema codegen (invokes NimbleFIX::nimblefix-schema-gen):
nimblefix_generate_schema(TARGET my_venue_schema NAMESPACE my::venue
    HEADER venue.hpp XML_FILES "${CMAKE_CURRENT_SOURCE_DIR}/venue.xml")
target_link_libraries(my_trader PRIVATE my_venue_schema)
```

Dependencies and acknowledgements are documented in [third_party/README.md](third_party/README.md).
