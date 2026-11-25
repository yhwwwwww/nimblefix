# NimbleFIX

**English** | [简体中文](README.zh-CN.md)

A modern, low-latency C++20 FIX protocol library supporting FIX 4.2, 4.3, 4.4, and FIXT 1.1 / 5.0 SP2. Built for deterministic sub-microsecond hot paths with zero steady-state heap allocations, segmented mmap persistence, and crash recovery.

[User Guide](docs/user-guide.md) · [Architecture](docs/architecture.md) · [Support Matrix](docs/session-support.md) · [Benchmarks](docs/benchmarks.md)

---

## Features at a Glance

- **Pay only for what you use**: Link a lightweight single-session client core (`NimbleFIX::nimblefix_core`), add OpenSSL 3 TLS (`NimbleFIX::nimblefix_core_tls`), or scale up to multi-session gateways and multi-core sharded servers (`NimbleFIX::nimblefix`).
- **Zero-allocation streaming codec**: Compile standard or custom FIX XML dictionaries into strongly typed C++20 headers with `nimblefix-schema-gen`. Encode directly into the wire buffer or mmap journal with native C++ control flow (`if`/`for`, `std::optional`, nested repeating groups) and zero intermediate objects.
- **Built-in persistence & crash recovery**: Every session uses a pre-allocated segmented mmap journal with hardware SSE4.2 CRC32C checksums. Inbound messages are persisted before delivery and outbound messages before socket write, providing automatic sequence recovery, `ResendRequest`/`GapFill`, and business replay across restarts.
- **Scalable I/O & multi-core runtime**: Choose between `epoll`, `busy_poll`, and optional Linux `io_uring`, or scale horizontally across pinned CPU cores with `ShardedEngine` on a single listening port.
- **Operational tooling included**: Multi-endpoint initiator failover (`ConnectionStrategy`), out-of-band mmap telemetry with nanosecond HDR latency histograms, deterministic journal replay, and an optional `nimblefix` CLI utility.

---

## Library Targets

| Layer | CMake / xmake Target | Included Capabilities | External Dependencies |
| :--- | :--- | :--- | :--- |
| **Core** | `NimbleFIX::nimblefix_core` / `nimblefix-core` | Streaming codec (`StreamWriter`), `Fixed<...>`, segmented mmap journal, `SessionCore`, nonblocking TCP (`sendmsg` batching), `DirectInitiator`, multi-endpoint failover, lock-free SPSC `DirectSendQueue`, mmap telemetry, `JournalReplayer` | C++20 + `pthread` |
| **Core + TLS** | `NimbleFIX::nimblefix_core_tls` / `nimblefix-core-tls` | Everything in Core + nonblocking OpenSSL 3 `DirectTlsTransport` (TLS 1.2/1.3, CA/SAN verification, mTLS) | OpenSSL 3+ |
| **Full Runtime** | `NimbleFIX::nimblefix` / `nimblefix` | Everything above + `Engine<Schema, Inline\|Queued>`, multi-core `ShardedEngine`, Acceptor, pluggable `IoBackend` (`epoll` / `busy_poll` / `io_uring`), MPSC `Sender`, SPSC `ProducerLane`, optional runtime XML (`NimbleFIX::nimblefix_xml`) | Optional `liburing`, `libxml2` |

---

## Quick Example: Low-Latency Initiator

See [examples/direct_initiator.cpp](examples/direct_initiator.cpp) for a complete runnable program, or read the [User Guide](docs/user-guide.md) for multi-session gateways, multi-core servers, threading models, and responsibility boundaries.

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
        (void)info; // info.is_recovery == true after crash recovery
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

const bool is_limit = true;
const std::optional<nf::Decimal> opt_px = nf::Decimal{15025, 2};
auto write_order = [&](auto& w) {
    // Optional per-message header fields (must precede body fields):
    // w.header().sender_sub_id("DESK-A").target_sub_id("EXCH-1");
    w.cl_ord_id("ORD-001")
     .symbol("AAPL")
     .side('1')
     .transact_time("20261125-09:30:00.000")
     .order_qty(nf::Decimal{100, 0})
     .ord_type(is_limit ? '2' : '1')
     .price(opt_px); // skipped automatically when std::nullopt
};

// Three composable send modes on DirectInitiator:
(void)init.send_stream<fix::NewOrderSingle>(write_order);       // 1) Immediate single-thread send
// (void)init.enqueue_stream<fix::NewOrderSingle>(write_order); // 2) Same-thread batch -> 1 sendmsg()
// (void)init.flush();
// nf::DirectSendQueue q{256, 512};                             // 3) 2-thread lock-free SPSC pipeline:
// (void)q.try_push_stream<fix::NewOrderSingle>(write_order);   //    strategy thread pushes,
// (void)init.drain(q);                                         //    I/O thread drains -> Journal + sendmsg()
```

---

## Performance Highlights

Measured in Release mode (`-O3`, GCC 16.2.1, AMD Ryzen 7 7840HS, Linux 7.2) with the full 912-field `FIX44.xml` dictionary, strict validation, and real segmented mmap journal persistence. All NimbleFIX hot paths record **0 heap allocations**. See [Benchmarks](docs/benchmarks.md) for full methodology and reproduction commands.

| Benchmark | NimbleFIX (p50 / p99) | Throughput | Heap Allocs |
| :--- | ---: | ---: | :---: |
| **Stream Encode** (`NewOrderSingle` + `NoPartyIDs[3]`) | **140 ns** / **151 ns** | 7.09M msgs/s | 0 |
| **Strict Parse + 912-Field Validate + Read** | **421 ns** / **431 ns** | 2.36M msgs/s | 0 |
| **Pre-TCP Engine Round-Trip** (`DirectInitiator` + mmap Journal) | **942 ns** / **1.44 µs** | 1.06M rt/s | 0 |
| **Loopback TCP Ping-Pong RTT** (`DirectInitiator`) | **9.67 µs** / **13.28 µs** | 104k rt/s | 0 |
| **Loopback TCP 32-Order Burst** (`DirectSendQueue`, CO-Free 1-way) | **12.86 µs** / **22.64 µs** | 1.43M ord/s | 0 |
| **4-Shard `ShardedEngine` Acceptor** (16 sessions, 1 port) | **31.19 µs** / **49.99 µs** | 471k ord/s | 0 |

In same-methodology parity tests against QuickFIX 1.16.0 (identical `FIX44.xml` strict validation and on-disk persistence): **8.6× faster** encode, **6.6× faster** strict parse & validate, **2.3× lower** TCP RTT, and **15.7× lower** 32-order burst latency.

---

## Building & Integrating

Clone with submodules:

```sh
git clone --recurse-submodules <repo-url>
```

### Build & Test with CMake

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release \
  -DNIMBLEFIX_ENABLE_TLS=ON -DNIMBLEFIX_ENABLE_XML=ON
cmake --build build-cmake -j
ctest --test-dir build-cmake --output-on-failure
```

### Build & Test with xmake

```sh
xmake f -m release --tls=y --xml=y -y
xmake check
```

*(Pass `-DNIMBLEFIX_ENABLE_IO_URING=ON` or `--io_uring=y` to enable the Linux `io_uring` backend.)*

### Use in Your CMake Project

```cmake
find_package(NimbleFIX CONFIG REQUIRED COMPONENTS core)

# Pick the layer your application needs:
target_link_libraries(my_trader PRIVATE NimbleFIX::nimblefix_core)
# target_link_libraries(my_trader PRIVATE NimbleFIX::nimblefix_core_tls)
# target_link_libraries(my_gateway PRIVATE NimbleFIX::nimblefix)

# Optional: compile a custom FIX XML dictionary into a typed C++20 header:
nimblefix_generate_schema(TARGET my_venue_schema NAMESPACE my::venue
    HEADER venue.hpp XML_FILES "${CMAKE_CURRENT_SOURCE_DIR}/venue.xml")
target_link_libraries(my_trader PRIVATE my_venue_schema)
```

Dependencies and third-party licenses are listed in [third_party/README.md](third_party/README.md).
