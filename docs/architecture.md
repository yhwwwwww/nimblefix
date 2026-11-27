# NimbleFIX Architecture

**English** | [简体中文](architecture.zh-CN.md)

NimbleFIX is structured as a three-layer C++20 FIX protocol library. Every hot-path decision targets predictable sub-microsecond protocol processing and zero steady-state heap allocations without relaxing FIX protocol correctness or crash recovery guarantees.

> Related docs: [User Guide](user-guide.md) · [Support Matrix](session-support.md) · [Benchmarks](benchmarks.md)

---

## 1. Three-Layer Modular Stack

```text
+-----------------------------------------------------------------------------+
| Full Runtime: Multi-Session Engine, ShardedEngine, Acceptor, Inline/Queued  |
|               Workers, Pluggable IoBackend (epoll / busy_poll / io_uring),  |
|               MPSC Sender, SPSC ProducerLane, Scheduler                     |
|               (NimbleFIX::nimblefix)                                        |
+-----------------------------------------------------------------------------+
| Core + TLS:   DirectTlsTransport (OpenSSL 3, TLS 1.2/1.3, CA/SAN, mTLS)     |
|               (NimbleFIX::nimblefix_core_tls)                               |
+-----------------------------------------------------------------------------+
| Core:         Streaming Codec (StreamWriter / Fixed), Generated Schemas,    |
|               Segmented mmap Journal (SSE4.2 CRC32C, SPMC JournalTailer),   |
|               SessionCore, TcpTransport (write_batch/sendmsg),            |
|               DirectInitiator, ConnectionStrategy Failover,               |
|               DirectSendQueue (SPSC), Telemetry, JournalReplayer          |
|               (NimbleFIX::nimblefix_core — C++20 + pthread only)            |
+-----------------------------------------------------------------------------+
```

- **Core (`nimblefix_core`)** gives single-session trading clients direct access to the protocol engine, mmap journal, and TCP transport with zero `Engine`, `epoll`, `mutex`, or background-thread overhead (~336 KB Release binary).
- **Core + TLS (`nimblefix_core_tls`)** adds nonblocking OpenSSL 3 TLS/mTLS by swapping only the `Transport` template parameter on `DirectInitiator`.
- **Full Runtime (`nimblefix`)** adds multi-session multiplexing, Acceptor listeners, pluggable `IoBackend`, `Queued` worker pools, MPSC/SPSC cross-thread sending, and multi-core `ShardedEngine`.

---

## 2. Codec & Message Construction

### Streaming Encoder (`StreamWriter`)
The primary outbound API streams typed FIX fields directly into the destination wire buffer (in `DirectInitiator`, directly into the mmap journal `reserve()` slot) with zero intermediate objects and zero heap allocations:
1. **Static Session Identity vs. Dynamic Standard Header**: Session-immutable fields (`8=<BeginString>\x019=` and `49=<SenderCompID>\x0156=<TargetCompID>\x01`) are cached in `StaticSessionPrefix`. Per-message dynamic header fields (`50=SenderSubID`, `57=TargetSubID`, `142=SenderLocationID`, `115/116=OnBehalfOf*`, `128/129=DeliverTo*`, etc.) remain writable per message via `w.header()` before body fields.
2. **Inline Validation & Formatting**: Every setter writes its compile-time tag prefix (`"11="`, `"44="`, ...) and formatted value, validates enums/timestamps/text inline, tracks required fields via a compact bitmask, and supports native `if`/`for`, `std::optional<T>`, automatic `DATA`/`LENGTH` pairs, and nested repeating groups.
3. **Single-Pass `BodyLength` & `CheckSum`**: Backpatches exact `BodyLength` (tag 9) and computes `CheckSum` (tag 10) in one contiguous SIMD-friendly pass.

| API | Mutability / Control Flow | Allocation | Reuse | Primary Use |
| :--- | :--- | :--- | :--- | :--- |
| **Streaming (Default)**: `send_stream<Msg>(fn)` / `encode_stream<Msg>` | Native `if`/`for`, `std::optional<T>`, nested groups, `w.header()` | 0 heap alloc | Stateless lambda | Fastest & most flexible production API (~110–150 ns) |
| **Fixed Template**: `NewOrderSingle::Fixed<ByteCap, fix::field::...>` | Fixed declared field set, presence bitmap | 0 heap alloc (stack buffer) | `reset()` in-place reuse | Reusable pre-populated message templates |
| **Untyped / Runtime**: `nf::message<"D">()` / `DynamicMessage` | Arbitrary runtime `(tag, value)` pairs | SSO / heap | Mutable | Low-level admin frames & dynamic venue tags |

### Compact Member Indexing & Subset Codegen
- `nimblefix-schema-gen` compiles standard or custom FIX XML dictionaries into compact constexpr tables and generated `switch(tag)` single-pass validators with generation-stamped O(1) `ParsedFieldIndex` lookups.
- All per-message slot tables and deduplication bitsets are indexed by compact per-message member index, never by raw FIX tag number — supporting custom venue tags across `1..999,999,999` with no memory bloat.
- `--messages` and `--from-fix-log` prune unused business messages and optional fields while preserving required admin messages, components, and repeating groups.

---

## 3. Segmented mmap Journal & Crash Recovery

Every session uses a pre-allocated, segmented `MAP_SHARED` mmap journal:

- **Zero-copy outbound write path**: `journal.reserve()` hands the encoder a direct pointer into the mapped segment; once encoded, `journal.commit()` writes the hardware SSE4.2 CRC32C and publishes a release-store commit word. The socket sends directly from the committed journal bytes.
- **Persistence order**:
  - **Outbound**: commit to journal -> transmit on socket.
  - **Inbound**: commit frame + next expected sequence to journal -> invoke business callback -> commit delivery completion marker (advancing FIX tag `369` `LastMsgSeqNumProcessed`).
- **Crash recovery**: After `SIGKILL` or process crash on the same host, reopening the store rebuilds sequence state, re-delivers committed-but-unfinished inbound messages (`DeliveryInfo::is_recovery == true`), reuses already-committed dependent replies, and serves `ResendRequest`, `GapFill`, and business `Replay` from retained segments.
- **Off-hot-path maintenance**: Segment pre-allocation (`.tmp` + atomic `rename`) and old-segment pruning run via `poll_maintenance()`, keeping `open`/`mmap`/`unlink` syscalls off the send hot path.
- **Durability scope**: Confirmation level `appended` covers same-host process crashes while the OS and filesystem remain healthy; no per-message `fsync`/`msync` runs on the hot path.

---

## 4. `DirectInitiator` Send Modes

`DirectInitiator<Schema, Handler, Transport>` exposes three composable send paths on the same session and journal:

1. **Direct Immediate Send (`send_stream`)** — 1 thread, no queue. Encodes directly into the mmap journal and issues a nonblocking `::send()`. Lowest single-order / Ping-Pong latency.
2. **Same-Thread Burst Batching (`enqueue_stream` + `flush`)** — 1-thread coalescing. Stages up to 64 consecutive frames in the journal and flushes them in a single `sendmsg()` scatter-gather syscall.
3. **2-Thread Lock-Free SPSC Pipeline (`DirectSendQueue` + `drain`)** — Strategy thread calls `queue.try_push_stream(...)` into a pre-allocated cache-line-aligned ring buffer (0 mutex / 0 malloc); dedicated I/O thread calls `init.drain(queue)` to batch-pop up to 64 orders, stamp `MsgSeqNum`/`SendingTime` into the mmap journal, and flush via a single `sendmsg()`.

---

## 5. `Engine`, Pluggable `IoBackend` & Multi-Core `ShardedEngine`

### Single-Writer Protocol Ownership
`Engine<Schema, Policy>` runs all protocol state, TCP/TLS I/O, timers, and journal writes on a single protocol-owner thread (`engine.run()` or caller-driven `start()`/`poll_once()`):
- Every session has strictly one protocol writer — zero locks on `SessionCore`, sequence numbers, or mmap journal commits.
- Business callbacks run either `Inline` (on the protocol thread, lowest latency) or `Queued` (dispatched to bounded worker threads with at most one in-flight callback per session).
- External threads submit outbound messages through bounded MPSC `Sender` or lock-free SPSC `ProducerLane`.

### Pluggable `IoBackend`
OS readiness/completion handling is abstracted behind `IoBackend` (`<nimblefix/io_backend.hpp>`):
- **`epoll` (default)**: General-purpose readiness loop with zero extra dependencies.
- **`busy_poll`**: Zero-sleep nonblocking poll loop for dedicated cores targeting minimum single-session latency.
- **`io_uring` (optional Linux Proactor)**: Uses `liburing` with `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_R_DISABLED`, submitting asynchronous `IORING_OP_RECV` and `IORING_OP_SEND` batches while sleeping cleanly when idle.

### Multi-Core `ShardedEngine`
For multi-core deployments scaling across many concurrent sessions (`<nimblefix/sharded_engine.hpp>`):
- Runs `K` independent `IoBackend` reactor threads with optional per-shard Linux CPU core pinning (`cpu_cores` / `pin_current_thread`) and configurable `IdleStrategy` (`sleep`, `busy_spin`, `hybrid_backoff`).
- Every session is statically pinned to one shard, preserving single-writer lock-free `SessionCore` and mmap journal ownership within every shard.
- Shared Acceptor listeners bind once on Shard 0. Incoming TCP/TLS connections perform `Logon (35=A)` routing on Shard 0 and hand the accepted socket directly to the owning shard's reactor loop.

---

## 6. Out-of-Band mmap Telemetry & Historical Replay

- **mmap Telemetry (`<nimblefix/telemetry.hpp>`)**: Opt-in `MAP_SHARED` `.nfxt` telemetry region with cache-line-aligned per-session slots written exclusively by each session's protocol-owner thread (relaxed load+store, zero locks/mallocs/syscalls). When `telemetry_stage_probes = true`, records 433-bucket log-linear nanosecond HDR histograms for `read`, `parse`, `validate`, `journal`, and `write`. `TelemetryReader` maps the file `O_RDONLY`/`PROT_READ` from an external process and exports structured snapshots or Prometheus metrics.
- **Deterministic Replay & Cross-Process SPMC Tailing (`<nimblefix/journal.hpp>`, `<nimblefix/replay.hpp>`)**: `JournalTailer` maps live or offline `.nfxj` segments `O_RDONLY`/`PROT_READ` without taking `LOCK_EX` and polls the writer's release-published `record_commit_marker` lock-free across segment rotations — enabling zero-writer-overhead DropCopy, compliance, and risk sidecar processes. `JournalReplayer` uses `JournalTailer` for path-based live/offline playback (and supports an in-process lease-protected `JournalReader` overload), with direction (`inbound`/`outbound`/`both`), sequence range, epoch, admin, and `WireHints` filtering at full-speed, original `52=SendingTime`, or scaled pacing into an in-process callback or TCP endpoint.
