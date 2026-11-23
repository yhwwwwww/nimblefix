# NimbleFIX 0.3

A C++20 FIX engine designed around **pay only for what you need**: start with a zero-overhead Layer 0 **DirectInitiator** (`NimbleFIX::nimblefix_core`, 0 malloc / 0 mutex / 0 fsync steady state, full restart recovery & GapFill/Replay), opt into Layer 0 OpenSSL 3 TLS (`NimbleFIX::nimblefix_core_tls`), or scale up to the full Layer 1/2 multi-session **Engine** (`NimbleFIX::nimblefix`) with pluggable **`IoBackend`** (`epoll`, `busy_poll`, optional Linux `io_uring` Proactor), Acceptor, `Inline`/`Queued` dispatch, MPSC/SPSC senders, runtime XML, and TOML CLI.

[中文入门](docs/getting-started.zh-CN.md) · [Supported behavior](docs/session-support.md) ·
[Design](docs/rewrite-design.md)

---

## Architecture: 3-Layer Pay-Only-For-What-You-Need

| Layer | CMake Target / xmake Target | Included Capabilities | Extra Dependencies |
| :--- | :--- | :--- | :--- |
| **Layer 0 (Core)** | `NimbleFIX::nimblefix_core` / `nimblefix-core` | Single-pass backpatch Codec, `FixedMessage`, generated Schemas (`--messages` subset codegen), pure v2 segmented mmap Journal (hardware SSE4.2 CRC32C, `reserve()`/`commit()`), `SessionCore`, nonblocking `TcpTransport`, **`DirectInitiator<Schema, Handler, Transport>`** | C++20 + `pthread` only |
| **Layer 0 + TLS** | `NimbleFIX::nimblefix_core_tls` / `nimblefix-core-tls` | Everything in Layer 0 + nonblocking OpenSSL 3 **`DirectTlsTransport`** (TLS 1.2/1.3, CA/hostname/IP verification, mTLS, `TlsContext` reuse on reconnect) | OpenSSL 3+ (optional) |
| **Layer 1 / 2 (Full)** | `NimbleFIX::nimblefix` / `nimblefix` | Everything above + `SessionDriver`, pluggable `IoBackend` (`epoll` / `busy_poll` / optional `io_uring` Proactor), multi-session `Engine<Schema, Inline\|Queued>`, Acceptor, MPSC `Sender`, SPSC `ProducerLane`, optional runtime libxml2 XML & TOML CLI | Optional `liburing`, `libxml2` |

---

## Quickstart: Layer 0 `DirectInitiator` (Zero-Overhead Initiator)

See [examples/direct_initiator.cpp](examples/direct_initiator.cpp) for a complete runnable initiator with zero-allocation `FixedMessage`, v2 mmap Journal persistence, automatic restart recovery, and GapFill/Replay:

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

// Option A: Fluent make() + std::optional (0 malloc via 16-slot / 256B SSO; spills to heap if larger):
auto order = fix::NewOrderSingle::make()
    .cl_ord_id("ORD-001").symbol("AAPL").side('1')
    .transact_time("20261123-00:00:00.000")
    .order_qty(nf::Decimal{100, 0}).ord_type('1')
    .price(std::optional<nf::Decimal>{std::nullopt}); // absent optional skipped automatically

// Option B: Reusable Draft + native runtime `if` (no upfront field declaration, 0 malloc via SSO):
// fix::NewOrderSingle::Draft order;
// order.cl_ord_id("ORD-001").symbol("AAPL").side('1')
//      .transact_time("20261123-00:00:00.000").order_qty(nf::Decimal{100, 0}).ord_type('2');
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

Measured with `direct-initiator-bench` (30,000 samples, 5,000 warmups, `-Wl,--wrap=malloc` verifying **0 allocations**) on the **full 912-field FIX 4.4 standard schema** (`NewOrderSingle` populated inside the timed loop with 7 scalars including `Price` + 3-field `NoPartyIDs` repeating group; 11-field `ExecutionReport`; real **v2 segmented mmap Journal + SSE4.2 hardware CRC32C** persistence):

| Workload Boundary | p50 | p95 | p99 | Throughput | Heap Allocs |
| :--- | ---: | ---: | ---: | ---: | :---: |
| **User Encode** (in-loop fill + single-pass `encode_fix44`) | **291.0 ns** | 311.0 ns | 390.0 ns | 3,345,749 msgs/s | **0** |
| **Pre-TCP Session Outbound** (in-loop fill + `DirectInitiator::send` + v2 mmap Journal) | **521.0 ns** | 621.0 ns | 832.0 ns | 1,849,466 msgs/s | **0** |
| **Pre-TCP Session Inbound** (frame + parse + 912-field FIX 4.4 Schema + dual v2 Journal records + callback) | **1,002.0 ns** | 1,102.0 ns | 1,393.0 ns | 973,061 msgs/s | **0** |
| **Pre-TCP Combined** (total pure FIX engine round-trip budget) | **1,523.0 ns (1.52 µs)** | 1,693.0 ns | 2,374.0 ns | 637,600 rt/s | **0** |
| **Real TCP Loopback RTT** (kernel TCP + full FIX 4.4 + v2 mmap Journal) | **9.248 µs** | 11.261 µs | 16.931 µs | 103,134 rt/s | **0** |

Binary footprint (Release): `direct-initiator-example` is **336 KB** (`libnimblefix-core.a`: 409 KB) vs full multi-session `initiator-example` at **1.2 MB**.

### Acceptor Benchmark (`acceptor-bench`, same full 912-field FIX 4.4 + `NoPartyIDs[3]` + v2 mmap Journal workload)

| Acceptor Tier | p50 | p95 | p99 | Throughput |
| :--- | ---: | ---: | ---: | ---: |
| **Tier 1: Pre-TCP Acceptor** (`SessionDriver` wire-in -> 912-field Schema -> in-handler ER -> zero-copy v2 Journal -> flush) | **1.533 µs** | 1.723 µs | 2.184 µs | **621,161 orders/s** |
| **Tier 2: Single-Session TCP** (`Engine<fix44::Schema, Inline>`, 1 `epoll` thread) | **11.681 µs** | 15.038 µs | 19.466 µs | **77,334 orders/s** |
| **Tier 3: 8 Concurrent Sessions TCP** (`Engine<fix44::Schema, Inline>`, 1 `epoll` thread) | **36.678 µs** | 80.390 µs | 100.798 µs | **157,998 orders/s** |

#### Same-Run `IoBackend` Comparison (`acceptor-bench 2000 20000 8 all`, back-to-back under identical clock/thermal state)

| `IoBackend` | 1-Session p50 | 1-Session p99 | 1-Session Rate | 8-Session p50 | 8-Session p99 | 8-Session Aggregate |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **`epoll`** (default, zero extra deps) | 29.916 µs | 48.591 µs | 31,272 orders/s | 87.242 µs | 224.618 µs | 82,728 orders/s |
| **`busy_poll`** (zero-sleep nonblocking poll) | **13.024 µs** | **33.643 µs** | **64,758 orders/s** | **50.213 µs** | 133.649 µs | 121,751 orders/s |
| **`io_uring`** (Proactor `RECV`/`SEND`, sleeping) | **15.158 µs** | 43.702 µs | 55,969 orders/s | **52.397 µs** | **95.578 µs** | **137,321 orders/s** (+66%) |

---

## Layer 1 / 2: Multi-Session Engine, Pluggable `IoBackend`, Acceptor & CLI

For multi-session server/client gateways, Acceptor listeners, `Queued` worker dispatch, MPSC `Sender`, SPSC `ProducerLane`, or TOML-driven operations, start with [examples/acceptor.cpp](examples/acceptor.cpp) and [examples/initiator.cpp](examples/initiator.cpp).

### Pluggable `IoBackend` (`epoll`, `busy_poll`, optional `io_uring`)

The OS I/O multiplexing layer is cleanly decoupled from the FIX engine via `IoBackendConfig` (`<nimblefix/io_backend.hpp>`):

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

> **Acceptor Threading & Roadmap**: The protocol I/O layer runs a single-threaded `IoBackend` (`epoll` / `busy_poll` / `io_uring`) over pre-configured sessions so every session remains strictly single-writer and lock-free (preserving FIX `MsgSeqNum` ordering, L1/L2 cache locality, and zero-lock v2 mmap Journal writes), while business callbacks can run either `Inline` or in per-session serialized `Queued` workers. For future multi-core scaling beyond single-core limits (hundreds/thousands of high-rate sessions), the planned architecture is a **Sharded Reactor** (`K` independent `IoBackend` threads with static session-to-shard pinning, keeping every shard 100% single-writer and lock-free — never work-stealing across protocol state).

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

- **Unified v2 segmented mmap Journal**: Legacy v1 single-file storage has been removed in 0.3.0. All sessions (both `DirectInitiator` and `Engine`) use segmented pre-allocated mmap journals with hardware SSE4.2 CRC32C, atomic `.tmp` + `rename` segment creation, and off-hot-path `poll_maintenance()`.
- **Exact recovery guarantees**: Inbound frames are journaled before business delivery; outbound frames are journaled before socket write. After process restart or `SIGKILL`, both `DirectInitiator` and `Engine` rebuild sequence state, re-deliver unfinished inbound records (`is_recovery == true`), and serve `ResendRequest`, `GapFill`, and business `Replay` accurately.
- **Durability scope**: Confirmation level `appended` covers same-host process crashes while the OS/filesystem remains healthy; no per-message `fsync`/`msync` is executed on the hot path.

---

## Build, Test & Install

```sh
# Full xmake build & verification suite (Debug or Release)
xmake f -m release --tls=y --xml=y --benchmarks=y -y
xmake check

# Run the DirectInitiator benchmark (Pre-TCP + Real TCP Loopback)
xmake build direct-initiator-bench
./build/linux/x86_64/release/direct-initiator-bench 5000 30000

# CMake build, sanitizers, and relocatable install verification
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release \
  -DNIMBLEFIX_ENABLE_TLS=ON -DNIMBLEFIX_ENABLE_XML=ON
cmake --build build-cmake -j
ctest --test-dir build-cmake --output-on-failure
python3 tests/install_test.py --features --examples
python3 tests/install_test.py --core-only --examples
```

Dependencies and acknowledgements are documented in [third_party/README.md](third_party/README.md).
