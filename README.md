# NimbleFIX 0.3

A C++20 FIX engine designed around **pay only for what you need**: start with a zero-overhead Layer 0 **DirectInitiator** (`NimbleFIX::nimblefix_core`, 0 malloc / 0 mutex / 0 fsync steady state, full restart recovery & GapFill/Replay), opt into Layer 0 OpenSSL 3 TLS (`NimbleFIX::nimblefix_core_tls`), or scale up to the full Layer 1/2 multi-session **Engine** (`NimbleFIX::nimblefix`) with pluggable **`IoBackend`** (`epoll`, `busy_poll`, optional Linux `io_uring` Proactor), Acceptor, `Inline`/`Queued` dispatch, MPSC/SPSC senders, runtime XML, and TOML CLI.

[中文入门](docs/getting-started.zh-CN.md) · [Architecture](docs/architecture.md) · [Support Matrix](docs/session-support.md) · [Benchmarks](docs/benchmarks.md)

---

## Architecture: 3-Layer Pay-Only-For-What-You-Need

| Layer | CMake Target / xmake Target | Included Capabilities | Extra Dependencies |
| :--- | :--- | :--- | :--- |
| **Layer 0 (Core)** | `NimbleFIX::nimblefix_core` / `nimblefix-core` | Single-pass backpatch Codec, SSO `make()` & mutable `Draft`, strict `Fixed<...>`, generated Schemas (`--messages` subset codegen), segmented mmap Journal (hardware SSE4.2 CRC32C, `reserve()`/`commit()`), `SessionCore`, nonblocking `TcpTransport` (`write_batch`/`sendmsg`), **`DirectInitiator`**, lock-free SPSC **`DirectSendQueue`** | C++20 + `pthread` only |
| **Layer 0 + TLS** | `NimbleFIX::nimblefix_core_tls` / `nimblefix-core-tls` | Everything in Layer 0 + nonblocking OpenSSL 3 **`DirectTlsTransport`** (TLS 1.2/1.3, CA/hostname/IP verification, mTLS, `TlsContext` reuse on reconnect) | OpenSSL 3+ (optional) |
| **Layer 1 / 2 (Full)** | `NimbleFIX::nimblefix` / `nimblefix` | Everything above + `SessionDriver`, pluggable `IoBackend` (`epoll` / `busy_poll` / optional `io_uring` Proactor), multi-session `Engine<Schema, Inline\|Queued>`, Acceptor, MPSC `Sender`, SPSC `ProducerLane`, optional runtime libxml2 XML & TOML CLI | Optional `liburing`, `libxml2` |

---

## Quickstart: Layer 0 `DirectInitiator` (Zero-Overhead Initiator)

See [examples/direct_initiator.cpp](examples/direct_initiator.cpp) for a complete runnable initiator with zero-allocation SSO builders, segmented mmap Journal persistence, automatic restart recovery, and GapFill/Replay:

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

// Option A: One-shot fluent chain make() / make<Slots, Bytes>() (compile-time wire order,
//           conditional fields via std::optional, default 16-slot / 256B SSO with heap spill):
auto order = fix::NewOrderSingle::make()
    .cl_ord_id("ORD-001").symbol("AAPL").side('1')
    .transact_time("20261123-00:00:00.000")
    .order_qty(nf::Decimal{100, 0}).ord_type('1')
    .price(std::optional<nf::Decimal>{std::nullopt}); // absent optional skipped automatically

// Repeating groups (same setter name for both fixed-count and dynamic-count, both 0-alloc!):
//   Fixed count (pass entries directly — compile-time tuple, 0 copy / 0 alloc / 0 overflow):
//     .no_party_i_ds(
//         fix::NoPartyIDsGroupEntry::make().party_id("CLIENT1").party_id_source('D').party_role(1))
//   Dynamic count (loop -> shared 1 KB inline arena, auto heap spill on overflow):
//     fix::NoPartyIDsGroup<> parties;
//     for (const auto& p : list) (void)parties.add(fix::NoPartyIDsGroupEntry::make().party_id(p.id)...);
//     ...no_party_i_ds(std::move(parties));

// Option B: Mutable reusable Draft / DraftWith<Slots, Bytes> (statement-by-statement mutation,
//           native runtime `if`, field overwrite, and reset() reuse; same 16-slot / 256B SSO):
// fix::NewOrderSingle::Draft order;
// order.cl_ord_id("ORD-001");
// order.symbol("AAPL");
// if (has_price) order.price(nf::Decimal{15025, 2});

// 3 send modes on DirectInitiator:
(void)init.send(order);               // 1) Direct immediate send (lowest single-order latency)
// (void)init.enqueue(order);         // 2) Same-thread burst batching -> 1 writev/sendmsg
// (void)init.flush();
// nf::DirectSendQueue q{256, 512};   // 3) 2-thread lock-free SPSC: strategy q.try_push(order),
// (void)init.drain(q);               //    I/O thread init.drain(q) -> Journal + 1 writev/sendmsg
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

### Subset Schema Codegen (`--messages`)

Trim generated headers and compile times to only the business messages you trade (admin messages and referenced groups/components are kept automatically):

```sh
python3 tools/schema/generate.py --output generated/venue.hpp --namespace venue \
  --messages NewOrderSingle,OrderCancelRequest,ExecutionReport schemas/standard/FIX44.xml
```

Arbitrary custom/venue tags across the full FIX range (`1..999,999,999`, e.g. `8899` or `900001`) are supported natively with zero bitset bloat — all slot tables and deduplication bitsets are indexed by compact per-message member index, never by raw tag number.

---

## Performance (Release, AMD Ryzen 7 7840HS, GCC 16.2.1, Linux 7.2)

Full methodology, boundary disclosures, and reproducible commands are in [docs/benchmarks.md](docs/benchmarks.md). All hot-path numbers below run with real **segmented mmap Journal + SSE4.2 hardware CRC32C** persistence and `-Wl,--wrap=malloc` verifying **0 heap allocations**.

### 1. Message Construction + Single-Pass FIX 4.4 Encode (`initiator-burst-bench`, 30,000 samples, alternating market/limit)

| Message API | p50 | p99 | Heap Allocs |
| :--- | ---: | ---: | :---: |
| **Scheme A**: `NewOrderSingle::make()` + `std::optional` (SSO `<16, 256>`) | **340.0 ns** | 390.0 ns | **0** |
| **Scheme B**: `NewOrderSingle::Draft` + native `if` + `reset()` (SSO `<16, 256>`) | **321.0 ns** | 411.0 ns | **0** |
| **Strict**: `NewOrderSingle::Fixed<128, fix::field::...>` | **221.0 ns** | 341.0 ns | **0** |

### 2. Initiator Send Modes: Ping-Pong vs Open-Loop Dense Burst (`initiator-burst-bench`, real loopback TCP + FIX 4.4 + Journal)

CO-Free = `t_recv - t_burst_start` (Coordinated-Omission-Free one-way latency from burst arrival to peer `recv()`).

| Initiator Send Mode | Ping-Pong 1-Way p50 (RTT p50) | Burst 32×0 µs CO-Free p50 / p99 (Rate) | Burst 32×2 µs Compute CO-Free p50 / p99 (Rate) |
| :--- | ---: | ---: | ---: |
| **1. `DirectInitiator::send()`** (1-thr, no queue) | **4.97 µs** (9.62 µs) | 57.04 µs / 113.00 µs (292k ord/s) | 100.50 µs / 198.74 µs (166k ord/s) |
| **2. `DirectInitiator::enqueue()+flush()`** (1-thr `sendmsg`) | 5.01 µs (9.67 µs) | 22.73 µs / **30.50 µs** (946k ord/s) | 95.40 µs / 130.63 µs (283k ord/s) |
| **3. `DirectInitiator + DirectSendQueue`** (2-thr SPSC + `sendmsg`) | 5.13 µs (**9.33 µs**) | **18.14 µs** / 41.65 µs (**1.02M ord/s**) | **48.52 µs** / **88.70 µs** (**368k ord/s**) |
| **4. `Engine(busy_poll) + ProducerLane`** (2-thr SPSC) | 7.89 µs (13.11 µs) | 76.31 µs / 155.31 µs (249k ord/s) | 79.85 µs / 152.32 µs (231k ord/s) |

### 3. `DirectInitiator` Layered Breakdown (`direct-initiator-bench`, full 912-field FIX 4.4 + `NoPartyIDs[3]` + mmap Journal)

| Workload Boundary | p50 | p95 | p99 | Throughput | Heap Allocs |
| :--- | ---: | ---: | ---: | ---: | :---: |
| **User Encode** (in-loop fill + single-pass `encode_fix44`) | **291.0 ns** | 311.0 ns | 390.0 ns | 3,345,749 msgs/s | **0** |
| **Pre-TCP Session Outbound** (in-loop fill + `DirectInitiator::send` + mmap Journal) | **521.0 ns** | 621.0 ns | 832.0 ns | 1,849,466 msgs/s | **0** |
| **Pre-TCP Session Inbound** (frame + parse + 912-field Schema + dual Journal records + callback) | **1,002.0 ns** | 1,102.0 ns | 1,393.0 ns | 973,061 msgs/s | **0** |
| **Pre-TCP Combined** (total pure FIX engine round-trip budget) | **1.52 µs** | 1.69 µs | 2.37 µs | 637,600 rt/s | **0** |
| **Real TCP Loopback RTT** (kernel TCP + full FIX 4.4 + mmap Journal) | **9.25 µs** | 11.26 µs | 16.93 µs | 103,134 rt/s | **0** |

Binary footprint (Release): `direct-initiator-example` is **336 KB** (`libnimblefix-core.a`: 409 KB) vs full multi-session `initiator-example` at **1.2 MB**.

### 4. Acceptor & Same-Run `IoBackend` Comparison (`acceptor-bench`, full 912-field FIX 4.4 + `NoPartyIDs[3]` + mmap Journal)

| `IoBackend` (`acceptor-bench 2000 20000 8 all`) | 1-Session p50 | 1-Session p99 | 1-Session Rate | 8-Session p50 | 8-Session p99 | 8-Session Aggregate |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **`epoll`** (default, zero extra deps) | 29.92 µs | 48.59 µs | 31,272 ord/s | 87.24 µs | 224.62 µs | 82,728 ord/s |
| **`busy_poll`** (zero-sleep nonblocking poll) | **13.02 µs** | **33.64 µs** | **64,758 ord/s** | **50.21 µs** | 133.65 µs | 121,751 ord/s |
| **`io_uring`** (Proactor `RECV`/`SEND`, sleeping) | **15.16 µs** | 43.70 µs | 55,969 ord/s | **52.40 µs** | **95.58 µs** | **137,321 ord/s** (+66%) |

*(Pre-TCP Acceptor `SessionDriver` wire-in -> 912-field Schema -> in-handler ER -> Journal -> flush: **p50 = 1.53 µs**, **p99 = 2.18 µs**, **621,161 ord/s**.)*

### 5. NimbleFIX 0.3 vs QuickFIX 1.16.0 (`quickfix-parity-bench`, Strictly Same-Methodology)

Both engines run in the same `-O3` binary against the same full 912-field `schemas/standard/FIX44.xml` (strict validation + 120 s `SendingTime` check ON), identical `NewOrderSingle` + `NoPartyIDs[1]` and 11-field `ExecutionReport` payloads, real on-disk persistence (v2 mmap Journal vs `FIX::FileStore`, OS page-cache, no `fsync`), logging disabled, and an identical neutral loopback TCP peer. Full breakdown in [docs/benchmarks.md](docs/benchmarks.md#4-nimblefix-03-vs-quickfix-1160--same-methodology-benchmark-quickfix-parity-bench).

| Suite (`1000` warmup / `5000` samples) | NimbleFIX 0.3 (p50 / p99 / alloc) | QuickFIX 1.16.0 (p50 / p99 / alloc) | Speedup (p50) |
| :--- | :--- | :--- | ---: |
| **Codec Encode** (`Fixed` / `make()` direct / `NoPartyIDs<>`) | **371 ns** / **611 ns** / **681 ns** (**0.00** alloc) | 1,653 ns / 1,703 ns / 19.00 | **4.46×** / **2.71×** / **2.43×** |
| **Parse + Full `FIX44.xml` Strict + Read** | **1,323 ns** / **1,663 ns** / **0.00** | 2,775 ns / 2,816 ns / 15.00 | **2.10×** |
| **TCP Initiator RTT** (`DirectInitiator` / `Engine busy_poll` / `epoll`) | **9.79 µs** / **14.83 µs** / **18.55 µs** (**0.00** alloc) | 22.43 µs / p99 32.64 µs / 35.06 | **2.29×** / 1.51× / 1.21× |
| **TCP Acceptor RTT** (`Engine busy_poll` / `epoll`) | **10.98 µs** / **13.29 µs** (p99 **14.70 µs**, **0.00** alloc) | 23.82 µs / p99 32.48 µs / 36.99 | **2.17×** / 1.79× |
| **TCP Dense Burst 32×** (`batch sendmsg`, CO-Free 1-way) | **26.61 µs** / **36.21 µs** / **0.00** (**827k/s**) | 200.92 µs / 403.16 µs / 28.06 (79k/s) | **7.55×** (**10.5×** rate) |

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

Or use the TOML CLI:

```sh
xmake run nimblefix-cli init --directory ./demo
xmake run nimblefix-cli validate --config ./demo/acceptor.toml --json
xmake run nimblefix-cli run --config ./demo/acceptor.toml
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
