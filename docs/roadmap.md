# NimbleFIX Roadmap

This document tracks the planned engineering priorities for NimbleFIX after `0.3`, focusing on **single-core hot-path latency** first, followed by **multi-core scalability, operational resilience, and zero-overhead observability**, while keeping the 3-layer pay-only-for-what-you-need C++20 core lean and embeddable.

---

## Phase 1 (Current Priority): Single-Core Hot-Path Performance

Goal: Push both outbound encoding and inbound strict parsing/validation firmly into the **sub-microsecond** regime while preserving 0 steady-state heap allocations and FIX protocol correctness.

### 1. Expected-Order Fast-Path Parser & Validator
- **Motivation**: Full 912-field `FIX44.xml` strict parsing + validation currently takes `~861 ns – 1.32 µs`. Most counterparties emit fields in a predictable canonical/venue order.
- **Design**:
  - Codegen emits expected-order field transition hints alongside the existing compact member-index tables.
  - Inbound parser/validator checks the next expected tag with an O(1) cursor comparison first; out-of-order (still FIX-valid) tags seamlessly fall back to the existing compact table lookup without losing strictness.
  - Reduce redundant per-field header/timestamp re-validation on the hot path.
- **Target**: Strict 912-field parse + validate + typed view read under **~400–600 ns** (`~2×` faster than `0.3`).

### 2. [Done] Zero-Intermediate-Copy Streaming Encoder (`StreamWriter` / `send_stream`)
- **Status**: Implemented and verified across Layer 0/1/2 (`encode_stream`, `encode_fix44_stream`, `encode_body_stream`, `DirectInitiator::send_stream`/`enqueue_stream`, `DirectSendQueue::try_push_stream`, `SessionContext`/`SessionDriver`/`Sender`/`ProducerLane::send_stream`); legacy `make()` and `Draft` builders removed (`Fixed` preserved).
- **Measured Result**: Typical `NewOrderSingle` down to **130 ns p50** (0 alloc), `NewOrderSingle + NoPartyIDs[1]` **140–211 ns** (**6.9M msgs/s**, **7.9–8.6× faster than QuickFIX 1.16.0**), `DirectInitiator::send_stream` + mmap Journal **281 ns p50** (**3.47M msgs/s**), 32-order TCP burst **1.33M–1.43M ord/s** (**12.69× lower CO-Free p50 than QuickFIX**).

### 3. Sub-Millisecond Timestamps (`µs` / `ns`) & Optional Checksum Bypass
- **Timestamps**: Extend `FastUtcClock` and session configuration to support per-session **millisecond (`ms`), microsecond (`µs`), and nanosecond (`ns`)** precision for `SendingTime (52)` / `TransactTime (60)` (required by MiFID II and modern low-latency venues; parser/validator already accepts all 4 FIX precisions).
- **Optional Checksum Skip**: Allow explicit per-session opt-in to skip inbound FIX `CheckSum (10)` verification on trusted internal/dedicated links (saving `~150–200 ns` per frame; enabled by default for safety).

### 4. Log-Driven Schema Optimization Tooling
- Extend `tools/schema/generate.py` (building on existing `--messages` subset pruning) with a log-analyzer helper that scans sample production FIX logs and generates a minimal venue schema containing only observed messages, fields, and groups to maximize L1/L2 cache density.

---

## Phase 2: Multi-Core Scalability, Resilience & Observability

### 5. Initiator Multi-Endpoint Failover (`ConnectionStrategy`)
- Support an ordered list of counterparty endpoints (`host:port`) on both `DirectInitiator` and `Engine` initiators with configurable reconnection policies (`ordered` primary-first, `round_robin`, backoff, and retry caps) for active/standby venue gateways.

### 6. Sharded Reactor Multi-Core Scaling & Core Affinity
- Implement the **Sharded Reactor** model outlined in [architecture.md](architecture.md): `K` independent `IoBackend` (`epoll` / `busy_poll` / `io_uring`) worker shards with static session-to-shard pinning, preserving strict single-writer lock-free `SessionCore` and mmap Journal ownership per shard.
- Add configurable CPU core pinning and idle pausing strategies (`busy_spin`, `hybrid_backoff`, `sleep`).
- **Target**: Scale multi-session gateway throughput from single-core `146k ord/s` to **500k–1M+ ord/s** across hundreds of concurrent sessions.

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
