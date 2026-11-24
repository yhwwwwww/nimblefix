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

### Part 0: Message Construction + Single-Pass FIX 4.4 Encode
*30,000 samples (5,000 warmup), alternating Market (`39/40=1`, no `Price`) and Limit (`40=2`, with `Price=150.25`) `NewOrderSingle`.*

| Message API | p50 | p99 | Heap Allocs (`__wrap_malloc`) |
| :--- | ---: | ---: | :---: |
| **Scheme A**: `NewOrderSingle::make()` + `std::optional` (SSO `<16, 256>`) | **340.0 ns** | **390.0 ns** | **0** |
| **Scheme B**: `NewOrderSingle::Draft` + native `if` + `reset()` (SSO `<16, 256>`) | **321.0 ns** | **411.0 ns** | **0** |
| **Strict**: `NewOrderSingle::Fixed<128, fix::field::...>` | **221.0 ns** | **341.0 ns** | **0** |

**Takeaway**: Both user-friendly SSO APIs (`make()` and `Draft`) achieve **0 heap allocations** and sit within ~100–120 ns of the compile-time fixed-shape `Fixed<128, ...>` layout (~320–340 ns vs ~221 ns total including FIX 4.4 framing, `BodyLength` backpatch, and `CheckSum`).

### Part 1–3: Initiator Send Mode Comparison (Real Loopback TCP + FIX 4.4 + mmap Journal)

Each order embeds `<t_burst_ns>-<t_send_ns>` in `ClOrdID (11)`. The receiving peer timestamps `t_recv_ns = now_ns()` immediately on `::recv()` and reports two one-way metrics:
- **CO-Free (`t_recv - t_burst_start`)**: **Coordinated-Omission-Free latency** — true wall time from the moment the burst signal arrived until the counterparty received order `i`.
- **Enq->Recv (`t_recv - t_send_stamp`)**: Per-order handoff-to-recv latency (useful for diagnosing transport queueing, but **hides head-of-line blocking** when the producer thread blocks on I/O).

#### Workload 1: Ping-Pong (1 in-flight, 5,000 orders, 500 warmup)

| Mode | 1-Way p50 | 1-Way p99 | RTT p50 | Throughput |
| :--- | ---: | ---: | ---: | ---: |
| **1. `DirectInitiator::send()`** (1-thread, no queue) | **4.97 µs** | 9.34 µs | 9.62 µs | 97,276 rt/s |
| **2. `DirectInitiator::enqueue()+flush()`** (1-thread `sendmsg`) | 5.01 µs | **8.46 µs** | 9.67 µs | 97,474 rt/s |
| **3. `DirectInitiator + DirectSendQueue`** (2-thread SPSC) | 5.13 µs | 9.60 µs | **9.33 µs** | **97,996 rt/s** |
| **4. `Engine(busy_poll) + ProducerLane`** (2-thread SPSC) | 7.89 µs | 15.61 µs | 13.11 µs | 67,229 rt/s |

#### Workload 2: Dense Burst — 0 µs Strategy Compute (200 bursts × 32 orders, 20 warmup bursts)

| Mode | CO-Free p50 | CO-Free p99 | Enq->Recv p50 | Throughput |
| :--- | ---: | ---: | ---: | ---: |
| **1. `DirectInitiator::send()`** (1-thread, no queue) | 57.04 µs | 113.00 µs | 5.37 µs | 291,964 ord/s |
| **2. `DirectInitiator::enqueue()+flush()`** (1-thread `sendmsg`) | 22.73 µs | **30.50 µs** | 15.38 µs | 945,924 ord/s |
| **3. `DirectInitiator + DirectSendQueue`** (2-thread SPSC + `sendmsg`) | **18.14 µs** | 41.65 µs | 14.51 µs | **1,018,595 ord/s** |
| **4. `Engine(busy_poll) + ProducerLane`** (2-thread SPSC) | 76.31 µs | 155.31 µs | 58.94 µs | 248,563 ord/s |

#### Workload 3: Dense Burst + 2 µs Strategy Compute per Order (200 bursts × 32 orders)

| Mode | CO-Free p50 | CO-Free p99 | Enq->Recv p50 | Throughput |
| :--- | ---: | ---: | ---: | ---: |
| **1. `DirectInitiator::send()`** (1-thread, no queue) | 100.50 µs | 198.74 µs | 6.75 µs | 165,840 ord/s |
| **2. `DirectInitiator::enqueue()+flush()`** (1-thread `sendmsg`) | 95.40 µs | 130.63 µs | 55.64 µs | 282,802 ord/s |
| **3. `DirectInitiator + DirectSendQueue`** (2-thread SPSC + `sendmsg`) | **48.52 µs** | **88.70 µs** | 8.19 µs | **367,878 ord/s** |
| **4. `Engine(busy_poll) + ProducerLane`** (2-thread SPSC) | 79.85 µs | 152.32 µs | 23.56 µs | 231,432 ord/s |

### Engineering Interpretation

1. **Single-order / Ping-Pong**: `DirectInitiator::send()` wins single-order 1-way p50 (**4.97 µs**) with zero cross-thread handoff; `DirectSendQueue` adds only ~160 ns to 1-way p50 (**5.13 µs**) and is **2.76 µs faster** than full multi-session `Engine + ProducerLane` (7.89 µs).
2. **Why `Enq->Recv` alone is misleading (Coordinated Omission)**: In Workload 3, Mode 1 (`DirectInitiator::send()`) shows `Enq->Recv p50 = 6.75 µs`, yet its true `CO-Free p50` is **100.50 µs** (`p99 = 198.74 µs`) because every synchronous `::send()` stalls the strategy thread before it can compute order `i+1`.
3. **Burst without strategy compute (Workload 2)**: Coalescing 32 orders into 1 `sendmsg()` (`enqueue()+flush()`) cuts `CO-Free p50` from **57.04 µs -> 22.73 µs (2.5×)** and triples throughput (**292k -> 946k ord/s**); adding the 2-thread SPSC `DirectSendQueue` pushes throughput past **1.02M ord/s** and `CO-Free p50` down to **18.14 µs**.
4. **Burst with strategy compute (Workload 3)**: With 32 × 2 µs = 64 µs of strategy compute per burst, 2-thread SPSC pipelining (`DirectSendQueue`) overlaps strategy computation with journal + TCP I/O on a second core, cutting `CO-Free p50` in half (**100.50 µs -> 48.52 µs**) and `p99` by **2.24×** (**198.74 µs -> 88.70 µs**). Even Layer 1/2 `Engine + ProducerLane` (79.85 µs) beats both single-thread modes under compute-heavy bursts for the same pipelining reason.

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
| **User Encode** (in-loop fill + single-pass `encode_fix44`) | **291.0 ns** | 311.0 ns | 390.0 ns | 3,345,749 msgs/s | **0** |
| **Pre-TCP Outbound** (in-loop fill + `DirectInitiator::send` + mmap Journal) | **521.0 ns** | 621.0 ns | 832.0 ns | 1,849,466 msgs/s | **0** |
| **Pre-TCP Inbound** (frame + parse + 912-field Schema + dual Journal records + callback) | **1,002.0 ns** | 1,102.0 ns | 1,393.0 ns | 973,061 msgs/s | **0** |
| **Pre-TCP Combined** (total pure FIX engine round-trip budget) | **1.52 µs** | 1.69 µs | 2.37 µs | 637,600 rt/s | **0** |
| **Real TCP Loopback RTT** (kernel TCP + full FIX 4.4 + mmap Journal) | **9.25 µs** | 11.26 µs | 16.93 µs | 103,134 rt/s | **0** |

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
| **Tier 1: Pre-TCP Acceptor** (`SessionDriver` wire-in -> 912-field Schema -> in-handler ER -> mmap Journal -> flush) | **1.53 µs** | 1.72 µs | 2.18 µs | **621,161 ord/s** |
| **Tier 2: 1-Session TCP** (`Engine<fix44::Schema, Inline>`, 1 `epoll` thread) | **11.68 µs** | 15.04 µs | 19.47 µs | **77,334 ord/s** |
| **Tier 3: 8 Concurrent Sessions TCP** (`Engine<fix44::Schema, Inline>`, 1 `epoll` thread) | **36.68 µs** | 80.39 µs | 100.80 µs | **157,998 ord/s** |

### Same-Run `IoBackend` Head-to-Head (`acceptor-bench 2000 20000 8 all`)
*(Executed back-to-back in a single process under identical clock/thermal state.)*

| `IoBackend` | 1-Session p50 | 1-Session p99 | 1-Session Rate | 8-Session p50 | 8-Session p99 | 8-Session Aggregate |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **`epoll`** (default) | 29.92 µs | 48.59 µs | 31,272 ord/s | 87.24 µs | 224.62 µs | 82,728 ord/s |
| **`busy_poll`** (zero-sleep spin) | **13.02 µs** | **33.64 µs** | **64,758 ord/s** | **50.21 µs** | 133.65 µs | 121,751 ord/s |
| **`io_uring`** (Proactor `RECV`/`SEND`, sleeping) | **15.16 µs** | 43.70 µs | 55,969 ord/s | **52.40 µs** | **95.58 µs** | **137,321 ord/s** (+66%) |

**Selection Guide**:
- **`epoll`**: Default zero-dependency choice for moderate session counts and low idle CPU.
- **`busy_poll`**: Dedicated isolated core with few sessions needing minimum 1-session p50/p99.
- **`io_uring`**: Multi-session gateways where Proactor batching cuts syscall count, delivering **+66% aggregate throughput** and **2.35× lower p99** than `epoll` without burning 100% CPU when idle.

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
| **NimbleFIX `Fixed` + `PartyGroup` (0-alloc)** | **371.0 ns** | **411.0 ns** | **2,733,537/s** | **0.00** | **4.46×** |
| **NimbleFIX `make()` + direct entry (`StaticGroup`)** | **611.0 ns** | **641.0 ns** | **1,606,102/s** | **0.00** | **2.71×** |
| **NimbleFIX `make()` + `NoPartyIDs<>` (dynamic arena)** | **681.0 ns** | **721.0 ns** | **1,463,412/s** | **0.00** | **2.43×** |
| **QuickFIX `FIX44::NewOrderSingle::toString`** | 1,653.0 ns | 1,703.0 ns | 601,914/s | 19.00 | 1.00× |

#### Suite 2: Parse + Full `FIX44.xml` Strict Validation + Typed Field & Group Read (5,000 samples)
| Engine | p50 | p99 | Rate | Heap Allocs / op | Speedup (p50) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| **NimbleFIX `parse` + `FIX44` strict + typed view** | **1,323.0 ns** | **1,663.0 ns** | **720,830/s** | **0.00** | **2.10×** |
| **QuickFIX `setString` + `FIX44` validate + `get`** | 2,775.0 ns | 2,816.0 ns | 359,028/s | 15.00 | 1.00× |

#### Suite 3: Real Loopback TCP Initiator Ping-Pong RTT (5,000 orders, 1,000 warmup)
| Engine / Mode | p50 | p99 | Throughput | Heap Allocs / op | Speedup (p50) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| **NimbleFIX `DirectInitiator` (Layer 0)** | **9.79 µs** | 29.39 µs | **86,979 rt/s** | **0.00** | **2.29×** |
| **NimbleFIX `Engine` (`busy_poll`)** | 14.83 µs | **20.80 µs** | 65,576 rt/s | **0.00** | 1.51× |
| **NimbleFIX `Engine` (`epoll`)** | 18.55 µs | 27.15 µs | 49,909 rt/s | **0.00** | 1.21× |
| **QuickFIX `SocketInitiator` (`FileStore`)** | 22.43 µs | 32.64 µs | 41,989 rt/s | 35.06 | 1.00× |

#### Suite 4: Real Loopback TCP Acceptor Turnaround RTT (5,000 orders, 1,000 warmup)
| Engine / Mode | p50 | p99 | Throughput | Heap Allocs / op | Speedup (p50) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| **NimbleFIX `Engine` Acceptor (`busy_poll`)** | **10.98 µs** | **14.70 µs** | **85,970 ord/s** | **0.00** | **2.17×** |
| **NimbleFIX `Engine` Acceptor (`epoll`)** | 13.29 µs | 16.59 µs | 74,672 ord/s | **0.00** | 1.79× |
| **QuickFIX `SocketAcceptor` (`FileStore`)** | 23.82 µs | 32.48 µs | 41,038 ord/s | 36.99 | 1.00× |

#### Suite 5: Open-Loop Dense Burst (156 bursts × 32 orders, CO-Free 1-Way Latency & Throughput)
| Engine / Mode | CO-Free 1-Way p50 | CO-Free 1-Way p99 | Throughput | Heap Allocs / op | Speedup (p50) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| **NimbleFIX `DirectInitiator::send()`** | 64.28 µs | 125.10 µs | 259,121 ord/s | **0.00** | 3.13× |
| **NimbleFIX `DirectInitiator` batch `sendmsg`** | **26.61 µs** | **36.21 µs** | **826,995 ord/s** | **0.00** | **7.55×** |
| **QuickFIX `SocketInitiator::sendToTarget`** | 200.92 µs | 403.16 µs | 79,136 ord/s | 28.06 | 1.00× |

### Key Takeaways
1. **Zero steady-state heap traffic across ALL APIs**: Both idiomatic fluent repeating groups (direct `.no_party_i_ds(entry1, ...)` `StaticGroup` and dynamic `NoPartyIDsGroup<>` 1 KB inline arena) and `Fixed` record **0.00 heap allocations/op** across all 5 suites (including full 912-field `FIX44.xml` validation and mmap journaling), whereas QuickFIX performs **15–37 heap allocations/op** (`std::map`/`std::string`/group nodes).
2. **Same OS sleep/wake model still wins cleanly**: Comparing sleeping `epoll` vs QuickFIX's `select`/`poll` loop directly, NimbleFIX `Engine(epoll)` is **1.21× faster** on Initiator RTT (18.55 µs vs 22.43 µs) and **1.79× faster** on Acceptor turnaround (13.29 µs vs 23.82 µs, **1.96× lower p99**).
3. **Architecture scales when latency matters most**: Switching to single-threaded Layer 0 `DirectInitiator` cuts Initiator ping-pong RTT to **9.79 µs (2.29× faster than QuickFIX)** and `Engine(busy_poll)` Acceptor to **10.98 µs (2.17× faster)**; under 32-order bursts, `enqueue()+flush()` (`sendmsg` batching) achieves **26.61 µs CO-Free p50 (7.55× lower)**, **36.21 µs p99 (11.1× lower)**, and **827k ord/s (10.5× higher throughput)**.
