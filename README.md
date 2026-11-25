# NimbleFIX 0.3

A C++20 FIX engine designed around **pay only for what you need**: start with a zero-overhead Layer 0 **DirectInitiator** (`NimbleFIX::nimblefix_core`, 0 malloc / 0 mutex / 0 fsync steady state, full restart recovery & GapFill/Replay), opt into Layer 0 OpenSSL 3 TLS (`NimbleFIX::nimblefix_core_tls`), or scale up to the full Layer 1/2 multi-session **Engine** (`NimbleFIX::nimblefix`) with pluggable **`IoBackend`** (`epoll`, `busy_poll`, optional Linux `io_uring` Proactor), Acceptor, `Inline`/`Queued` dispatch, MPSC/SPSC senders, runtime XML, and TOML CLI.

[中文入门](docs/getting-started.zh-CN.md) · [Architecture](docs/architecture.md) · [Support Matrix](docs/session-support.md) · [Benchmarks](docs/benchmarks.md) · [Roadmap](docs/roadmap.md)

---

## Architecture: 3-Layer Pay-Only-For-What-You-Need

| Layer | CMake Target / xmake Target | Included Capabilities | Extra Dependencies |
| :--- | :--- | :--- | :--- |
| **Layer 0 (Core)** | `NimbleFIX::nimblefix_core` / `nimblefix-core` | Zero-intermediate-copy **Streaming Encoder (`StreamWriter`)**, `Fixed<...>`, generated Schemas (`--messages` subset codegen), segmented mmap Journal (hardware SSE4.2 CRC32C, `reserve()`/`commit()`), `SessionCore`, nonblocking `TcpTransport` (`write_batch`/`sendmsg`), **`DirectInitiator`**, lock-free SPSC **`DirectSendQueue`** | C++20 + `pthread` only |
| **Layer 0 + TLS** | `NimbleFIX::nimblefix_core_tls` / `nimblefix-core-tls` | Everything in Layer 0 + nonblocking OpenSSL 3 **`DirectTlsTransport`** (TLS 1.2/1.3, CA/hostname/IP verification, mTLS, `TlsContext` reuse on reconnect) | OpenSSL 3+ (optional) |
| **Layer 1 / 2 (Full)** | `NimbleFIX::nimblefix` / `nimblefix` | Everything above + `SessionDriver`, pluggable `IoBackend` (`epoll` / `busy_poll` / optional `io_uring` Proactor), multi-session `Engine<Schema, Inline\|Queued>`, Acceptor, MPSC `Sender`, SPSC `ProducerLane`, optional runtime libxml2 XML & TOML CLI | Optional `liburing`, `libxml2` |

---

## Quickstart: Layer 0 `DirectInitiator` (Zero-Overhead Initiator)

See [examples/direct_initiator.cpp](examples/direct_initiator.cpp) for a complete runnable initiator with zero-intermediate-copy streaming encoding, segmented mmap Journal persistence, automatic restart recovery, and GapFill/Replay:

```cpp
#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/fix44_sample.hpp>

namespace nf = nimblefix;
namespace fix = nimblefix::fix44_sample;

struct MyHandler {
    [[nodiscard]] nf::Status on_message(const nf::FlatMessageView& msg,
                                        const nf::DeliveryInfo& info) noexcept {
        // Typed zero-copy view — no magic tag numbers required.
        if (msg.is<fix::ExecutionReport>()) {
            const auto er = msg.as<fix::ExecutionReport>();
            (void)er.exec_id();
        }
        (void)info; // info.is_recovery == true for crash-recovered deliveries
        return nf::Status::success();
    }
};

auto init = nf::DirectInitiator<fix::Schema, MyHandler>::open({
    .host = "127.0.0.1",
    .port = 9876,
    .storage_directory = "./state/buy",
    .local_comp_id = "BUY",
    .remote_comp_id = "SELL",
}).value();
(void)init.connect();
while (!init.is_active()) (void)init.poll_once();

// Zero-intermediate-copy streaming directly into the mmap Journal (0 heap allocs, native if/for,
// std::optional support, dynamic per-message standard header via w.header(), nested repeating groups):
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
    // Repeating groups (supports nested groups and dynamic runtime loops, 0 alloc):
    // w.no_party_i_ds(1, [](auto& g) {
    //     g.entry().party_id("CLIENT1").party_id_source('D').party_role(1);
    // });
};

// 3 send modes on DirectInitiator:
(void)init.send_stream<fix::NewOrderSingle>(write_order);    // 1) Direct immediate send (lowest single-order latency)
// (void)init.enqueue_stream<fix::NewOrderSingle>(write_order); // 2) Same-thread burst batching -> 1 writev/sendmsg
// (void)init.flush();
// nf::DirectSendQueue q{256, 512};                          // 3) 2-thread lock-free SPSC:
// (void)q.try_push_stream<fix::NewOrderSingle>(write_order);//    strategy streams body into SPSC slot,
// (void)init.drain(q);                                      //    I/O thread stamps seq/time -> Journal + 1 sendmsg
```

### Enabling FIX over TLS (`DirectTlsTransport`)

Link `NimbleFIX::nimblefix_core_tls` (`nimblefix-core-tls` in xmake) and swap the `Transport` template argument — zero Engine/Acceptor overhead:

```cpp
#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/direct_tls_transport.hpp>

nf::TlsConfig tls{};
tls.enabled = true;
tls.ca_file = "/path/to/ca.pem";
tls.server_name = "fix.venue.example";
tls.minimum_version = 13; // TLS 1.3 (or 12 for TLS 1.2+)
// Optional mTLS:
// tls.certificate_file = "/path/to/client.pem";
// tls.private_key_file = "/path/to/client.key";

auto transport = nf::DirectTlsTransport::create(std::move(tls)).value();
auto init = nf::DirectInitiator<fix::Schema, MyHandler, nf::DirectTlsTransport>::open(
    std::move(cfg), MyHandler{}, std::move(transport)).value();
```

### Subset & Log-Driven Schema Codegen (`--messages` / `--from-fix-log`)

Trim generated headers and compile times to only the business messages you trade (admin messages and referenced groups/components are kept automatically):

```sh
# Explicit message subset:
python3 tools/schema/generate.py --output generated/venue.hpp --namespace venue \
  --messages NewOrderSingle,OrderCancelRequest,ExecutionReport schemas/standard/FIX44.xml

# Or auto-prune messages + optional fields/groups directly from sample FIX logs (.log / .fix / .nfxj):
python3 tools/schema/generate.py --output generated/venue.hpp --namespace venue \
  --from-fix-log ./logs/venue.log schemas/standard/FIX44.xml
```

Per-session sub-millisecond timestamps (`timestamp_precision = TimestampPrecision::microseconds | nanoseconds`, TOML `"ms" | "us" | "ns"`) and explicit opt-in inbound checksum skip (`verify_checksum = false`, `true` by default) are configurable on both `DirectInitiatorConfig` and `SessionSettings`.

Arbitrary custom/venue tags across the full FIX range (`1..999,999,999`, e.g. `8899` or `900001`) are supported natively with zero bitset bloat — all slot tables and deduplication bitsets are indexed by compact per-message member index, never by raw tag number.

---

## Performance (Release, AMD Ryzen 7 7840HS, GCC 16.2.1, Linux 7.2)

Full methodology, boundary disclosures, and reproducible commands are in [docs/benchmarks.md](docs/benchmarks.md). All hot-path numbers below run with real **segmented mmap Journal + SSE4.2 hardware CRC32C** persistence and `-Wl,--wrap=malloc` verifying **0 heap allocations**.

### 1. Message Construction + Single-Pass FIX 4.4 Encode (`initiator-burst-bench`, 30,000 samples, alternating market/limit)

| Message API | p50 | p99 | Heap Allocs |
| :--- | ---: | ---: | :---: |
| **Streaming**: `encode_fix44_stream<NewOrderSingle>` + native `if` | **130.0 ns** | **170.0 ns** | **0** |
| **Streaming**: `encode_fix44_stream<NewOrderSingle>` + `std::optional` | **130.0 ns** | **180.0 ns** | **0** |
| **Strict**: `NewOrderSingle::Fixed<128, fix::field::...>` | **220.0 ns** | 391.0 ns | **0** |

### 2. Initiator Send Modes: Ping-Pong vs Open-Loop Dense Burst (`initiator-burst-bench`, real loopback TCP + FIX 4.4 + Journal)

CO-Free = `t_recv - t_burst_start` (Coordinated-Omission-Free one-way latency from burst arrival to peer `recv()`).

| Initiator Send Mode | Ping-Pong 1-Way p50 (RTT p50) | Burst 32×0 µs CO-Free p50 / p99 (Rate) | Burst 32×2 µs Compute CO-Free p50 / p99 (Rate) |
| :--- | ---: | ---: | ---: |
| **1. `DirectInitiator::send_stream()`** (1-thr, no queue) | **4.69 µs** (**8.86 µs**) | 49.93 µs / 98.19 µs (335k ord/s) | 86.87 µs / 228.77 µs (179k ord/s) |
| **2. `DirectInitiator::enqueue_stream()+flush()`** (1-thr `sendmsg`) | 5.42 µs (10.39 µs) | 14.58 µs / **19.83 µs** (1.27M ord/s) | 80.85 µs / 217.01 µs (320k ord/s) |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`** (2-thr SPSC) | 5.27 µs (9.09 µs) | **12.86 µs** / 22.64 µs (**1.43M ord/s**) | **42.62 µs** / **76.08 µs** (**422k ord/s**) |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`** (2-thr SPSC) | 7.69 µs (12.27 µs) | 67.28 µs / 117.28 µs (278k ord/s) | 64.77 µs / 118.97 µs (280k ord/s) |

### 3. `DirectInitiator` Layered Breakdown (`direct-initiator-bench`, full 912-field FIX 4.4 + `NoPartyIDs[3]` + mmap Journal)

| Workload Boundary | p50 | p95 | p99 | Throughput | Heap Allocs |
| :--- | ---: | ---: | ---: | ---: | :---: |
| **User Encode (`StreamWriter`)** (`encode_fix44_stream`) | **140.0 ns** | 150.0 ns | **151.0 ns** | **7,089,686 msgs/s** | **0** |
| **User Encode (`Fixed`)** (in-loop fill + `encode_fix44`) | 351.0 ns | 391.0 ns | 411.0 ns | 2,804,887 msgs/s | **0** |
| **Pre-TCP Session Outbound** (`DirectInitiator::send_stream` + mmap Journal) | **211.0 ns** | 280.0 ns | **421.0 ns** | **4,420,157 msgs/s** | **0** |
| **Pre-TCP Session Inbound** (frame + parse + 912-field Schema + dual Journal records + callback) | **721.0 ns** | 822.0 ns | **1,062.0 ns** | **1,392,046 msgs/s** | **0** |
| **Pre-TCP Combined** (total pure FIX engine round-trip budget) | **942.0 ns** | 1,091.0 ns | **1,442.0 ns** | **1,058,645 rt/s** | **0** |
| **Real TCP Loopback RTT** (kernel TCP + full FIX 4.4 + mmap Journal) | **9.67 µs** | 10.67 µs | 13.28 µs | 103,837 rt/s | **0** |

Binary footprint (Release): `direct-initiator-example` is **336 KB** (`libnimblefix-core.a`: 409 KB) vs full multi-session `initiator-example` at **1.2 MB**.

### 4. Acceptor & Same-Run `IoBackend` Comparison (`acceptor-bench`, full 912-field FIX 4.4 + `NoPartyIDs[3]` + mmap Journal)

| `IoBackend` (`acceptor-bench 2000 20000 8 all`) | 1-Session p50 | 1-Session p99 | 1-Session Rate | 8-Session p50 | 8-Session p99 | 8-Session Aggregate |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **`epoll`** (default, zero extra deps) | 14.68 µs | 32.07 µs | 62,553 ord/s | **44.56 µs** | 114.46 µs | 133,972 ord/s |
| **`busy_poll`** (zero-sleep nonblocking poll) | **12.45 µs** | 23.38 µs | **73,464 ord/s** | 45.38 µs | 118.44 µs | 129,649 ord/s |
| **`io_uring`** (Proactor `RECV`/`SEND`, sleeping) | 14.58 µs | **19.56 µs** | 66,515 ord/s | 49.10 µs | **97.90 µs** | **146,037 ord/s** (+9%) |

*(Pre-TCP Acceptor `SessionDriver` wire-in -> 912-field Schema -> in-handler ER -> Journal -> flush: **p50 = 1.71 µs**, **p99 = 3.03 µs**, **581,156 ord/s**; 4-shard pinned `ShardedEngine` (`hybrid_backoff`, 1 shared port): **8 sessions = 443k ord/s** with **p99 = 21.27 µs / p99.9 = 28.98 µs** (3.62× lower p99, 7.84× lower p99.9 than 1-core `epoll`), **16 sessions = 471k ord/s** (2.67×).)*

### 5. NimbleFIX 0.3 vs QuickFIX 1.16.0 (`quickfix-parity-bench`, Strictly Same-Methodology)

Both engines run in the same `-O3` binary against the same full 912-field `schemas/standard/FIX44.xml` (strict validation + 120 s `SendingTime` check ON), identical `NewOrderSingle` + `NoPartyIDs[1]` and 11-field `ExecutionReport` payloads, real on-disk persistence (v2 mmap Journal vs `FIX::FileStore`, OS page-cache, no `fsync`), logging disabled, and an identical neutral loopback TCP peer. Full breakdown in [docs/benchmarks.md](docs/benchmarks.md#4-nimblefix-03-vs-quickfix-1160--same-methodology-benchmark-quickfix-parity-bench).

| Suite (`1000` warmup / `5000` samples) | NimbleFIX 0.3 (p50 / p99 / alloc) | QuickFIX 1.16.0 (p50 / p99 / alloc) | Speedup (p50) |
| :--- | :--- | :--- | ---: |
| **Codec Encode** (`StreamWriter` / `Fixed`) | **170 ns** / **371 ns** (**0.00** alloc) | 1,463 ns / 1,523 ns / 19.00 | **8.61×** / **3.94×** |
| **Parse + Full `FIX44.xml` Strict + Read** | **421 ns** / **431 ns** / **0.00** | 2,795 ns / 3,386 ns / 15.00 | **6.64×** |
| **TCP Initiator RTT** (`DirectInitiator` / `Engine busy_poll` / `epoll`) | **10.21 µs** / **14.39 µs** / **19.16 µs** (**0.00** alloc) | 23.72 µs / p99 34.56 µs / 35.06 | **2.32×** / 1.65× / 1.24× |
| **TCP Acceptor RTT** (`Engine busy_poll` / `epoll`) | **10.59 µs** / **11.57 µs** (p99 **14.33 µs**, **0.00** alloc) | 23.20 µs / p99 33.92 µs / 37.00 | **2.19×** / 2.01× |
| **TCP Dense Burst 32×** (`enqueue_stream` + `sendmsg`, CO-Free 1-way) | **12.63 µs** / **23.51 µs** / **0.00** (**1.34M/s**) | 198.47 µs / 399.32 µs / 28.06 (80k/s) | **15.71×** (**16.6×** rate) |

---

## Layer 1 / 2: Multi-Session Engine, Pluggable `IoBackend`, Acceptor & CLI

For multi-session server/client gateways, Acceptor listeners, `Queued` worker dispatch, MPSC `Sender`, SPSC `ProducerLane`, or TOML-driven operations, start with [examples/acceptor.cpp](examples/acceptor.cpp) and [examples/initiator.cpp](examples/initiator.cpp).

### Pluggable `IoBackend` (`epoll`, `busy_poll`, optional `io_uring`)

The OS I/O multiplexing layer is decoupled from the FIX engine via `IoBackendConfig` (`<nimblefix/io_backend.hpp>`):

```cpp
nf::EngineConfig cfg{};
cfg.io_backend = nf::IoBackendConfig::io_uring(); // or ::epoll() (default) / ::busy_poll()
```

Or in TOML under `[engine]`:

```toml
[engine]
io_backend = "io_uring" # "epoll" (default) | "busy_poll" | "io_uring"
```

- `epoll` and `busy_poll` require zero extra dependencies.
- `io_uring` uses Linux `liburing` in true **Proactor** mode (`IORING_OP_RECV` / `IORING_OP_SEND` batched via `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN`) and is strictly opt-in (`--io_uring=y` in xmake, `-DNIMBLEFIX_ENABLE_IO_URING=ON` in CMake). When compiled without `liburing`, requesting `io_uring` returns `ErrorCode::unsupported` cleanly.

```sh
xmake f -m release --tls=y --xml=y --io_uring=y
xmake build acceptor-example initiator-example direct-initiator-example nimblefix-cli
```

Run the paired order-entry examples:

```sh
# Terminal 1:
xmake run acceptor-example --state ./state/sell --port 9876
# Terminal 2:
xmake run initiator-example --state ./state/buy --port 9876
```

Or use the TOML CLI (including out-of-band mmap telemetry & deterministic historical replay):

```sh
xmake run nimblefix-cli init --directory ./demo
xmake run nimblefix-cli validate --config ./demo/acceptor.toml --json
xmake run nimblefix-cli run --config ./demo/acceptor.toml
xmake run nimblefix-cli telemetry --path ./demo/telemetry.nfxt --prometheus
xmake run nimblefix-cli replay --path ./demo/state/.../session.nfxj --direction inbound --timing original --endpoint 127.0.0.1:9876
```

---

## Storage & Crash Recovery

- **Segmented mmap Journal**: All sessions (`DirectInitiator` and `Engine`) use pre-allocated segmented `MAP_SHARED` mmap journals with hardware SSE4.2 CRC32C, atomic `.tmp` + `rename` segment creation, zero-copy `reserve()`/`commit()`, and off-hot-path `poll_maintenance()`.
- **Exact recovery guarantees**: Inbound frames are journaled before business delivery; outbound frames are journaled before socket write. After process restart or `SIGKILL`, both `DirectInitiator` and `Engine` rebuild sequence state, re-deliver unfinished inbound records (`is_recovery == true`), and serve `ResendRequest`, `GapFill`, and business `Replay` accurately.
- **Durability scope**: Confirmation level `appended` covers same-host process crashes while the OS/filesystem remains healthy; no per-message `fsync`/`msync` is executed on the hot path.

---

## Build, Test & Install

Clone with submodules (`third_party/tomlplusplus` and `third_party/quickfix` are tracked as git submodules — no separate third-party downloads required):

```sh
git clone --recurse-submodules <repo-url>
# Or in an existing clone:
git submodule update --init --recursive
```

```sh
# Full xmake build & verification suite (Debug or Release)
xmake f -m release --tls=y --xml=y --benchmarks=y --quickfix=y -y
xmake check

# Run the benchmarks (including same-methodology QuickFIX 1.16.0 comparison)
xmake build initiator-burst-bench direct-initiator-bench acceptor-bench quickfix-parity-bench
./build/linux/x86_64/release/initiator-burst-bench
./build/linux/x86_64/release/direct-initiator-bench 5000 30000
./build/linux/x86_64/release/acceptor-bench 2000 20000 8 all
./build/linux/x86_64/release/quickfix-parity-bench 1000 5000 schemas/standard/FIX44.xml

# CMake build and relocatable install verification
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release \
  -DNIMBLEFIX_ENABLE_TLS=ON -DNIMBLEFIX_ENABLE_XML=ON \
  -DNIMBLEFIX_BUILD_BENCHMARKS=ON -DNIMBLEFIX_ENABLE_QUICKFIX=ON
cmake --build build-cmake -j
ctest --test-dir build-cmake --output-on-failure
python3 tests/install_test.py --features --examples
python3 tests/install_test.py --core-only --examples
```

Dependencies and acknowledgements are documented in [third_party/README.md](third_party/README.md).
