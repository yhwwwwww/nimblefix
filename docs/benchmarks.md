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
