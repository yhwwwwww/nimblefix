# NimbleFIX 0.3 Benchmarks

All measurements below were collected on the same machine in Release mode with real **segmented mmap Journal + SSE4.2 hardware CRC32C** persistence (no in-memory stub store) and `-Wl,--wrap=malloc` allocation auditing on hot paths.

## Test Environment

| Item | Value |
| :--- | :--- |
| CPU | AMD Ryzen 7 7840HS (8C / 16T) |
| OS / Kernel | Linux 7.2 (`x86_64`, loopback TCP with `TCP_NODELAY`) |
| Compiler | GCC 16.2.1 (`xmake f -m release --tls=y --xml=y --benchmarks=y`, `-O3`) |
| Schema | Full standard **FIX 4.4** (`schemas/standard/FIX44.xml`, 912 fields) |
| Persistence | Real segmented `MAP_SHARED` mmap Journal + hardware SSE4.2 CRC32C (`appended` confirmation) |
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
| **Strict**: `NewOrderSingle::Fixed<128, fix::field::...>` | **220.0 ns** | **391.0 ns** | **0** |

**Takeaway**: Zero-intermediate-copy streaming (`StreamWriter`) achieves **130.0 ns p50** with **0 heap allocations** (including full FIX 4.4 enum/timestamp/required-field validation, `BodyLength` backpatch, and `CheckSum`), outperforming even pre-shaped `Fixed<128, ...>` by **1.69×** while supporting arbitrary native `if`/`for`, `std::optional`, dynamic `w.header()`, and nested repeating groups.

### Part 1–3: Initiator Send Mode Comparison (Real Loopback TCP + FIX 4.4 + mmap Journal)

Each order embeds `<t_burst_ns>-<t_send_ns>` in `ClOrdID (11)`. The receiving peer timestamps `t_recv_ns = now_ns()` immediately on `::recv()` and reports two one-way metrics:
- **CO-Free (`t_recv - t_burst_start`)**: **Coordinated-Omission-Free latency** — true wall time from the moment the burst signal arrived until the counterparty received order `i`.
- **Enq->Recv (`t_recv - t_send_stamp`)**: Per-order handoff-to-recv latency (useful for diagnosing transport queueing, but **hides head-of-line blocking** when the producer thread blocks on I/O).

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

### Engineering Interpretation

1. **Single-order / Ping-Pong**: `DirectInitiator::send_stream()` wins single-order 1-way p50 (**4.69 µs**, **8.86 µs RTT**, **105.9k rt/s**) with zero cross-thread handoff; `DirectSendQueue::try_push_stream()` sits at **9.09 µs RTT** and is **2.4 µs faster** on 1-way p50 than full multi-session `Engine + ProducerLane` (7.69 µs).
2. **Why `Enq->Recv` alone is misleading (Coordinated Omission)**: In Workload 3, Mode 1 (`DirectInitiator::send_stream()`) shows `Enq->Recv p50 = 5.55 µs`, yet its true `CO-Free p50` is **86.87 µs** because every synchronous `::send()` stalls the strategy thread before it can compute order `i+1`.
3. **Burst without strategy compute (Workload 2)**: Coalescing 32 orders into 1 `sendmsg()` (`enqueue_stream()+flush()`) cuts `CO-Free p50` from **49.93 µs -> 14.58 µs (3.42×)** and pushes throughput to **1.27M ord/s**; adding the 2-thread SPSC `DirectSendQueue::try_push_stream()` pushes throughput to **1.43M ord/s** and `CO-Free p50` down to **12.86 µs**.
4. **Burst with strategy compute (Workload 3)**: With 32 × 2 µs = 64 µs of strategy compute per burst, 2-thread SPSC pipelining (`DirectSendQueue`) overlaps strategy computation with journal + TCP I/O on a second core, cutting `CO-Free p50` in half (**86.87 µs -> 42.62 µs**, **p99 = 76.08 µs**, **422k ord/s**).

---

## 2. `DirectInitiator` Layered Breakdown (`direct-initiator-bench`)

Reproduce:
```sh
xmake build direct-initiator-bench
./build/linux/x86_64/release/direct-initiator-bench 5000 30000
```

Workload: Full 912-field FIX 4.4 `NewOrderSingle` (7 scalars including `Price` + 3-field `NoPartyIDs` repeating group, populated inside the timed loop) and 11-field `ExecutionReport`, with real segmented mmap Journal + SSE4.2 CRC32C.

| Boundary | p50 | p95 | p99 | Throughput | Heap Allocs |
| :--- | ---: | ---: | ---: | ---: | :---: |
| **User Encode (`StreamWriter`)** (`encode_fix44_stream`, no session/journal) | **140.0 ns** | 150.0 ns | **151.0 ns** | **7,089,686 msgs/s** | **0** |
| **User Encode (`Fixed`)** (in-loop fill + `encode_fix44`, no session/journal) | 351.0 ns | 391.0 ns | 411.0 ns | 2,804,887 msgs/s | **0** |
| **Pre-TCP Outbound** (`DirectInitiator::send_stream` + mmap Journal) | **211.0 ns** | 280.0 ns | **421.0 ns** | **4,420,157 msgs/s** | **0** |
| **Pre-TCP Inbound** (frame + parse + 912-field Schema + dual Journal records + callback) | **721.0 ns** | 822.0 ns | **1,062.0 ns** | **1,392,046 msgs/s** | **0** |
| **Pre-TCP Combined** (total pure FIX engine round-trip budget) | **942.0 ns** | 1,091.0 ns | **1,442.0 ns** | **1,058,645 rt/s** | **0** |
| **Real TCP Loopback RTT** (kernel TCP + full FIX 4.4 + mmap Journal) | **9.67 µs** | 10.67 µs | 13.28 µs | 103,837 rt/s | **0** |

---

## 3. Acceptor & `IoBackend` Comparison (`acceptor-bench`)

Reproduce (requires `--io_uring=y` for the `io_uring` row):
```sh
xmake f -m release --tls=y --xml=y --io_uring=y --benchmarks=y -y
xmake build acceptor-bench
./build/linux/x86_64/release/acceptor-bench 2000 20000 8 all
```

### Acceptor Tiers (`epoll`)
| Tier | p50 | p95 | p99 | Throughput |
| :--- | ---: | ---: | ---: | ---: |
| **Tier 1: Pre-TCP Acceptor** (`SessionDriver` wire-in -> 912-field Schema -> in-handler ER -> mmap Journal -> flush) | **1.71 µs** | 1.83 µs | 3.03 µs | **581,156 ord/s** |
| **Tier 2: 1-Session TCP** (`Engine<fix44::Schema, Inline>`, 1 `epoll` thread) | **14.68 µs** | 18.59 µs | 32.07 µs | **62,553 ord/s** |
| **Tier 3: 8 Concurrent Sessions TCP** (`Engine<fix44::Schema, Inline>`, 1 `epoll` thread) | **44.56 µs** | 94.84 µs | 114.46 µs | **133,972 ord/s** |

### Same-Run `IoBackend` Head-to-Head (`acceptor-bench 2000 20000 8 all`)
*(Executed back-to-back in a single process under identical clock/thermal state.)*

| `IoBackend` | 1-Session p50 | 1-Session p99 | 1-Session Rate | 8-Session p50 | 8-Session p99 | 8-Session Aggregate |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **`epoll`** (default) | 14.68 µs | 32.07 µs | 62,553 ord/s | **44.56 µs** | 114.46 µs | 133,972 ord/s |
| **`busy_poll`** (zero-sleep spin) | **12.45 µs** | 23.38 µs | **73,464 ord/s** | 45.38 µs | 118.44 µs | 129,649 ord/s |
| **`io_uring`** (Proactor `RECV`/`SEND`, sleeping) | 14.58 µs | **19.56 µs** | 66,515 ord/s | 49.10 µs | **97.90 µs** | **146,037 ord/s** (+9%) |

**Selection Guide**:
- **`epoll`**: Default zero-dependency choice for moderate session counts and low idle CPU.
- **`busy_poll`**: Dedicated isolated core with few sessions needing minimum 1-session p50 (`12.45 µs`, `73.5k ord/s`).
- **`io_uring`**: Multi-session gateways where Proactor batching cuts syscall count, delivering the highest 8-session aggregate throughput (**146,037 ord/s**) and lowest p99 on both 1-session (**19.56 µs**, 1.64× lower than `epoll`) and 8-session (**97.90 µs**) without burning 100% CPU when idle.

---

## 4. NimbleFIX 0.3 vs QuickFIX 1.16.0 — Same-Methodology Benchmark (`quickfix-parity-bench`)

QuickFIX `v1.16.0` is vendored as a git submodule at `third_party/quickfix` (built out-of-source as a static library so the submodule tree stays clean).

Reproduce (xmake):
```sh
git submodule update --init --recursive
xmake f -m release --tls=y --xml=y --benchmarks=y --quickfix=y -y
xmake build quickfix-parity-bench
./build/linux/x86_64/release/quickfix-parity-bench 1000 5000 schemas/standard/FIX44.xml
```

Reproduce (CMake):
```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release \
  -DNIMBLEFIX_BUILD_BENCHMARKS=ON -DNIMBLEFIX_ENABLE_QUICKFIX=ON
cmake --build build-cmake --target quickfix-parity-bench -j
./build-cmake/quickfix-parity-bench 1000 5000 schemas/standard/FIX44.xml
```

### Parity Disclosure (Apples-to-Apples Controls)

| Dimension | NimbleFIX 0.3 | QuickFIX 1.16.0 |
| :--- | :--- | :--- |
| **Binary / Compiler** | Single benchmark binary, GCC 16.2.1 `-O3` (`-std=c++20`) | Same binary, GCC 16.2.1 `-O3` (`-std=c++17`, `quickfix-bundled`) |
| **Clock & Allocator Hook** | `CLOCK_MONOTONIC` + unified `-Wl,--wrap=malloc` & global `operator new/delete` | Identical clock & allocator hook |
| **FIX Dictionary** | Full 912-field `schemas/standard/FIX44.xml`, `ValidationLevel::strict` + 120 s `SendingTime` check | Same `schemas/standard/FIX44.xml`, `UseDataDictionary=Y`, `ValidateFieldsOutOfOrder/HaveValues/UserDefinedFields=Y`, `CheckLatency=Y`, `MaxLatency=120` |
| **Order & Ack Payload** | `NewOrderSingle` (7 scalars + `NoPartyIDs[1]`: `448`/`447`/`452`, dynamic `ClOrdID`) & 11-field `ExecutionReport` populated in callback; cross-validated by QuickFIX before timing | Identical fields and repeating group; cross-validated by NimbleFIX before timing |
| **Persistence Tier** | Real segmented `MAP_SHARED` mmap Journal + hardware SSE4.2 CRC32C (OS page-cache, no `fsync`) | Real `FIX::FileStore` (`PersistMessages=Y`, OS page-cache, no `fsync`) |
| **Logging** | Disabled | Disabled (no `FileLog` / `ScreenLog`) |
| **TCP Peer** | Identical neutral raw-TCP loopback peer (`127.0.0.1`, `TCP_NODELAY=1`) | Same peer |

### Measured Results (`quickfix-parity-bench 1000 5000 schemas/standard/FIX44.xml`)

#### Suite 1: Codec Encode (`NewOrderSingle` + `NoPartyIDs[1]` + Session Header & `CheckSum`, 5,000 samples)
| Engine / API | p50 | p99 | Rate | Heap Allocs / op | Speedup (p50) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| **NimbleFIX `StreamWriter` + `PartyGroup` (0-alloc)** | **170.0 ns** | **181.0 ns** | **5,779,304/s** | **0.00** | **8.61×** |
| **NimbleFIX `Fixed` + `PartyGroup` (0-alloc)** | **371.0 ns** | **421.0 ns** | **2,633,072/s** | **0.00** | **3.94×** |
| **QuickFIX `FIX44::NewOrderSingle::toString`** | 1,463.0 ns | 1,523.0 ns | 679,232/s | 19.00 | 1.00× |

#### Suite 2: Parse + Full `FIX44.xml` Strict Validation + Typed Field & Group Read (5,000 samples)
| Engine | p50 | p99 | Rate | Heap Allocs / op | Speedup (p50) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| **NimbleFIX `parse` + `FIX44` strict + O(1) indexed view** | **421.0 ns** | **431.0 ns** | **2,357,773/s** | **0.00** | **6.64×** |
| **QuickFIX `setString` + `FIX44` validate + `get`** | 2,795.0 ns | 3,386.0 ns | 352,979/s | 15.00 | 1.00× |

#### Suite 3: Real Loopback TCP Initiator Ping-Pong RTT (5,000 orders, 1,000 warmup)
| Engine / Mode | p50 | p99 | Throughput | Heap Allocs / op | Speedup (p50) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| **NimbleFIX `DirectInitiator` (`send_stream`, Layer 0)** | **10.21 µs** | **17.12 µs** | **92,236 rt/s** | **0.00** | **2.32×** |
| **NimbleFIX `Engine` (`busy_poll`)** | 14.39 µs | 18.33 µs | 71,298 rt/s | **0.00** | 1.65× |
| **NimbleFIX `Engine` (`epoll`)** | 19.16 µs | 29.70 µs | 50,588 rt/s | **0.00** | 1.24× |
| **QuickFIX `SocketInitiator` (`FileStore`)** | 23.72 µs | 34.56 µs | 40,707 rt/s | 35.06 | 1.00× |

#### Suite 4: Real Loopback TCP Acceptor Turnaround RTT (5,000 orders, 1,000 warmup)
| Engine / Mode | p50 | p99 | Throughput | Heap Allocs / op | Speedup (p50) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| **NimbleFIX `Engine` Acceptor (`busy_poll`)** | **10.59 µs** | **14.33 µs** | **88,772 ord/s** | **0.00** | **2.19×** |
| **NimbleFIX `Engine` Acceptor (`epoll`)** | 11.57 µs | 17.61 µs | 76,687 ord/s | **0.00** | 2.01× |
| **QuickFIX `SocketAcceptor` (`FileStore`)** | 23.20 µs | 33.92 µs | 42,185 ord/s | 37.00 | 1.00× |

#### Suite 5: Open-Loop Dense Burst (156 bursts × 32 orders, CO-Free 1-Way Latency & Throughput)
| Engine / Mode | CO-Free 1-Way p50 | CO-Free 1-Way p99 | Throughput | Heap Allocs / op | Speedup (p50) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| **NimbleFIX `DirectInitiator::send_stream()`** | 59.25 µs | 114.14 µs | 284,662 ord/s | **0.00** | 3.35× |
| **NimbleFIX `DirectInitiator` `enqueue_stream` + `sendmsg`** | **12.63 µs** | **23.51 µs** | **1,338,302 ord/s** | **0.00** | **15.71×** |
| **QuickFIX `SocketInitiator::sendToTarget`** | 198.47 µs | 399.32 µs | 80,393 ord/s | 28.06 | 1.00× |

### Key Takeaways
1. **Zero steady-state heap traffic across ALL APIs**: Both `StreamWriter` (with dynamic repeating groups, `if`/`for`, and `std::optional`) and `Fixed` record **0.00 heap allocations/op** across all 5 suites (including full 912-field `FIX44.xml` validation and mmap journaling), whereas QuickFIX performs **15–37 heap allocations/op** (`std::map`/`std::string`/group nodes).
2. **Sub-microsecond codec in both directions**: `StreamWriter` cuts `NewOrderSingle + NoPartyIDs[1]` encode latency to **170 ns (8.61× faster than QuickFIX)**, while generated `switch(tag)` single-pass validation + generation-stamped O(1) `ParsedFieldIndex` cuts full 912-field strict parse + validate + typed read to **421 ns (6.64× faster than QuickFIX)** — bringing the entire Pre-TCP round-trip FIX engine budget to **942 ns (0.942 µs)**!
3. **Architecture scales when latency matters most**: Switching to single-threaded Layer 0 `DirectInitiator` achieves **~9.67–10.21 µs** Initiator ping-pong RTT (**2.32× faster than QuickFIX**), `Engine(busy_poll)` Acceptor hits **10.59 µs (2.19× faster)** with **14.33 µs p99**, and 32-order burst (`enqueue_stream + flush`) reaches **1.34M ord/s** with **12.63 µs CO-Free p50 (15.71× lower than QuickFIX)**.
