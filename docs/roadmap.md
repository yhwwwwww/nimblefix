# NimbleFIX Roadmap

This document tracks the planned engineering priorities for NimbleFIX after `0.3`, focusing on **single-core hot-path latency** first, followed by **multi-core scalability, operational resilience, and zero-overhead observability**, while keeping the 3-layer pay-only-for-what-you-need C++20 core lean and embeddable.

---

## Phase 1 (Current Priority): Single-Core Hot-Path Performance

Goal: Push both outbound encoding and inbound strict parsing/validation firmly into the **sub-microsecond** regime while preserving 0 steady-state heap allocations and FIX protocol correctness.

### 1. [Done] Generated Switch-Based Fused Inbound Validator & O(1) Indexed View
- **Status**: Implemented and verified across codegen (`tools/schema/generate.py`), `SchemaDescriptor::fast_validate`, `detail::ParsedFieldIndex`, `inspect_frame`/`parse_flat_verified`, inline `Decimal::parse`, `DirectInitiator`, `SessionDriver`, and `Engine`.
- **Design**:
  - Codegen emits per-schema `detail_validate::fast_validate_message` with compile-time `switch (tag)` single-pass validation, bitset duplicate & required-field checks, leaf-first repeating group validators, and O(1) `MsgType` dispatch while preserving 100% exact error codes, `ValidationLevel`, and error precedence.
  - Generation-stamped `detail::ParsedFieldIndex` (zero `memset` per frame) records top-level field wire indices and repeating group boundaries during validation, powering O(1) `{Msg}View` getters (`get_indexed` / `group_indexed`) with transparent fallback for unindexed views.
  - Prevalidated/builtin-only `WireHints`, zero-division tag & decimal parsing, and single-pass `fast_checksum_bytes` eliminate framing overhead.
- **Measured Result**: Full 912-field `FIX44.xml` strict parse + validate + typed field & group read dropped from **1,323 ns -> 421 ns p50** (**3.14× faster**, **6.64× faster than QuickFIX 1.16.0** at 2,795 ns, **0 alloc**); Pre-TCP `DirectInitiator` inbound dropped from **972 ns -> 721 ns p50** (**1.39M msgs/s**); **combined pure FIX engine round-trip budget reached 942 ns (0.942 µs, 1.06M rt/s)** — sub-microsecond end-to-end!

### 2. [Done] Zero-Intermediate-Copy Streaming Encoder (`StreamWriter` / `send_stream`)
- **Status**: Implemented and verified across Layer 0/1/2 (`encode_stream`, `encode_fix44_stream`, `encode_body_stream`, `DirectInitiator::send_stream`/`enqueue_stream`, `DirectSendQueue::try_push_stream`, `SessionContext`/`SessionDriver`/`Sender`/`ProducerLane::send_stream`); legacy `make()` and `Draft` builders removed (`Fixed` preserved).
- **Measured Result**: Typical `NewOrderSingle` down to **130 ns p50** (0 alloc), `NewOrderSingle + NoPartyIDs[1]` **140–211 ns** (**6.9M msgs/s**, **7.9–8.6× faster than QuickFIX 1.16.0**), `DirectInitiator::send_stream` + mmap Journal **281 ns p50** (**3.47M msgs/s**), 32-order TCP burst **1.33M–1.43M ord/s** (**12.69× lower CO-Free p50 than QuickFIX**).

### 3. [Done] Sub-Millisecond Timestamps (`µs` / `ns`) & Optional Checksum Bypass
- **Status**: Implemented and verified across `TimestampPrecision` (`milliseconds` / `microseconds` / `nanoseconds`), `FastUtcClock` & `Engine` wall clock, `CodecLimits::verify_checksum` (`true` by default), `DirectInitiatorConfig`, `SessionSettings`, and TOML configuration (`timestamp_precision = "ms" | "us" | "ns"`, `verify_checksum = true | false`).
- **Safety**: Even when `verify_checksum = false` is explicitly opted into on trusted links, `10=ddd<SOH>` framing syntax and `000..255` range remain strictly enforced.

### 4. [Done] Log-Driven Schema Optimization Tooling (`--from-fix-log`)
- **Status**: Implemented in `tools/schema/generate.py` with `--from-fix-log <path>` (repeatable) and `--keep-all-fields`.
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

### 7. Zero-Hot-Path-Impact Telemetry Ring & Stage Probes
- Expose a lock-free / mmap-backed telemetry region (inspired by out-of-band monitoring queues) that records per-session state, counters, and nanosecond stage latency histograms (`p50/p90/p99/p99.9/p99.99` across read, parse, validate, journal, and write) without locks, allocations, or syscalls on the hot path.
- Enable external sidecar tools / Prometheus / Grafana exporters to read metrics with zero jitter impact on trading threads.

### 8. Deterministic Historical Replay (`nimblefix-cli replay`)
- Build on the existing lease-protected mmap Journal reader to replay recorded FIX sessions (original timing or full-speed) into a target endpoint or offline test harness for deterministic bug reproduction and regression benchmarking.

### 9. Heterogeneous Multi-Schema Engine & `UnexpectedFieldPolicy`
- Allow sessions within the same multi-session `Engine` to bind distinct compiled or runtime schemas.
- Add per-session `UnexpectedFieldPolicy` (`reject`, `ignore_with_metric`, `passthrough`) to handle unannounced venue/custom tags gracefully without downtime.

---

## Explicit Scope Boundaries (Non-Goals for Core)

To keep NimbleFIX fast, auditable, and embeddable (~336 KB Layer 0 core), the following are intentionally **outside** the core library scope:
- **Cross-host distributed HA / Raft journal replication**: If needed later, should live in an optional Layer 3 replication module rather than coupling into Layer 0/1.
- **FIX Router DSL / Version Translation / Rule-Based Drop Copy**: Better built as standalone applications/examples on top of NimbleFIX.
- **Web Management UI & Non-FIX Protocols (e.g., ITCH)**: Out-of-process tools can consume the telemetry/control interfaces; non-FIX binary market data protocols belong in separate libraries.
