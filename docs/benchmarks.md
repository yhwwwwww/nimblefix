# NimbleFIX Benchmarks

**English** | [简体中文](benchmarks.zh-CN.md)

All measurements below were collected on the same machine in Release mode with real segmented mmap journal persistence (hardware SSE4.2 CRC32C, no in-memory stub store) and unified thread-safe allocation auditing (`-Wl,--wrap={malloc,calloc,realloc,aligned_alloc,posix_memalign}` plus global `operator new/delete`, including C++17 `std::align_val_t` overloads).

> Related docs: [User Guide](user-guide.md) · [Architecture](architecture.md) · [Support Matrix](session-support.md)

## Test Environment

| Item | Value |
| :--- | :--- |
| CPU | AMD Ryzen 7 7840HS (8C / 16T) |
| OS / Kernel | Linux 7.2 (`x86_64`, loopback TCP with `TCP_NODELAY`) |
| Compiler | GCC 16.2.1 (`cmake -DCMAKE_BUILD_TYPE=Release`, `-O3`) |
| Schema | Full standard **FIX 4.4** (`schemas/standard/FIX44.xml`, 912 fields, strict Header/Trailer/Body validation) |
| Persistence | Real segmented `MAP_SHARED` mmap journal + hardware SSE4.2 CRC32C (`appended` confirmation) |
| Clock | `CLOCK_MONOTONIC` (`clock_gettime`) in user space |
| Allocation Audit | Shared `bench/alloc_audit.hpp`: atomic counters wrapping `malloc`, `calloc`, `realloc`, `aligned_alloc`, `posix_memalign`, and global `operator new/delete` (covers executable and all statically linked libraries, including NimbleFIX and bundled QuickFIX; internal allocations inside dynamically linked system libraries that bypass global `operator new` are outside `--wrap` scope) |

> **Boundary Note**: Loopback TCP measurements include full Linux kernel TCP/IP stack traversal (`send`/`sendmsg` + `recv`). Pre-TCP measurements isolate pure NimbleFIX codec, schema validation, session state machine, and mmap journal cost. In long multi-segment Acceptor runs (≥20,000 orders), crossing each 4 MiB journal segment boundary triggers an off-order atomic `.ckp` checkpoint file write (~4 small allocations per 4 MiB segment, or `0.0004 alloc/order`); the steady-state per-message path performs **0** heap allocations.

---

## 1. Burst, Open-Loop Schedule & One-Way Timestamp Benchmark (`initiator-burst-bench`)

Reproduce:
```sh
cmake --build build-cmake --target initiator-burst-bench -j
./build-cmake/initiator-burst-bench
```

### Part 0: Message Construction + FIX 4.4 Encode
*30,000 samples (5,000 warmup), alternating Market (`40=1`, no `Price`) and Limit (`40=2`, with `Price=150.25`) `NewOrderSingle`.*

| Message API | p50 | p99 | Heap Allocs |
| :--- | ---: | ---: | :---: |
| **Streaming**: `encode_fix44_stream<NewOrderSingle>` + native `if` | **121.0 ns** | **131.0 ns** | **0** |
| **Streaming**: `encode_fix44_stream<NewOrderSingle>` + `std::optional` | **121.0 ns** | **150.0 ns** | **0** |
| **Fixed**: `NewOrderSingle::Fixed<128, fix::field::...>` | **211.0 ns** | **250.0 ns** | **0** |

### Workloads 1–4: Initiator Send Mode Comparison (Loopback TCP + FIX 4.4 + mmap Journal)

Each order embeds `<t_burst_ns>-<t_send_ns>` in `ClOrdID (11)`. The receiving peer timestamps `t_recv_ns = now_ns()` immediately on `::recv()` and reports:
- **Burst->Recv (`t_recv - t_burst_start`)** *(Workloads 2 & 3)*: Intra-burst simultaneous-arrival one-way latency from batch start until the counterparty receives order `i` (batches wait for peer drain between bursts so bursts do not accumulate across batch boundaries).
- **Enq->Recv (`t_recv - t_send_stamp`)**: Per-order handoff-to-recv latency (useful for diagnosing transport queueing, though it hides head-of-line blocking when the producer blocks on I/O).
- **OpenLoop (`t_recv - t_sched`)** *(Workload 4)*: True Coordinated-Omission-Free exogenous latency driven by an independent fixed arrival schedule (`t_sched = t_origin + b * 500 µs`, **no** inter-batch drain wait), capturing both intra-burst queueing and any cross-burst backlog carry-over.

#### Workload 1: Ping-Pong (1 in-flight, 5,000 orders, 500 warmup)

| Mode | 1-Way p50 | 1-Way p99 | RTT p50 / p99 | Throughput | Heap Allocs |
| :--- | ---: | ---: | ---: | ---: | :---: |
| **1. `DirectInitiator::send_stream()`** (1-thread, no queue) | **4.62 µs** | 7.04 µs | **8.58 µs** / 12.57 µs | 107,695 rt/s | **0** |
| **2. `DirectInitiator::enqueue_stream()+flush()`** (1-thread `sendmsg`) | 4.65 µs | 7.60 µs | 8.59 µs / 13.01 µs | 111,536 rt/s | **0** |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`** (2-thread SPSC) | 5.05 µs | **6.89 µs** | **8.58 µs** / **11.25 µs** | **114,535 rt/s** | **0** |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`** (2-thread SPSC) | 5.48 µs | 8.51 µs | 9.82 µs / 14.54 µs | 96,547 rt/s | **0** |

#### Workload 2: Single-Batch Simultaneous Burst — 0 µs Strategy Compute (200 bursts × 32 orders, inter-batch drain)

| Mode | Burst->Recv p50 | Burst->Recv p99 | Enq->Recv p50 | Throughput | Heap Allocs |
| :--- | ---: | ---: | ---: | ---: | :---: |
| **1. `DirectInitiator::send_stream()`** (1-thread, no queue) | 49.30 µs | 95.30 µs | 4.87 µs | 344,234 ord/s | **0** |
| **2. `DirectInitiator::enqueue_stream()+flush()`** (1-thread `sendmsg`) | **12.56 µs** | **17.05 µs** | 9.84 µs | **1,435,952 ord/s** | **0** |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`** (2-thread SPSC + `sendmsg`) | 12.93 µs | 19.27 µs | 12.27 µs | 1,394,570 ord/s | **0** |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`** (2-thread SPSC + `sendmsg`) | 16.94 µs | 23.23 µs | 12.57 µs | 1,275,895 ord/s | **0** |

#### Workload 3: Single-Batch Simultaneous Burst + 2 µs Strategy Compute per Order (200 bursts × 32 orders, inter-batch drain)

| Mode | Burst->Recv p50 | Burst->Recv p99 | Enq->Recv p50 | Throughput | Heap Allocs |
| :--- | ---: | ---: | ---: | ---: | :---: |
| **1. `DirectInitiator::send_stream()`** (1-thread, no queue) | 87.51 µs | 191.76 µs | 5.51 µs | 188,951 ord/s | **0** |
| **2. `DirectInitiator::enqueue_stream()+flush()`** (1-thread `sendmsg`) | 78.31 µs | 83.39 µs | 42.18 µs | 361,942 ord/s | **0** |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`** (2-thread SPSC + `sendmsg`) | **41.34 µs** | **75.53 µs** | 6.49 µs | **427,094 ord/s** | **0** |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`** (2-thread SPSC + `sendmsg`) | 43.83 µs | 81.11 µs | **6.35 µs** | 398,380 ord/s | **0** |

#### Workload 4: True Open-Loop Scheduled Bursts (200 bursts × 32 orders @ 500 µs/burst = 64,000 target ord/s, NO inter-batch wait)

| Mode | OpenLoop p50 | OpenLoop p99 | OpenLoop max | Achieved Rate | Backpressure Retries | Heap Allocs |
| :--- | ---: | ---: | ---: | ---: | :---: | :---: |
| **1. `DirectInitiator::send_stream()`** (1-thread, no queue) | 49.59 µs | 96.40 µs | 113.99 µs | 64,261 ord/s | 0 | **0** |
| **2. `DirectInitiator::enqueue_stream()+flush()`** (1-thread `sendmsg`) | **14.95 µs** | **18.89 µs** | **20.00 µs** | 64,303 ord/s | 0 | **0** |
| **3. `DirectInitiator + DirectSendQueue::try_push_stream()`** (2-thread SPSC + `sendmsg`) | 15.86 µs | 24.11 µs | 85.69 µs | 64,302 ord/s | 0 | **0** |
| **4. `Engine(busy_poll) + ProducerLane::send_stream()`** (2-thread SPSC + `sendmsg`) | 17.67 µs | 40.04 µs | 48.84 µs | 64,302 ord/s | 0 | **0** |

---

## 2. `DirectInitiator` Layered Breakdown (`direct-initiator-bench`)

Reproduce:
```sh
cmake --build build-cmake --target direct-initiator-bench -j
./build-cmake/direct-initiator-bench 5000 30000
```

Workload: Full 912-field FIX 4.4 `NewOrderSingle` (7 scalars including `Price` + `NoPartyIDs[1]` with 3 party fields `448`/`447`/`452`, populated inside the timed loop) and 11-field `ExecutionReport`, with segmented mmap journal + SSE4.2 CRC32C.

| Boundary | p50 | p95 | p99 | Throughput | Heap Allocs |
| :--- | ---: | ---: | ---: | ---: | :---: |
| **User Encode (`StreamWriter`)** (`encode_fix44_stream`, no session/journal) | **110.0 ns** | 111.0 ns | **120.0 ns** | **9,066,848 msgs/s** | **0** |
| **User Encode (`Fixed`)** (in-loop fill + `encode_fix44`, no session/journal) | 300.0 ns | 431.0 ns | 481.0 ns | 3,217,353 msgs/s | **0** |
| **Pre-TCP Outbound** (`DirectInitiator::send_stream` + mmap journal) | **220.0 ns** | 241.0 ns | **341.0 ns** | **4,432,308 msgs/s** | **0** |
| **Pre-TCP Inbound** (frame + parse + 912-field Schema + dual journal records + callback) | **662.0 ns** | 701.0 ns | **941.0 ns** | **1,466,009 msgs/s** | **0** |
| **Pre-TCP Combined** (total pure FIX engine round-trip budget) | **881.0 ns** | 941.0 ns | **1,252.0 ns** | **1,101,637 rt/s** | **0** |
| **Real TCP Loopback RTT** (kernel TCP + full FIX 4.4 + mmap journal) | **8.63 µs** | 10.57 µs | **12.20 µs** | **111,166 rt/s** | **0** |

---

## 3. Acceptor, `IoBackend` & Multi-Core `ShardedEngine` (`acceptor-bench`)

Reproduce (requires `-DNIMBLEFIX_ENABLE_IO_URING=ON` or `--io_uring=y` for the `io_uring` rows):
```sh
cmake -S . -B build-uring -DCMAKE_BUILD_TYPE=Release -DNIMBLEFIX_ENABLE_IO_URING=ON \
  -DNIMBLEFIX_BUILD_STANDARD_SCHEMAS=ON -DNIMBLEFIX_BUILD_BENCHMARKS=ON
cmake --build build-uring --target acceptor-bench -j
./build-uring/acceptor-bench 2000 20000 8 all
./build-uring/acceptor-bench 1000 10000 8 sharded
./build-uring/acceptor-bench 1000 10000 16 sharded
```

### Acceptor Tiers & Single-Core `IoBackend` Comparison (`epoll` / `busy_poll` / `io_uring`)

| Tier / `IoBackend` | 1-Session p50 / p99 | 1-Session Rate (Allocs / 20k) | 8-Session p50 / p99 | 8-Session Aggregate (Allocs / 80k) |
| :--- | ---: | ---: | ---: | ---: |
| **Tier 1: Pre-TCP Acceptor** (`SessionDriver`, 912-field Schema + `send_stream` ER + mmap journal + flush) | **842 ns** / **1.02 µs** *(cool `epoll` build)* · 962 ns (`all` run) | **1,123,600 ord/s** (8*) | — | — |
| **Tier 2/3: `epoll`** (default, sleeping) | 10.88 µs / 14.54 µs *(cool)* · 14.95 µs (`all`) | 85,888 ord/s (8*) | 29.68 µs / 69.18 µs *(cool)* · 38.39 µs (`all`) | 200,699 ord/s *(cool)* · 156,313 ord/s (`all`, 32–36*) |
| **Tier 2/3: `busy_poll`** (zero-sleep spin) | **11.56 µs** / **17.75 µs** (`all`) | **81,635 ord/s** (8*) | **37.96 µs** / 93.40 µs (`all`) | 159,385 ord/s (`all`, 36*) |
| **Tier 2/3: `io_uring`** (two-phase Proactor `RECV`/`SEND`, sleeping) | 14.00 µs / 21.94 µs (`all`) | 65,621 ord/s (8*) | 45.65 µs / **83.71 µs** (`all`) | **164,184 ord/s** (`all`, 32*) |

*\* Measured over the entire steady-state window: 0 per-order allocations; the 8 / 32–36 total allocations (~4 per 4 MiB segment crossing, `0.0004/order`) come exclusively from periodic atomic `.ckp` checkpoint / segment-rotation maintenance.*

### Multi-Core `ShardedEngine` Scaling (`acceptor-bench 1000 10000 8/16 sharded`)
*(Full 912-field `FIX44.xml` strict validation + `NoPartyIDs[1]` + `StreamWriter` `ExecutionReport` + real mmap journal, 1 shared listening port, strict 1-in-flight TCP ping-pong on 8C/16T host shared with client threads. All rows within each session count are captured in the same run.)*

| Configuration | Sessions | p50 | p99 | p99.9 | Aggregate Throughput | Speedup vs 1-Core | Total Allocs* |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| **1-core `Engine` (`epoll`)** | 8 | 39.79 µs | 113.57 µs | 357.34 µs | 150,951 ord/s | 1.00× | 36 |
| **4-shard `ShardedEngine` (`epoll`, pinned)** | 8 | 19.32 µs | 30.63 µs | 64.06 µs | 383,488 ord/s | **2.54×** (**3.71×** lower p99) | 32 |
| **4-shard `ShardedEngine` (`epoll` + `hybrid_backoff`, pinned)** | 8 | 18.97 µs | 25.76 µs | **43.29 µs** | 396,827 ord/s | **2.63×** (**4.41×** lower p99) | 32 |
| **4-shard `ShardedEngine` (`io_uring` + `hybrid_backoff`, pinned)** | 8 | **18.31 µs** | **25.45 µs** | 44.36 µs | **407,095 ord/s** | **2.70×** (**4.46×** lower p99) | 32 |
| **1-core `Engine` (`epoll`)** | 16 | 89.97 µs | 214.17 µs | 338.46 µs | 148,186 ord/s | 1.00× | 64 |
| **4-shard `ShardedEngine` (`epoll`, pinned)** | 16 | 36.60 µs | 59.00 µs | 105.12 µs | 425,861 ord/s | **2.87×** (**3.63×** lower p99) | 64 |
| **4-shard `ShardedEngine` (`epoll` + `hybrid_backoff`, pinned)** | 16 | 36.19 µs | 57.93 µs | **89.31 µs** | 428,623 ord/s | **2.89×** (**3.70×** lower p99) | 68 |
| **4-shard `ShardedEngine` (`io_uring` + `hybrid_backoff`, pinned)** | 16 | **34.77 µs** | **52.17 µs** | 99.78 µs | **446,798 ord/s** | **3.02×** (**4.11×** lower p99) | 64 |

*\* Over 80,000 orders (8 sessions) / 160,000 orders (16 sessions): `0.0004 alloc/order` from 1 segment checkpoint rotation per session; 0 per-order hot-path allocations.*

> **Selection Guide**: `epoll` is the zero-dependency default; `busy_poll` minimizes 1-session RTT on a dedicated core; `io_uring` batches async `RECV`/`SEND` syscalls with two-phase send completion commit (`stage_tx` -> `commit_tx_written`) to boost multi-session throughput and cut p99 tail latency without spinning when idle (and supports `IdleStrategy::hybrid_backoff` with `IORING_SETUP_TASKRUN_FLAG` for the highest multi-core `ShardedEngine` throughput).

---

## 4. NimbleFIX vs QuickFIX 1.16.0 — Same-Methodology Benchmark (`quickfix-parity-bench`)

Reproduce:
```sh
git submodule update --init --recursive
cmake --build build-cmake --target quickfix-parity-bench -j
./build-cmake/quickfix-parity-bench 1000 5000 schemas/standard/FIX44.xml
```

### Apples-to-Apples Controls

| Dimension | NimbleFIX | QuickFIX 1.16.0 |
| :--- | :--- | :--- |
| **Binary / Compiler** | Single benchmark binary, GCC 16.2.1 `-O3` (`-std=c++20`) | Same binary, GCC 16.2.1 `-O3` (`-std=c++17`) |
| **Clock & Allocator Hook** | `CLOCK_MONOTONIC` + unified `-Wl,--wrap={malloc,calloc,realloc,aligned_alloc,posix_memalign}` & global `operator new/delete` | Identical clock & allocator hook |
| **FIX Dictionary & Parity Gate** | Full 912-field `schemas/standard/FIX44.xml`, `ValidationLevel::strict` (Header + Trailer + Body + section order, pre-timing 12-case accept/reject equivalence gate) + `check_sending_time = true` (`120 s` skew on both `DirectInitiator` and `Engine`) | Same `schemas/standard/FIX44.xml`, `UseDataDictionary=Y`, strict validation + `CheckLatency=Y`, `MaxLatency=120` |
| **Order & Ack Payload** | `NewOrderSingle` (7 scalars + `NoPartyIDs[1]`: `448`/`447`/`452`) & 11-field `ExecutionReport` | Identical fields and repeating group |
| **Persistence** | Segmented `MAP_SHARED` mmap journal + SSE4.2 CRC32C (OS page-cache, no `fsync`) | `FIX::FileStore` (`PersistMessages=Y`, OS page-cache, no `fsync`) |
| **Logging** | Disabled | Disabled |
| **TCP Peer** | Neutral raw-TCP loopback peer (`127.0.0.1`, `TCP_NODELAY=1`) | Same peer |

### Measured Results (`1000` warmup / `5000` samples)

| Suite | NimbleFIX (p50 / p99 / alloc) | QuickFIX 1.16.0 (p50 / p99 / alloc) | Speedup (p50) |
| :--- | :--- | :--- | ---: |
| **Suite 1: Codec Encode** (`StreamWriter`) | **150 ns** / **161 ns** / **0.00** (6.52M/s) | 1,172 ns / 1,222 ns / 19.00 (849k/s) | **7.81×** |
| **Suite 1: Codec Encode** (`Fixed`) | **301 ns** / **531 ns** / **0.00** (3.07M/s) | 1,172 ns / 1,222 ns / 19.00 (849k/s) | **3.89×** |
| **Suite 2: Parse + Full `FIX44.xml` Strict + Typed Read** | **411 ns** / **421 ns** / **0.00** (2.44M/s) | 2,345 ns / 3,858 ns / 15.00 (411k/s) | **5.71×** |
| **Suite 3: TCP Initiator Ping-Pong RTT** (`DirectInitiator`) | **8.78 µs** / **12.15 µs** / **0.00** (111k rt/s) | 21.26 µs / 35.74 µs / 35.06 (44.4k rt/s) | **2.42×** |
| **Suite 3: TCP Initiator Ping-Pong RTT** (`Engine busy_poll`) | **10.58 µs** / **15.85 µs** / **0.00** (89.4k rt/s) | 21.26 µs / 35.74 µs / 35.06 (44.4k rt/s) | **2.01×** |
| **Suite 3: TCP Initiator Ping-Pong RTT** (`Engine epoll`) | **16.54 µs** / **21.32 µs** / **0.00** (59.1k rt/s) | 21.26 µs / 35.74 µs / 35.06 (44.4k rt/s) | **1.29×** |
| **Suite 4: TCP Acceptor Turnaround RTT** (`Engine busy_poll`) | **9.39 µs** / **13.29 µs** / **0.00** (95.9k ord/s) | 20.74 µs / 32.16 µs / 36.58 (45.5k ord/s) | **2.21×** |
| **Suite 4: TCP Acceptor Turnaround RTT** (`Engine epoll`) | **11.41 µs** / **17.86 µs** / **0.00** (80.3k ord/s) | 20.74 µs / 32.16 µs / 36.58 (45.5k ord/s) | **1.82×** |
| **Suite 5: Single-Batch 32-Order Burst** (`DirectInitiator::send()`, `Burst->Recv`) | **49.89 µs** / **99.76 µs** / **0.00** (338k ord/s) | 178.04 µs / 340.85 µs / 28.06 (94.0k ord/s) | **3.57×** |
| **Suite 5: Single-Batch 32-Order Burst** (`enqueue_stream` + `sendmsg`, `Burst->Recv`) | **12.50 µs** / **17.99 µs** / **0.00** (**1.41M ord/s**) | 178.04 µs / 340.85 µs / 28.06 (94.0k ord/s) | **14.24×** (**15.0×** rate) |
