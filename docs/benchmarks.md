# NimbleFIX Benchmarks

**English** | [简体中文](benchmarks.zh-CN.md)

All measurements below were collected on the same machine in Release mode with real segmented mmap journal persistence (hardware SSE4.2 CRC32C, no in-memory stub store) and `-Wl,--wrap=malloc` allocation auditing on hot paths.

> Related docs: [User Guide](user-guide.md) · [Architecture](architecture.md) · [Support Matrix](session-support.md)

## Test Environment

| Item | Value |
| :--- | :--- |
| CPU | AMD Ryzen 7 7840HS (8C / 16T) |
| OS / Kernel | Linux 7.2 (`x86_64`, loopback TCP with `TCP_NODELAY`) |
| Compiler | GCC 16.2.1 (`xmake f -m release --tls=y --xml=y --benchmarks=y`, `-O3`) |
| Schema | Full standard **FIX 4.4** (`schemas/standard/FIX44.xml`, 912 fields) |
| Persistence | Real segmented `MAP_SHARED` mmap journal + hardware SSE4.2 CRC32C (`appended` confirmation) |
| Clock | `CLOCK_MONOTONIC` (`clock_gettime`) in user space |

> **Boundary Note**: Loopback TCP measurements include full Linux kernel TCP/IP stack traversal (`send`/`sendmsg` + `recv`). Pre-TCP measurements isolate pure NimbleFIX codec, schema validation, session state machine, and mmap journal cost.

---

## 1. Open-Loop Burst & One-Way Timestamp Benchmark (`initiator-burst-bench`)

Reproduce:
```sh
xmake f -m release --tls=y --xml=y --benchmarks=y -y
xmake build initiator-burst-bench
./build/linux/x86_64/release/initiator-burst-bench
```

### Part 0: Message Construction + FIX 4.4 Encode
*30,000 samples (5,000 warmup), alternating Market (`40=1`, no `Price`) and Limit (`40=2`, with `Price=150.25`) `NewOrderSingle`.*

| Message API | p50 | p99 | Heap Allocs (`__wrap_malloc`) |
| :--- | ---: | ---: | :---: |
| **Streaming**: `encode_fix44_stream<NewOrderSingle>` + native `if` | **130.0 ns** | **170.0 ns** | **0** |
| **Streaming**: `encode_fix44_stream<NewOrderSingle>` + `std::optional` | **130.0 ns** | **180.0 ns** | **0** |
| **Fixed**: `NewOrderSingle::Fixed<128, fix::field::...>` | **220.0 ns** | **391.0 ns** | **0** |

### Part 1–3: Initiator Send Mode Comparison (Loopback TCP + FIX 4.4 + mmap Journal)

Each order embeds `<t_burst_ns>-<t_send_ns>` in `ClOrdID (11)`. The receiving peer timestamps `t_recv_ns = now_ns()` immediately on `::recv()` and reports two one-way metrics:
- **CO-Free (`t_recv - t_burst_start`)**: Coordinated-Omission-Free latency from burst arrival until the counterparty received order `i`.
- **Enq->Recv (`t_recv - t_send_stamp`)**: Per-order handoff-to-recv latency (useful for diagnosing transport queueing, though it hides head-of-line blocking when the producer blocks on I/O).

#### Workload 1: Ping-Pong (1 in-flight, 5,000 orders, 500 warmup)

| Mode | 1-Way p50 | 1-Way p99 | RTT p50 | Throughput |
| :--- | ---: | ---: | ---: | ---: |
| **1. `DirectInitiator::send_stream()`** (1-thread, no queue) | **4.64 µs** | **7.61 µs** | **8.61 µs** | **110,081 rt/s** |
| **2. `DirectInitiator::enqueue_stream()+flush()`** (1-thread `sendmsg`) | 4.74 µs | 8.03 µs | 8.76 µs | 102,958 rt/s |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`** (2-thread SPSC) | 5.11 µs | 7.69 µs | 8.65 µs | **112,950 rt/s** |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`** (2-thread SPSC) | **5.48 µs** | **7.96 µs** | **9.92 µs** | **98,067 rt/s** |

#### Workload 2: Dense Burst — 0 µs Strategy Compute (200 bursts × 32 orders, 20 warmup bursts)

| Mode | CO-Free p50 | CO-Free p99 | Enq->Recv p50 | Throughput |
| :--- | ---: | ---: | ---: | ---: |
| **1. `DirectInitiator::send_stream()`** (1-thread, no queue) | 49.18 µs | 99.08 µs | 4.62 µs | 341,745 ord/s |
| **2. `DirectInitiator::enqueue_stream()+flush()`** (1-thread `sendmsg`) | **12.19 µs** | **19.32 µs** | 9.63 µs | **1,402,288 ord/s** |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`** (2-thread SPSC + `sendmsg`) | 13.03 µs | 25.00 µs | 12.38 µs | 1,344,907 ord/s |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`** (2-thread SPSC + `sendmsg`) | **17.18 µs** | **26.87 µs** | 12.72 µs | **1,178,200 ord/s** |

#### Workload 3: Dense Burst + 2 µs Strategy Compute per Order (200 bursts × 32 orders)

| Mode | CO-Free p50 | CO-Free p99 | Enq->Recv p50 | Throughput |
| :--- | ---: | ---: | ---: | ---: |
| **1. `DirectInitiator::send_stream()`** (1-thread, no queue) | 85.58 µs | 169.47 µs | 5.48 µs | 192,543 ord/s |
| **2. `DirectInitiator::enqueue_stream()+flush()`** (1-thread `sendmsg`) | 78.57 µs | 83.32 µs | 42.18 µs | 358,644 ord/s |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`** (2-thread SPSC + `sendmsg`) | **41.50 µs** | **78.99 µs** | 6.61 µs | **409,374 ord/s** |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`** (2-thread SPSC + `sendmsg`) | **44.11 µs** | **81.11 µs** | **6.53 µs** | **398,843 ord/s** |

---

## 2. `DirectInitiator` Layered Breakdown (`direct-initiator-bench`)

Reproduce:
```sh
xmake build direct-initiator-bench
./build/linux/x86_64/release/direct-initiator-bench 5000 30000
```

Workload: Full 912-field FIX 4.4 `NewOrderSingle` (7 scalars including `Price` + 3-field `NoPartyIDs` repeating group, populated inside the timed loop) and 11-field `ExecutionReport`, with segmented mmap journal + SSE4.2 CRC32C.

| Boundary | p50 | p95 | p99 | Throughput | Heap Allocs |
| :--- | ---: | ---: | ---: | ---: | :---: |
| **User Encode (`StreamWriter`)** (`encode_fix44_stream`, no session/journal) | **140.0 ns** | 150.0 ns | **151.0 ns** | **7,089,686 msgs/s** | **0** |
| **User Encode (`Fixed`)** (in-loop fill + `encode_fix44`, no session/journal) | 351.0 ns | 391.0 ns | 411.0 ns | 2,804,887 msgs/s | **0** |
| **Pre-TCP Outbound** (`DirectInitiator::send_stream` + mmap journal) | **211.0 ns** | 280.0 ns | **421.0 ns** | **4,420,157 msgs/s** | **0** |
| **Pre-TCP Inbound** (frame + parse + 912-field Schema + dual journal records + callback) | **721.0 ns** | 822.0 ns | **1,062.0 ns** | **1,392,046 msgs/s** | **0** |
| **Pre-TCP Combined** (total pure FIX engine round-trip budget) | **942.0 ns** | 1,091.0 ns | **1,442.0 ns** | **1,058,645 rt/s** | **0** |
| **Real TCP Loopback RTT** (kernel TCP + full FIX 4.4 + mmap journal) | **9.67 µs** | 10.67 µs | 13.28 µs | 103,837 rt/s | **0** |

---

## 3. Acceptor, `IoBackend` & Multi-Core `ShardedEngine` (`acceptor-bench`)

Reproduce (requires `--io_uring=y` for the `io_uring` row):
```sh
xmake f -m release --tls=y --xml=y --io_uring=y --benchmarks=y -y
xmake build acceptor-bench
./build/linux/x86_64/release/acceptor-bench 2000 20000 8 all
```

### Acceptor Tiers (`epoll` & `busy_poll`)
| Tier | p50 | p95 | p99 | Throughput |
| :--- | ---: | ---: | ---: | ---: |
| **Tier 1: Pre-TCP Acceptor** (`SessionDriver` wire-in -> 912-field Schema -> `send_stream` ER -> mmap journal -> flush) | **912 ns** | 962 ns | **1.13 µs** | **1,043,558 ord/s** |
| **Tier 2: 1-Session TCP (`epoll`)** (`Engine<fix44::Schema, Inline>`, 1 thread) | **11.19 µs** | 13.99 µs | 18.50 µs | **80,606 ord/s** |
| **Tier 2: 1-Session TCP (`busy_poll`)** (`Engine<fix44::Schema, Inline>`, 1 thread) | **9.35 µs** | 12.53 µs | **15.73 µs** | **94,160 ord/s** |
| **Tier 3: 8 Concurrent Sessions TCP (`epoll`)** (`Engine<fix44::Schema, Inline>`, 1 thread) | **32.45 µs** | 68.56 µs | 76.04 µs | **180,535 ord/s** |

### Multi-Core `ShardedEngine` Scaling (`acceptor-bench 1000 10000 8/16 sharded`)
*(Full 912-field `FIX44.xml` strict validation + `NoPartyIDs[1]` + `StreamWriter` `ExecutionReport` + real mmap journal, 1 shared listening port, strict 1-in-flight TCP ping-pong on 8C/16T host shared with client threads.)*

| Configuration | Sessions | p50 | p99 | p99.9 | Aggregate Throughput | Speedup vs 1-Core |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **1-core `Engine` (`epoll`)** | 8 | 32.01 µs | 75.70 µs | 233.57 µs | 180,564 ord/s | 1.00× |
| **4-shard `ShardedEngine` (`epoll`, pinned)** | 8 | 15.73 µs | 21.97 µs | 34.98 µs | 456,296 ord/s | **2.53×** (**3.45×** lower p99) |
| **4-shard `ShardedEngine` (`hybrid_backoff`, pinned)** | 8 | **15.52 µs** | **20.28 µs** | **30.32 µs** | **462,905 ord/s** | **2.56×** (**7.70×** lower p99.9) |
| **1-core `Engine` (`epoll`)** | 16 | 73.69 µs | 161.47 µs | 400.03 µs | 179,777 ord/s | 1.00× |
| **4-shard `ShardedEngine` (`epoll`, pinned)** | 16 | 29.99 µs | **48.17 µs** | 66.46 µs | 496,186 ord/s | **2.76×** (**3.35×** lower p99) |
| **4-shard `ShardedEngine` (`hybrid_backoff`, pinned)** | 16 | **29.82 µs** | 48.55 µs | **64.47 µs** | **503,230 ord/s** | **2.80×** (**6.20×** lower p99.9) |

---

## 4. NimbleFIX vs QuickFIX 1.16.0 — Same-Methodology Benchmark (`quickfix-parity-bench`)

Reproduce:
```sh
git submodule update --init --recursive
xmake f -m release --tls=y --xml=y --benchmarks=y --quickfix=y -y
xmake build quickfix-parity-bench
./build/linux/x86_64/release/quickfix-parity-bench 1000 5000 schemas/standard/FIX44.xml
```

### Apples-to-Apples Controls

| Dimension | NimbleFIX | QuickFIX 1.16.0 |
| :--- | :--- | :--- |
| **Binary / Compiler** | Single benchmark binary, GCC 16.2.1 `-O3` (`-std=c++20`) | Same binary, GCC 16.2.1 `-O3` (`-std=c++17`) |
| **Clock & Allocator Hook** | `CLOCK_MONOTONIC` + unified `-Wl,--wrap=malloc` & global `operator new/delete` | Identical clock & allocator hook |
| **FIX Dictionary** | Full 912-field `schemas/standard/FIX44.xml`, `ValidationLevel::strict` + 120 s `SendingTime` check | Same `schemas/standard/FIX44.xml`, `UseDataDictionary=Y`, strict validation + `MaxLatency=120` |
| **Order & Ack Payload** | `NewOrderSingle` (7 scalars + `NoPartyIDs[1]`: `448`/`447`/`452`) & 11-field `ExecutionReport` | Identical fields and repeating group |
| **Persistence** | Segmented `MAP_SHARED` mmap journal + SSE4.2 CRC32C (OS page-cache, no `fsync`) | `FIX::FileStore` (`PersistMessages=Y`, OS page-cache, no `fsync`) |
| **Logging** | Disabled | Disabled |
| **TCP Peer** | Neutral raw-TCP loopback peer (`127.0.0.1`, `TCP_NODELAY=1`) | Same peer |

### Measured Results (`1000` warmup / `5000` samples)

| Suite | NimbleFIX (p50 / p99 / alloc) | QuickFIX 1.16.0 (p50 / p99 / alloc) | Speedup (p50) |
| :--- | :--- | :--- | ---: |
| **Codec Encode** (`StreamWriter` / `Fixed`) | **150 ns** / **151 ns** (**0.00** alloc) · `Fixed` **331 ns** | 1,312 ns / 1,383 ns / 19.00 | **8.75×** / **3.96×** |
| **Parse + Full `FIX44.xml` Strict + Typed Read** | **361 ns** / **371 ns** / **0.00** | 2,464 ns / 2,495 ns / 15.00 | **6.83×** |
| **TCP Initiator Ping-Pong RTT** (`DirectInitiator`) | **8.85 µs** / **11.89 µs** / **0.00** (110k rt/s) | 21.58 µs / 29.90 µs / 35.06 (45k rt/s) | **2.44×** |
| **TCP Initiator Ping-Pong RTT** (`Engine busy_poll` / `epoll`) | **10.52 µs** / **16.66 µs** / **0.00** | 21.58 µs / 29.90 µs / 35.06 | **2.05×** / **1.30×** |
| **TCP Acceptor Turnaround RTT** (`Engine busy_poll` / `epoll`) | **9.45 µs** / **11.46 µs** (p99 **12.96 µs**, **0.00**) | 21.20 µs / 34.48 µs / 37.04 (44k ord/s) | **2.24×** / **1.85×** |
| **TCP Dense Burst 32×** (`enqueue_stream` + `sendmsg`, CO-Free 1-way) | **13.95 µs** / **35.88 µs** / **0.00** (**1.23M/s**) | 178.67 µs / 352.25 µs / 28.06 (92k/s) | **12.81×** (**13.3×** rate) |
