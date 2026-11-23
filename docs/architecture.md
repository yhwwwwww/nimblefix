# NimbleFIX 0.3 Architecture

NimbleFIX 0.3 is structured as a **3-layer, pay-only-for-what-you-need** C++20 FIX engine. Every hot-path decision targets predictable sub-microsecond protocol processing and zero steady-state heap allocations without relaxing FIX protocol correctness or crash recovery guarantees.

---

## 1. Three-Layer Modular Stack

```text
+-----------------------------------------------------------------------------+
| Layer 2: Multi-Session Engine, Acceptor, Inline/Queued Workers, XML, CLI    |
|          (NimbleFIX::nimblefix)                                             |
+-----------------------------------------------------------------------------+
| Layer 1: SessionDriver, Pluggable IoBackend (epoll / busy_poll / io_uring), |
|          MPSC Sender, SPSC ProducerLane, Scheduler, Diagnostics             |
+-----------------------------------------------------------------------------+
| Layer 0 + TLS: DirectTlsTransport (OpenSSL 3, TLS 1.2/1.3, SAN, mTLS)       |
|                (NimbleFIX::nimblefix_core_tls)                              |
+-----------------------------------------------------------------------------+
| Layer 0 Core: Codec (SSO make() / Draft / Fixed), Generated Schemas,        |
|               Segmented mmap Journal (SSE4.2 CRC32C), SessionCore,          |
|               TcpTransport (write_batch/sendmsg), DirectInitiator,          |
|               DirectSendQueue (lock-free SPSC)                              |
|               (NimbleFIX::nimblefix_core — C++20 + pthread only)            |
+-----------------------------------------------------------------------------+
```

- **Layer 0 (`nimblefix_core`)** gives single-session trading clients direct access to the protocol engine, mmap journal, and TCP transport with zero `Engine`, `epoll`, `mutex`, or background-thread overhead (~336 KB Release binary).
- **Layer 0 + TLS (`nimblefix_core_tls`)** adds nonblocking OpenSSL 3 TLS/mTLS by swapping only the `Transport` template parameter on `DirectInitiator`.
- **Layer 1 / 2 (`nimblefix`)** adds multi-session multiplexing, Acceptor listeners, pluggable `IoBackend`, `Queued` worker pools, MPSC/SPSC cross-thread sending, runtime XML dictionaries, and the TOML CLI.

---

## 2. Codec & Message Construction

### Unified Single-Pass Backpatch Encoder
All message builders (`make()`, `Draft`, `Fixed`, `DynamicMessage`) feed the same single-pass encoder:
1. Reserves fixed-width placeholders for `8=FIX...`, `9=000000`, and session header fields (`35`, `49`, `56`, `34`, `52`).
2. Writes body fields directly into the output span (in `DirectInitiator` and `Engine`, directly into the **zero-copy mmap Journal `reserve()` slot**).
3. Backpatches exact `BodyLength` (tag 9) and computes `CheckSum` (tag 10) in a single pass with zero intermediate string copies.

### Small-Buffer-Optimized (SSO) Message APIs
Both primary message APIs embed **16 field slots + 256 bytes of inline string storage** on the stack by default (customizable via `<Slots, Bytes>`), giving **0 `malloc`** for typical orders while automatically spilling to heap if an outlier order carries extra fields or long strings (never rejecting valid orders):

| API | Mutability | Conditional Fields | Reuse | Wire Order |
| :--- | :--- | :--- | :--- | :--- |
| **Scheme A**: `NewOrderSingle::make()` / `make<S, B>()` | Immutable fluent chain (rvalue-consumed) | `std::optional<T>` (`nullopt` skipped) | One-shot | Compile-time canonical order (no runtime sort) |
| **Scheme B**: `NewOrderSingle::Draft` / `DraftWith<S, B>` | Mutable object (statement-by-statement, overwrite) | Native `if (...)` or `std::optional<T>` | `reset()` in-place reuse | Canonical `WireRank` order |
| **Strict**: `NewOrderSingle::Fixed<ByteCap, fix::field::...>` | Fixed declared field set, no heap fallback | Presence bitmap (`if (...)`) | `reset()` in-place reuse | Compile-time canonical order |
| **Runtime**: `DynamicMessage` | Arbitrary runtime `(tag, value)` pairs | Native `if (...)` | Mutable | Insertion order |

### Compact Member Indexing & Subset Codegen
- `tools/schema/generate.py` compiles standard or custom FIX XML dictionaries into compact constexpr tables.
- All per-message slot tables and deduplication bitsets are indexed by **compact per-message member index**, never by raw FIX tag number — supporting custom venue tags across `1..999,999,999` with zero memory bloat.
- `--messages NewOrderSingle,ExecutionReport,...` prunes unused business messages while preserving required admin messages, components, and repeating groups.

---

## 3. Segmented mmap Journal & Crash Recovery

Every session (both `DirectInitiator` and `Engine`) uses a pre-allocated, segmented `MAP_SHARED` mmap journal:

- **Zero-copy outbound write path**: `journal.reserve()` hands the encoder a direct pointer into the mapped segment; once encoded, `journal.commit()` writes the hardware SSE4.2 CRC32C and publishes a release-store commit word. The socket sends directly from the committed journal bytes.
- **Strict persistence order**:
  - **Outbound**: commit to journal -> transmit on socket.
  - **Inbound**: commit frame + next expected sequence to journal -> invoke business callback -> commit delivery completion marker (which advances FIX tag 369 `LastMsgSeqNumProcessed`).
- **Crash recovery**: After `SIGKILL` or process crash on the same host, reopening the store rebuilds sequence state, re-delivers any committed-but-unfinished inbound messages (`DeliveryInfo::is_recovery == true`), reuses already-committed dependent replies, and serves `ResendRequest` / `GapFill` / business `Replay` from retained segments.
- **Off-hot-path maintenance**: Segment pre-allocation (`.tmp` + atomic `rename`) and old-segment pruning run via `poll_maintenance()`, keeping `open`/`mmap`/`unlink` syscalls off the send hot path.
- **Durability contract**: Confirmation level `appended` protects against same-host process crashes while OS/filesystem remain healthy; no per-message `fsync`/`msync` is executed on the hot path.

---

## 4. Layer 0 `DirectInitiator`: Three Send Modes

`DirectInitiator<Schema, Handler, Transport>` exposes three composable send paths on the same session and journal:

1. **Direct Immediate Send (`init.send(msg)`)** — 1-thread, zero queue. Encodes directly into the mmap journal and issues a nonblocking `::send()`. Lowest single-order / Ping-Pong latency (~4.97 µs 1-way over loopback TCP).
2. **Same-Thread Burst Batching (`init.enqueue(msg)` + `init.flush()`)** — 1-thread coalescing. Stages up to 64 consecutive frames in the journal and flushes them in a single `sendmsg()` scatter-gather syscall (`TcpTransport::write_batch()`). Ideal when one thread emits a tight burst of ready orders (3.2× higher burst throughput, 2.5× lower burst p50 than serial `send()`).
3. **2-Thread Lock-Free SPSC Pipeline (`DirectSendQueue` + `init.drain(queue)`)** — Strategy thread calls `queue.try_push(msg)` (encodes only body fields into a pre-allocated cache-line-aligned ring buffer, 0 mutex / 0 malloc); dedicated I/O thread calls `init.drain(queue)` to batch-pop up to 64 orders, stamp `MsgSeqNum`/`SendingTime` into the mmap journal, and flush via a single `sendmsg()`. Overlaps strategy compute with I/O and delivers **2× lower Coordinated-Omission-Free latency** and **1.0M+ ord/s** burst throughput.

---

## 5. Layer 1 / 2 `Engine`: Single-Writer Model & Pluggable `IoBackend`

### Single-Writer Protocol Ownership
`Engine<Schema, Policy>` runs all protocol state, TCP/TLS I/O, timers, and journal writes on a single protocol-owner thread (`engine.run()` or caller-driven `start()`/`poll_once()`):
- Every session has strictly one protocol writer — zero locks on `SessionCore`, sequence numbers, or mmap journal commits.
- Business callbacks run either **`Inline`** (directly on the protocol thread, lowest latency) or **`Queued`** (dispatched to bounded worker threads with at most one in-flight callback per session to preserve per-session order).
- External threads submit outbound messages through bounded MPSC **`Sender`** or lock-free SPSC **`ProducerLane`**.

### Pluggable `IoBackend`
OS readiness/completion handling is abstracted behind `IoBackend` (`<nimblefix/io_backend.hpp>`):
- **`epoll` (default)**: Zero extra dependencies, edge/level readiness loop suitable for general workloads.
- **`busy_poll`**: Zero-sleep nonblocking poll loop for dedicated isolated cores targeting minimum 1-session latency.
- **`io_uring` (optional Linux Proactor)**: Uses `liburing` with `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_R_DISABLED` (enabling the ring lazily on the `run()` thread). Submits true asynchronous `IORING_OP_RECV` and `IORING_OP_SEND` batches while sleeping cleanly when idle — delivering +66% aggregate throughput and 2.35× lower p99 under 8 concurrent sessions compared to `epoll`.

### Multi-Core Scaling Roadmap
For deployments scaling to hundreds or thousands of high-rate sessions beyond a single core's budget, the planned model is a **Sharded Reactor** (`K` independent `IoBackend` loops with static session-to-shard pinning), preserving strict single-writer lock-free protocol state and journal locality within every shard.
