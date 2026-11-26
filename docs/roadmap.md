# NimbleFIX Roadmap

This document tracks the planned engineering priorities for NimbleFIX, focusing on **single-core hot-path latency** first, followed by **multi-core scalability, operational resilience, and zero-overhead observability**, while keeping the three-layer C++20 core lean and embeddable.

---

## Phase 1 (Current Priority): Single-Core Hot-Path Performance

Goal: Push both outbound encoding and inbound strict parsing/validation firmly into the **sub-microsecond** regime while preserving 0 steady-state heap allocations and FIX protocol correctness.

### 1. [Done] Generated Switch-Based Fused Inbound Validator & O(1) Indexed View
- **Status**: Implemented and verified across the native C++20 schema compiler (`tools/schema/generate.cpp` / `nimblefix-schema-gen`), `SchemaDescriptor::fast_validate`, `detail::ParsedFieldIndex`, `inspect_frame`/`parse_flat_verified`, inline `Decimal::parse`, `DirectInitiator`, `SessionDriver`, and `Engine`.
- **Design**:
  - Codegen emits per-schema `detail_validate::fast_validate_message` with compile-time `switch (tag)` single-pass validation, bitset duplicate & required-field checks, leaf-first repeating group validators, and O(1) `MsgType` dispatch while preserving 100% exact error codes, `ValidationLevel`, and error precedence.
  - Generation-stamped `detail::ParsedFieldIndex` (zero `memset` per frame) records top-level field wire indices and repeating group boundaries during validation, powering O(1) `{Msg}View` getters (`get_indexed` / `group_indexed`) with transparent fallback for unindexed views.
  - Prevalidated/builtin-only `WireHints`, zero-division tag & decimal parsing, and single-pass `fast_checksum_bytes` eliminate framing overhead.
- **Measured Result**: Full 912-field `FIX44.xml` strict parse + validate + typed field & group read dropped from **1,323 ns -> 421 ns p50** (**3.14× faster**, **6.64× faster than QuickFIX 1.16.0** at 2,795 ns, **0 alloc**); Pre-TCP `DirectInitiator` inbound dropped from **972 ns -> 721 ns p50** (**1.39M msgs/s**); **combined pure FIX engine round-trip budget reached 942 ns (0.942 µs, 1.06M rt/s)** — sub-microsecond end-to-end!

### 2. [Done] Zero-Intermediate-Copy Streaming Encoder (`StreamWriter` / `send_stream`)
- **Status**: Implemented and verified across Layer 0/1/2 (`encode_stream`, `encode_fix44_stream`, `encode_body_stream`, `DirectInitiator::send_stream`/`enqueue_stream`, `DirectSendQueue::try_push_stream`, `SessionContext`/`SessionDriver`/`Sender`/`ProducerLane::send_stream`); legacy `make()` and `Draft` builders removed (`Fixed` preserved).
- **Measured Result**: Typical `NewOrderSingle` down to **121 ns p50** (0 alloc), `NewOrderSingle + NoPartyIDs[1]` **110–150 ns** (**9.07M msgs/s**, **7.81× faster than QuickFIX 1.16.0**), `DirectInitiator::send_stream` + mmap Journal **220 ns p50** (**4.43M msgs/s**), single-batch 32-order simultaneous TCP burst **1.41M–1.44M ord/s** (**14.24× lower `Burst->Recv` p50 than QuickFIX**).

### 3. [Done] Sub-Millisecond Timestamps (`µs` / `ns`) & Optional Checksum Bypass
- **Status**: Implemented and verified across `TimestampPrecision` (`milliseconds` / `microseconds` / `nanoseconds`), `FastUtcClock` & `Engine` wall clock, `CodecLimits::verify_checksum` (`true` by default), `DirectInitiatorConfig`, `SessionSettings`, and TOML configuration (`timestamp_precision = "ms" | "us" | "ns"`, `verify_checksum = true | false`).
- **Safety**: Even when `verify_checksum = false` is explicitly opted into on trusted links, `10=ddd<SOH>` framing syntax and `000..255` range remain strictly enforced.

### 4. [Done] Log-Driven Schema Optimization Tooling (`--from-fix-log`)
- **Status**: Implemented in the native C++20 schema compiler (`tools/schema/generate.cpp` / `nimblefix-schema-gen`) with `--from-fix-log <path>` (repeatable) and `--keep-all-fields`.
- **Capabilities**: Scans raw SOH (`\x01`), pipe-delimited (`|`), or binary `.nfxj` journal files to prune unobserved business messages and optional fields/groups while automatically preserving admin messages, required fields, repeating group delimiters, `Header`/`Trailer`, and paired `LENGTH`/`DATA` tags.

---

## Phase 2: Multi-Core Scalability, Resilience & Observability

### 5. [Done] Initiator Multi-Endpoint Failover (`ConnectionStrategy`)
- **Status**: Implemented and verified across Layer 0 `DirectInitiator`, Layer 1/2 `Engine` initiators, and TOML configuration (`endpoints = [...]` + `[sessions.connection_strategy]`).
- **Capabilities**: Shared deterministic `FailoverController` supporting `FailoverPolicy::ordered` (primary-first with `reset_to_primary_on_success`) and `FailoverPolicy::round_robin`, `immediate_failover` (0 ms switch to standby before wrap-around), exponential backoff (`initial_backoff_ms`, `max_backoff_ms`, `backoff_multiplier`), and `max_retries` exhaustion.

### 6. [Done] Sharded Reactor Multi-Core Scaling & Core Affinity
- **Status**: Implemented and verified via `<nimblefix/sharded_engine.hpp>` (`ShardedEngine<Schema, Policy>`), `pin_current_thread(int cpu_core)`, `IdleStrategy` (`sleep` / `busy_spin` / `hybrid_backoff`), single-port Logon-route socket handoff (TCP & TLS), and TOML configuration (`shards`, `cpu_cores`, `cpu_core`, `idle_strategy`).
- **Measured Result** (`acceptor-bench 1000 10000 8/16 sharded`, full 912-field `FIX44.xml` + mmap Journal, 1 shared port, loopback TCP on 8C/16T CPU):
  - **8 concurrent sessions**: 1-core `epoll` **176k ord/s** (`p99 = 76.91 µs`, `p99.9 = 227.25 µs`) -> 4-shard pinned `hybrid_backoff` **443k ord/s (2.51×)** with **`p99 = 21.27 µs` (3.62× lower)** and **`p99.9 = 28.98 µs` (7.84× lower)**.
  - **16 concurrent sessions**: 1-core `epoll` **176k ord/s** (`p50 = 75.64 µs`, `p99 = 161.11 µs`) -> 4-shard pinned `hybrid_backoff` **471k ord/s (2.67×)** with **`p50 = 31.19 µs`** and **`p99 = 49.99 µs` (3.22× lower)**.

### 7. [Done] Zero-Hot-Path-Impact Telemetry Ring & Stage Probes
- **Status**: Implemented and verified via `<nimblefix/telemetry.hpp>` (`TelemetryWriter`, `TelemetryReader`, lock-free 433-bucket HDR log-linear nanosecond `TelemetryHistogram`), `DirectInitiator`, `Engine`, `ShardedEngine` (shared single `.nfxt` across all shards), TOML configuration (`telemetry_path`, `telemetry_stage_probes`), and `nimblefix telemetry --path <file.nfxt> [--json | --prometheus]`.
- **Verified Contract**: Per-session state, counters, and 5 stage latency histograms (`read`, `parse`, `validate`, `journal`, `write` with `p50/p90/p99/p99.9/p99.99`) run with **0 steady-state heap allocations, 0 mutex locks, and 0 fsync** on `DirectInitiator`, and are readable out-of-band via read-only `O_RDONLY`/`PROT_READ` mmap from external sidecar tools or Prometheus exporters.

### 8. [Done] Cross-Process SPMC Journal Tailing (`JournalTailer`) & Deterministic Replay (`JournalReplayer` / `nimblefix replay`)
- **Status**: Implemented and verified via `<nimblefix/journal.hpp>` (`JournalTailer`, `JournalTailerConfig`, `JournalTailStart`), `<nimblefix/replay.hpp>` (`JournalReplayer::replay`, `JournalReplayer::replay_to_tcp`, `ReplayOptions` including `wire_hints`, `ReplayDirection`, `ReplayTiming`, `ReplayStats`) and `nimblefix replay --path <session.nfxj> [--session-key KEY] [--direction inbound|outbound|both] [--timing full|original|scaled] [--speed FACTOR] [--seq-from N] [--seq-to M] [--epoch N] [--skip-admin] [--endpoint HOST:PORT] [--json]`.
- **Capabilities**: Lock-free cross-process read-only SPMC `JournalTailer` (`O_RDONLY`/`PROT_READ`, zero `LOCK_EX` contention, zero writer hot-path overhead, non-blocking segment rotation) powers DropCopy/risk/compliance sidecars as well as live/offline path-based `JournalReplayer` playback (alongside the in-process lease-protected `JournalReader` overload), with nanosecond `52=SendingTime` original/scaled timing or full-speed playback, optional admin/wire-hint filtering and strict frame verification, and direct streaming to either an in-process callback sink or a real TCP endpoint.

### 9. [Done] Heterogeneous Multi-Schema Engine & `UnexpectedFieldPolicy`
- **Status**: Implemented and verified across `schema.hpp`, `settings.hpp` (`SessionSettings::schema`, `schema_wire_hints`, `use_schema<Schema>()` / `use_schema(desc, hints)`, `UnexpectedFieldPolicy`), `Engine`, `ShardedEngine`, `DirectInitiator`, `SessionSnapshot`, and `TelemetrySessionSlot` / Prometheus exposition.
- **Capabilities**:
  - Sessions within the same multi-session `Engine<FlatSchema>` (or typed `Engine`) can bind distinct compiled or runtime `SchemaDescriptor`s with merged per-session `WireHints` and independent sender fingerprint verification.
  - Per-session `UnexpectedFieldPolicy` (`reject` [default, zero hot-path overhead via `fast_validate`], `ignore_with_metric`, `passthrough`) accepts unannounced venue/custom tags while preserving strict required-field, enum, type, ordering, and repeating-group validation.

### 10. [Done] Operational Production Controls & Low-Latency Socket Tuning
- **Status**: Implemented and verified across `SessionSchedule`, `SessionDriver`, `DirectInitiator`, `Engine`, `ShardedEngine`, `TcpStream`, `TcpListener`, and `TcpTransport`.
- **Capabilities**:
  - Opt-in schedule-boundary EOD/BOD sequence reset (`SessionSchedule::reset_on_schedule_boundary`, `window_id()`) with audited `epoch_reset` transitions.
  - Thread-safe runtime session admin controls (`request_logout`, `request_reconnect`, `request_sequence_reset`, durable O(1) `adjust_sequences` with GapFill replay support across adjusted ranges) on `Engine`, `ShardedEngine`, `SessionDriver`, and `DirectInitiator`.
  - Local source IP/port binding & low-latency socket tuning (`SocketOptions`: `local_bind_ip`, `local_bind_port`, best-effort `so_busy_poll_us`, `tcp_quickack`, `send_buffer_bytes`, `recv_buffer_bytes`).

---

## Explicit Scope Boundaries (Non-Goals for Core)

To keep NimbleFIX fast, auditable, and embeddable (~336 KB Layer 0 core), the following are intentionally **outside** the core library scope:
- **Cross-host distributed HA / Raft journal replication**: If needed later, should live in an optional Layer 3 replication module rather than coupling into Layer 0/1.
- **FIX Router DSL / Version Translation / Rule-Based Drop Copy**: Better built as standalone applications/examples on top of NimbleFIX.
- **Web Management UI & Non-FIX Protocols (e.g., ITCH)**: Out-of-process tools can consume the telemetry/control interfaces; non-FIX binary market data protocols belong in separate libraries.
