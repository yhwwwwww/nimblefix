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
| Layer 0 Core: Codec (Streaming StreamWriter / Fixed), Generated Schemas,      |
|               Segmented mmap Journal (SSE4.2 CRC32C), SessionCore,          |
|               TcpTransport (write_batch/sendmsg), DirectInitiator,          |
|               DirectSendQueue (lock-free SPSC)                              |
|               (NimbleFIX::nimblefix_core — C++20 + pthread only)            |
+-----------------------------------------------------------------------------+
```

- **Layer 0 (`nimblefix_core`)** gives single-session trading clients direct access to the protocol engine, mmap journal, and TCP transport with zero `Engine`, `epoll`, `mutex`, or background-thread overhead (~336 KB Release binary).
- **Layer 0 + TLS (`nimblefix_core_tls`)** adds nonblocking OpenSSL 3 TLS/mTLS by swapping only the `Transport` template parameter on `DirectInitiator`.
- **Layer 1 / 2 (`nimblefix`)** adds multi-session multiplexing, Acceptor listeners, pluggable `IoBackend`, `Queued` worker pools, MPSC/SPSC cross-thread sending, and optional runtime XML dictionaries. All library targets expose pure C++ configuration structs with zero config-file format preference; the optional standalone `nimblefix` CLI (`tools/nimblefix`) parses TOML purely as an application-layer utility.

---

## 2. Codec & Message Construction

### Zero-Intermediate-Copy Streaming Encoder (`StreamWriter`)
The primary generated outbound API streams typed FIX fields directly into the destination wire buffer (in `DirectInitiator`, directly into the **zero-copy mmap Journal `reserve()` slot**) with zero intermediate `SlotValue`/`PlanField` structures and zero heap allocations:
1. **Static Session Identity vs Dynamic Standard Header**: Only session-immutable fields (`8=<BeginString>\x019=` and `49=<SenderCompID>\x0156=<TargetCompID>\x01`) are cached in `StaticSessionPrefix`. Per-message dynamic standard header fields (`50=SenderSubID`, `57=TargetSubID`, `142=SenderLocationID`, `115/116=OnBehalfOf*`, `128/129=DeliverTo*`, etc.) remain dynamically writable per message via `w.header()` before body fields.
2. **Inline Validation & Formatting**: Every setter writes its compile-time tag prefix (`"11="`, `"44="`, ...) and formatted value (`write_u64_fast`, single-pass `write_decimal_fast`), validates enums/timestamps/text inline, tracks required fields via a compact bitmask, and supports native `if`/`for`, `std::optional<T>`, auto-length `DATA`/`LENGTH` pairs, and nested repeating groups (`w.no_party_i_ds(n, [&](auto& g) { g.entry()...; })`).
3. **Single-Pass `BodyLength` & Auto-Vectorizable `CheckSum`**: Backpatches exact `BodyLength` (tag 9, zero `memmove` for 100–999 B frames) and computes `CheckSum` (tag 10) in one contiguous SIMD-friendly loop.

| API | Mutability / Control Flow | Allocation | Reuse | Primary Use |
| :--- | :--- | :--- | :--- | :--- |
| **Streaming (Default)**: `send_stream<Msg>(fn)` / `encode_fix44_stream<Msg>` | Native `if`/`for`, `std::optional<T>`, nested groups, `w.header()` | **0 heap alloc** (streams straight to wire/Journal) | Stateless lambda | Fastest & most flexible production API (**130–140 ns**, **6.9M msgs/s**) |
| **Strict Fixed**: `NewOrderSingle::Fixed<ByteCap, fix::field::...>` | Fixed declared field set, presence bitmap (`if (...)`) | **0 heap alloc** (bounded stack buffer) | `reset()` in-place reuse | Reusable pre-populated message templates |
| **Untyped / Runtime**: `nf::message<"D">()` / `DynamicMessage` | Arbitrary runtime `(tag, value)` pairs | SSO / heap | Mutable | Low-level admin frames & dynamic venue tags |

### Compact Member Indexing & Subset Codegen
- `nimblefix-schema-gen` (`tools/schema/generate.cpp`, pure zero-dependency C++20) compiles standard or custom FIX XML dictionaries into compact constexpr tables.
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

### Multi-Core Scaling: `ShardedEngine` (Sharded Reactor) & Core Affinity
For multi-core deployments scaling across many concurrent sessions (`<nimblefix/sharded_engine.hpp>`):
- **`ShardedEngine<Schema, Policy>`** runs `K` completely independent `IoBackend` (`epoll` / `busy_poll` / `io_uring`) reactor threads with optional per-shard Linux CPU core pinning (`cpu_cores` / `pin_current_thread`) and configurable `IdleStrategy` (`sleep`, `busy_spin`, `hybrid_backoff`).
- **Static Session-to-Shard Pinning**: Every Initiator and Acceptor session is pinned to exactly one shard (`shard_index` or automatic round-robin), preserving strict single-writer lock-free `SessionCore` and segmented mmap Journal ownership within every shard.
- **Single-Port Shared Acceptor Handoff**: Shared Acceptor listeners bind once on Shard 0. When a client connects and sends its initial `Logon (35=A)` (plaintext TCP or TLS), Shard 0 routes by `(SenderCompID, TargetCompID, BeginString)` and hands the accepted socket directly into the owning shard's reactor loop — 100% of steady-state order processing executes lock-free on the owning shard's pinned CPU core.

---

## 6. Out-of-Band mmap Telemetry & Stage Latency Probes

To avoid introducing monitoring jitter onto trading threads, NimbleFIX provides an opt-in `MAP_SHARED` mmap telemetry file (`<nimblefix/telemetry.hpp>`, `.nfxt` format):
- **Single-Writer Lock-Free Design**: Each session owns one cache-line-aligned `TelemetrySessionSlot` written exclusively by its protocol-owner thread using relaxed load+store updates (no `lock` RMW prefix, no mutex, no heap allocation, no syscall). `ShardedEngine` shares a single `.nfxt` mapping across all `K` shards so external monitors inspect one file.
- **5 Stage HDR Histograms**: When `telemetry_stage_probes = true`, `read`, `parse`, `validate`, `journal`, and `write` durations are recorded into 433-bucket log-linear nanosecond histograms (`[0, 256 ns)` in 16 ns bins + 26 octaves × 16 sub-buckets up to 17.1 s), exporting `min/p50/p90/p99/p99.9/p99.99/max`.
- **Read-Only Sidecar Export**: `TelemetryReader::open(path)` maps the region `O_RDONLY` / `PROT_READ` with zero writes or locks on the trading process, powering both embedded monitors and `nimblefix telemetry --path <file.nfxt> [--json | --prometheus]`.

---

## 7. Deterministic Historical Replay

Built directly on top of the lease-protected mmap `JournalReader` (`<nimblefix/replay.hpp>`), `JournalReplayer` and `nimblefix replay` enable deterministic offline or TCP playback of recorded FIX sessions:
- **Live or Offline Playback**: `JournalReplayer::replay(Journal&, ...)` attaches a lease-protected `JournalReader` to a live journal; `JournalReplayer::replay(ReplayOptions, ...)` auto-probes the v2 `.nfxj` manifest header on disk (geometry + session key) and streams matching records verbatim.
- **Filtering & Pacing**: Supports direction (`inbound`, `outbound`, `both`), sequence range (`seq_from..seq_to`), epoch filter, optional admin skip, strict wire verification, and nanosecond `52=SendingTime` original / scaled (`speed_multiplier`) / full-speed pacing into either an in-process sink or a real TCP endpoint (`replay_to_tcp` / `--endpoint HOST:PORT`).
