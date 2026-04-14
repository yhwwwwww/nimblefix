# FastFix Benchmarking

This directory contains the benchmark sources, shared benchmark support code, and the helper script used to reproduce the published numbers.

## Files

- `main.cpp`: FastFix benchmark driver.
- `quickfix_main.cpp`: QuickFIX C++ comparison benchmark driver.
- `bench_support.h`: Bench-only support layer shared by both drivers for allocation counting, timing, perf counters, percentile summaries, and the neutral FIX44 business-order fixture.
- `bench.sh`: Build and run helper for the common benchmark workflows.

## Build And Run

The script is the intended entry point and auto-selects `xmake`, then `cmake + Ninja`, then `cmake + make`:

```bash
./bench/bench.sh build
./bench/bench.sh fastfix
./bench/bench.sh fastfix-ffd
./bench/bench.sh quickfix
./bench/bench.sh builder
./bench/bench.sh compare

# Alternative CMake path
FASTFIX_BUILD_SYSTEM=cmake FASTFIX_CMAKE_PRESET=dev-release ./bench/bench.sh build
FASTFIX_BUILD_SYSTEM=cmake FASTFIX_CMAKE_PRESET=dev-release ./bench/bench.sh fastfix
FASTFIX_BUILD_SYSTEM=cmake FASTFIX_CMAKE_PRESET=dev-release ./bench/bench.sh fastfix-ffd
FASTFIX_BUILD_SYSTEM=cmake FASTFIX_CMAKE_PRESET=dev-release ./bench/bench.sh quickfix
FASTFIX_BUILD_SYSTEM=cmake FASTFIX_CMAKE_PRESET=dev-release ./bench/bench.sh builder
FASTFIX_BUILD_SYSTEM=cmake FASTFIX_CMAKE_PRESET=dev-release ./bench/bench.sh compare

# Force the make fallback
FASTFIX_BUILD_SYSTEM=cmake FASTFIX_CMAKE_GENERATOR=make FASTFIX_CMAKE_PRESET=dev-release ./bench/bench.sh build
```

Every benchmark command above intentionally uses QuickFIX FIX44 inputs: `bench/vendor/quickfix/spec/FIX44.xml`, `build/bench/quickfix_FIX44.ffd`, or `build/bench/quickfix_FIX44.art`. The sample profile artifact is only a shared test/codegen asset and is not a benchmark input.

What each command does:

- `build`: builds `fastfix-bench`, `fastfix-xml2ffd`, `fastfix-dictgen`, regenerates the FIX44 `.ffd/.art` files under `build/bench/`, and compiles the QuickFIX comparison benchmark from the pinned submodule checkout using the selected build system. By default it auto-selects `xmake`, then `cmake + Ninja`, then `cmake + make`.
- `fastfix`: runs the main FastFix suite against `build/bench/quickfix_FIX44.art`. If no extra args are supplied, it uses `--iterations 100000 --loopback 1000 --replay 1000`.
- `fastfix-ffd`: runs the same FastFix suite but loads `build/bench/quickfix_FIX44.ffd` directly at startup. Default args are `--iterations 30000 --loopback 200 --replay 200`.
- `quickfix`: runs the QuickFIX comparison benchmark against the pinned `bench/vendor/quickfix/spec/FIX44.xml`. Default args are `--iterations 100000`.
- `builder`: runs the main FastFix suite against `build/bench/quickfix_FIX44.art`, defaulting to `--iterations 100000 --loopback 0 --replay 0`. Use this to iterate on the encode metric without loopback/replay noise.
- `compare`: runs the default FastFix artifact suite followed by the default QuickFIX comparison suite.

Prerequisites:

- `xmake` for the preferred path
- `g++`
- CMake 3.20+ plus Ninja or make if you select `FASTFIX_BUILD_SYSTEM=cmake`

The default path is now fully offline after `git submodule update --init --recursive`: Catch2, pugixml, QuickFIX, and `FIX44.xml` are all pinned inside this repository checkout.

## Measurement Boundaries

Encode comparisons measure from a neutral business object to wire bytes. Both FastFix and QuickFIX pin `SendingTime` to a fixture timestamp so numbers stay focused on object construction plus serialization instead of per-iteration clock formatting.

- FastFix `encode`: uses the codegen-generated typed writer (backed by `FixedLayoutWriter`) to populate fields from a business object, then calls `encode_to_buffer()`.
- QuickFIX `quickfix-encode-buffer`: builds `FIX44::NewOrderSingle` from the same business object, then serializes into a reused output buffer.
- Parse metrics start from a complete FIX frame and measure parse into the protocol object model.

## Benchmark Metrics

### FastFix Suite

| Metric | What it measures |
|--------|-----------------|
| `encode` | Business object → generated typed writer → wire bytes (reused buffer) |
| `peek` | Extract session header from raw FIX frame without full parse |
| `parse` | FIX44 wire frame → `fastfix::message::Message` |
| `session-inbound` | Full session layer: decode + admin/session handling for an inbound application frame |
| `replay` | Resend/recovery path across a span of stored messages |
| `loopback-roundtrip` | Initiator sends NewOrderSingle over TCP loopback, waits for ExecutionReport ack |

### QuickFIX Comparison Suite

| Metric | What it measures |
|--------|-----------------|
| `quickfix-parse` | FIX44 wire frame → `FIX44::NewOrderSingle` |
| `quickfix-encode` | Business object → `FIX44::NewOrderSingle` → fresh string |
| `quickfix-encode-buffer` | Same as above, but into a reused caller-owned output buffer |
| `quickfix-session-inbound` | Full session layer: `Session::next()` on an application frame including sequence validation and store |
| `quickfix-replay` | In-process ResendRequest replay across a span of stored messages |
| `quickfix-loopback` | Real TCP loopback using `ThreadedSocketAcceptor`; measures full round-trip RTT |

## Latest Full Compare Snapshot

The full default suite was rerun on 2026-04-13 with `./bench/bench.sh compare` after the compiled decoder, T1-fix, and hot-path optimisation rounds. The raw output from that run is saved at `build/bench/latest-compare.txt`.

### Cross-Engine Summary

| Boundary | FastFix metric | QuickFIX metric | FastFix p50 | FastFix p95 | QuickFIX p50 | QuickFIX p95 |
|----------|----------------|-----------------|-------------|-------------|--------------|--------------|
| encode (object → wire) | `encode` | `quickfix-encode-buffer` | 431 ns | 471 ns | ~1.25 µs | ~1.43 µs |
| parse (wire → object) | `parse` | `quickfix-parse` | 711 ns | 742 ns | ~1.25 µs | ~1.30 µs |
| session-inbound | `session-inbound` | `quickfix-session-inbound` | 1.68 µs | 1.98 µs | 2.32 µs | 2.44 µs |
| replay (128 msgs) | `replay` | `quickfix-replay` | 328 µs | 381 µs | 233 µs | 240 µs |
| loopback RTT | `loopback-roundtrip` | `quickfix-loopback` | 20.29 µs | 25.24 µs | 20.30 µs | 24.68 µs |

FastFix now beats QuickFIX on session-inbound by 1.38x (1.68 µs vs 2.32 µs). The codec layer shows a 2-3x advantage: encode is ~2.9x faster via the generated typed writer and parse is ~1.8x faster with the compiled decoder. QuickFIX replay is faster wall-time but allocates 4,117/op vs 0/op for FastFix — it returns stored string copies per message rather than re-encoding from the store. Wire-to-wire loopback RTT is roughly equal since kernel TCP overhead dominates at ~20 µs.
### FastFix Suite Snapshot

| Metric | p50 | p95 | p99 | alloc/op | ops/sec | cache/op | branch/op |
|--------|-----|-----|-----|----------|---------|----------|-----------|
| `encode` | 431 ns | 471 ns | 661 ns | 0 | 2,150,000 | 0.0 | 0.0 |
| `peek` | 140 ns | 141 ns | 141 ns | 0 | 6,330,000 | 0.0 | 0.0 |
| `parse` | 711 ns | 742 ns | 882 ns | 0 | 1,360,000 | 0.0 | 0.0 |
| `session-inbound` | 1.68 µs | 1.98 µs | 3.29 µs | 0 | 530,000 | 0.9 | 0.1 |
| `replay` | 328 µs | 381 µs | 387 µs | 0 | 3,000 | 19.7 | 197.1 |
| `loopback-roundtrip` | 20.29 µs | 25.24 µs | 29.50 µs | 3 | 47,000 | 16.1 | 34.4 |

### QuickFIX Suite Snapshot

| Metric | p50 | p95 | p99 | alloc/op | ops/sec | cache/op | branch/op |
|--------|-----|-----|-----|----------|---------|----------|-----------|
| `quickfix-parse` | 1.25 µs | 1.30 µs | 1.85 µs | 20 | 736,100 | 0.1 | 0.0 |
| `quickfix-encode` | 1.27 µs | 1.44 µs | 1.62 µs | 30 | 690,300 | 0.0 | 0.0 |
| `quickfix-encode-buffer` | 1.25 µs | 1.43 µs | 1.45 µs | 29 | 723,200 | 0.0 | 0.0 |
| `quickfix-session-inbound` | 2.32 µs | 2.44 µs | 3.11 µs | 18 | 421,400 | 0.5 | 0.2 |
| `quickfix-replay` | 233 µs | 240 µs | 242 µs | 4,117 | 4,300 | 12.1 | 215.2 |
| `quickfix-loopback` | 20.30 µs | 24.68 µs | 31.64 µs | 77 | 45,900 | 14.5 | 16.4 |

## Metric Fields

Each result is reported in two forms: a compact aligned table and a verbose single-line summary.

### Table columns

The printed table (via `PrintResultTable`) contains these columns:

| Column | Description |
|--------|-------------|
| `Metric` | Benchmark name |
| `Count` | Number of iterations |
| `p50`, `p95`, `p99` | Latency percentiles formatted as `ns` or `µs` |
| `Alloc/op` | Heap allocations per iteration (from global `operator new` hooks) |
| `Ops/sec` | Throughput derived from total wall time |
| `Cache/op` | L1/LLC cache misses per iteration from Linux `perf_event_open`; `n/a` when unavailable |
| `Branch/op` | Branch mispredictions per iteration from Linux `perf_event_open`; `n/a` when unavailable |

### Verbose line fields

Each benchmark also emits a single raw line with the full counter set:

- `count`: number of benchmark iterations.
- `total_ns`: wall-clock nanoseconds for the full benchmark run.
- `cpu_ns`: process CPU nanoseconds consumed during the run.
- `cpu_pct`: `cpu_ns / total_ns` as a percentage.
- `allocs`: total heap allocation count over all iterations.
- `alloc_bytes`: total bytes allocated over all iterations.
- `avg_ns`: average wall-clock nanoseconds per iteration.
- `min_ns`, `p50_ns`, `p95_ns`, `p99_ns`, `p999_ns`, `max_ns`: percentile summary over per-iteration `steady_clock` samples.
- `ops_per_sec`: iterations per second derived from wall time.
- `cache_miss`, `branch_miss`: total Linux `perf_event_open` hardware counters for the full run; omitted on non-Linux platforms.
- `<work_label>`, `<work_label>_per_sec`: optional extra throughput counters for benchmarks where one iteration produces multiple output units (e.g. `frames` for the replay path).

## Output Artifacts

- `build/bench/quickfix_FIX44.ffd`: FastFix text dictionary generated from QuickFIX `FIX44.xml`.
- `build/bench/quickfix_FIX44.art`: compiled FastFix artifact for the main FIX44 comparison suite.
- `build/sample-basic.art`: auxiliary merged sample profile artifact used by shared test/codegen flows; benchmark commands do not consume it.
- `build/linux/x86_64/release/quickfix-cpp-bench`: xmake output for the QuickFIX comparison binary.
- `build/cmake/<preset>/bin/quickfix-cpp-bench`: Ninja-based CMake output for the QuickFIX comparison binary.
- `build/cmake/<preset>-make/bin/quickfix-cpp-bench`: make-based CMake fallback output for the QuickFIX comparison binary.

If you need the wider development workflow or tool inventory, see `docs/development.md`. This file is only about the benchmark subsystem.