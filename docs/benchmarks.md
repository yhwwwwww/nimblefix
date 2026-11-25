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
| **1. `DirectInitiator::send_stream()`** (1-thread, no queue) | **4.69 µs** | **7.73 µs** | **8.86 µs** | **105,905 rt/s** |
| **2. `DirectInitiator::enqueue_stream()+flush()`** (1-thread `sendmsg`) | 5.42 µs | 9.39 µs | 10.39 µs | 88,421 rt/s |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`** (2-thread SPSC) | 5.27 µs | 8.72 µs | 9.09 µs | 101,836 rt/s |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`** (2-thread SPSC) | 7.69 µs | 12.07 µs | 12.27 µs | 74,338 rt/s |

#### Workload 2: Dense Burst — 0 µs Strategy Compute (200 bursts × 32 orders, 20 warmup bursts)

| Mode | CO-Free p50 | CO-Free p99 | Enq->Recv p50 | Throughput |
| :--- | ---: | ---: | ---: | ---: |
| **1. `DirectInitiator::send_stream()`** (1-thread, no queue) | 49.93 µs | 98.19 µs | 4.60 µs | 335,194 ord/s |
| **2. `DirectInitiator::enqueue_stream()+flush()`** (1-thread `sendmsg`) | 14.58 µs | **19.83 µs** | 11.26 µs | 1,274,415 ord/s |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`** (2-thread SPSC + `sendmsg`) | **12.86 µs** | 22.64 µs | 12.16 µs | **1,430,830 ord/s** |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`** (2-thread SPSC) | 67.28 µs | 117.28 µs | 53.81 µs | 277,608 ord/s |

#### Workload 3: Dense Burst + 2 µs Strategy Compute per Order (200 bursts × 32 orders)

| Mode | CO-Free p50 | CO-Free p99 | Enq->Recv p50 | Throughput |
| :--- | ---: | ---: | ---: | ---: |
| **1. `DirectInitiator::send_stream()`** (1-thread, no queue) | 86.87 µs | 228.77 µs | 5.55 µs | 179,146 ord/s |
| **2. `DirectInitiator::enqueue_stream()+flush()`** (1-thread `sendmsg`) | 80.85 µs | 217.01 µs | 45.87 µs | 320,174 ord/s |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`** (2-thread SPSC + `sendmsg`) | **42.62 µs** | **76.08 µs** | 7.42 µs | **421,837 ord/s** |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`** (2-thread SPSC) | 64.77 µs | 118.97 µs | 19.07 µs | 280,372 ord/s |

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

### Acceptor Tiers (`epoll`)
| Tier | p50 | p95 | p99 | Throughput |
| :--- | ---: | ---: | ---: | ---: |
| **Tier 1: Pre-TCP Acceptor** (`SessionDriver` wire-in -> 912-field Schema -> in-handler ER -> mmap journal -> flush) | **1.71 µs** | 1.83 µs | 3.03 µs | **581,156 ord/s** |
| **Tier 2: 1-Session TCP** (`Engine<fix44::Schema, Inline>`, 1 `epoll` thread) | **14.68 µs** | 18.59 µs | 32.07 µs | **62,553 ord/s** |
| **Tier 3: 8 Concurrent Sessions TCP** (`Engine<fix44::Schema, Inline>`, 1 `epoll` thread) | **44.56 µs** | 94.84 µs | 114.46 µs | **133,972 ord/s** |

### Same-Run `IoBackend` Comparison (`acceptor-bench 2000 20000 8 all`)

| `IoBackend` | 1-Session p50 | 1-Session p99 | 1-Session Rate | 8-Session p50 | 8-Session p99 | 8-Session Aggregate |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **`epoll`** (default) | 14.68 µs | 32.07 µs | 62,553 ord/s | **44.56 µs** | 114.46 µs | 133,972 ord/s |
| **`busy_poll`** (zero-sleep spin) | **12.45 µs** | 23.38 µs | **73,464 ord/s** | 45.38 µs | 118.44 µs | 129,649 ord/s |
| **`io_uring`** (Proactor `RECV`/`SEND`, sleeping) | 14.58 µs | **19.56 µs** | 66,515 ord/s | 49.10 µs | **97.90 µs** | **146,037 ord/s** (+9%) |

### Multi-Core `ShardedEngine` Scaling (`acceptor-bench 1000 10000 8/16 sharded`)
*(Full 912-field `FIX44.xml` strict validation + `NoPartyIDs[1]` + `StreamWriter` `ExecutionReport` + real mmap journal, 1 shared listening port, strict 1-in-flight TCP ping-pong on 8C/16T host shared with client threads.)*

| Configuration | Sessions | p50 | p99 | p99.9 | Aggregate Throughput | Speedup vs 1-Core |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **1-core `Engine` (`epoll`)** | 8 | 32.99 µs | 76.91 µs | 227.25 µs | 176,387 ord/s | 1.00× |
| **4-shard `ShardedEngine` (`epoll`, pinned)** | 8 | 16.58 µs | 22.15 µs | 31.21 µs | 433,318 ord/s | **2.46×** (**3.47×** lower p99) |
| **4-shard `ShardedEngine` (`hybrid_backoff`, pinned)** | 8 | **16.39 µs** | **21.27 µs** | **28.98 µs** | **443,302 ord/s** | **2.51×** (**7.84×** lower p99.9) |
| **1-core `Engine` (`epoll`)** | 16 | 75.64 µs | 161.11 µs | 346.42 µs | 176,184 ord/s | 1.00× |
| **4-shard `ShardedEngine` (`epoll`, pinned)** | 16 | 31.41 µs | 51.06 µs | 112.79 µs | 466,617 ord/s | **2.65×** (**3.15×** lower p99) |
| **4-shard `ShardedEngine` (`hybrid_backoff`, pinned)** | 16 | **31.19 µs** | **49.99 µs** | **96.74 µs** | **470,973 ord/s** | **2.67×** (**3.22×** lower p99) |

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
| **Codec Encode** (`StreamWriter` / `Fixed`) | **170 ns** / **181 ns** (**0.00** alloc) · `Fixed` **371 ns** | 1,463 ns / 1,523 ns / 19.00 | **8.61×** / **3.94×** |
| **Parse + Full `FIX44.xml` Strict + Typed Read** | **421 ns** / **431 ns** / **0.00** | 2,795 ns / 3,386 ns / 15.00 | **6.64×** |
| **TCP Initiator Ping-Pong RTT** (`DirectInitiator`) | **10.21 µs** / **17.12 µs** / **0.00** (92k rt/s) | 23.72 µs / 34.56 µs / 35.06 (41k rt/s) | **2.32×** |
| **TCP Initiator Ping-Pong RTT** (`Engine busy_poll` / `epoll`) | **14.39 µs** / **19.16 µs** / **0.00** | 23.72 µs / 34.56 µs / 35.06 | **1.65×** / **1.24×** |
| **TCP Acceptor Turnaround RTT** (`Engine busy_poll` / `epoll`) | **10.59 µs** / **11.57 µs** (p99 **14.33 µs**, **0.00**) | 23.20 µs / 33.92 µs / 37.00 (42k ord/s) | **2.19×** / **2.01×** |
| **TCP Dense Burst 32×** (`enqueue_stream` + `sendmsg`, CO-Free 1-way) | **12.63 µs** / **23.51 µs** / **0.00** (**1.34M/s**) | 198.47 µs / 399.32 µs / 28.06 (80k/s) | **15.71×** (**16.6×** rate) |
