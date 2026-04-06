# FastFix Benchmarking

This directory contains the benchmark sources, shared benchmark support code, and the helper script used to reproduce the published numbers.

## Files

- `main.cpp`: FastFix benchmark driver.
- `quickfix_main.cpp`: QuickFIX C++ comparison benchmark driver.
- `bench_support.h`: Bench-only support layer shared by both drivers for allocation counting, timing, perf counters, percentile summaries, and the neutral FIX44 business-order fixture.
- `bench.sh`: Build and run helper for the common benchmark workflows.

## Build And Run

The script is the intended entry point:

```bash
./bench/bench.sh build
./bench/bench.sh fastfix
./bench/bench.sh fastfix-ffd
./bench/bench.sh quickfix
./bench/bench.sh builder
./bench/bench.sh compare
```

What each command does:

- `build`: builds `fastfix-bench`, `fastfix-xml2ffd`, `fastfix-dictgen`, regenerates the FIX44 `.ffd/.art` files under `build/bench/`, regenerates the auxiliary `build/sample-basic-overlay.art`, configures/builds QuickFIX, and compiles `build/bench/quickfix-cpp-bench`.
- `fastfix`: runs the main FastFix suite against `build/bench/quickfix_FIX44.art`. If no extra args are supplied, it uses `--iterations 100000 --loopback 1000 --replay 1000`.
- `fastfix-ffd`: runs the same FastFix suite but loads `build/bench/quickfix_FIX44.ffd` directly at startup. Default args are `--iterations 30000 --loopback 200 --replay 200`.
- `quickfix`: runs the QuickFIX comparison benchmark against the same upstream `FIX44.xml`. Default args are `--iterations 100000`.
- `builder`: runs the main FastFix suite against `build/bench/quickfix_FIX44.art`, defaulting to `--iterations 100000 --loopback 0 --replay 0`. Use this when iterating on the object-to-wire encode comparison block (`builder-generic-e2e-buffer`, `builder-fixed-layout-buffer`, `builder-fixed-layout-hybrid-buffer`) without paying for loopback/replay noise.
- `compare`: runs the default FastFix artifact suite followed by the default QuickFIX comparison suite.

Prerequisites:

- `xmake`
- `cmake`
- `g++`
- `git` if `build/_deps/quickfix-src` does not already exist

## Measurement Boundaries

The object-to-wire encode comparisons are intentionally measured from a neutral business object to wire bytes.

- FastFix codec microbench metrics (`encode*`) build a `fastfix::message::Message` or `MessageBuilder` inside the timed region and then encode it.
- FastFix object-to-wire compare metrics (`builder-generic-e2e-buffer`, `builder-fixed-layout-buffer`, `builder-fixed-layout-hybrid-buffer`) build and populate the benchmark-side builder/writer inside the timed region and then call `encode_to_buffer()`.
- QuickFIX encode metrics build `FIX44::NewOrderSingle` inside the timed region and then serialize it.
- Both sides pin `SendingTime` to the fixture timestamp for the primary compare path, so the numbers stay focused on object construction plus serialization instead of per-iteration clock formatting.
- Parse metrics start from a complete FIX frame and measure parse into the protocol object model.

This avoids comparing one side's pure serializer against the other side's object-construction path.

## Benchmark Groups

### FastFix Main Suite

- `parse`: FIX44 wire frame to `fastfix::message::Message`.
- `encode`: business object to `Message`, then encode to a fresh string.
- `encode-buffer`: business object to `Message`, then encode into a reused output buffer.
- `encode-buffer-time-fixed`: same as `encode-buffer`, but `SendingTime` is fixed instead of regenerated each iteration.
- `encode-buffer-template`: same as `encode-buffer`, but the template lookup is cached.
- `encode-buffer-time-fixed-template`: combines fixed `SendingTime` with cached template lookup.
- `encode-buffer-time-fixed-template-no-float`: same as above, with the float field omitted from the sample order.
- `encode-buffer-precompiled-table`: encode path using the precompiled field table.
- `peek`: parse header-enough information for lightweight inspection.
- `session-inbound`: decode plus admin/session handling for an inbound application frame.
- `replay`: resend/recovery path across a span of stored messages.
- `loopback-roundtrip`: initiator sends `NewOrderSingle (35=D)` over local TCP loopback and waits until the acceptor returns `ExecutionReport (35=8)` order ack.

### FastFix Object-To-Wire Encode Compare

These are the three FastFix encode methods intended for direct comparison against the QuickFIX encode metrics.

- `builder-generic-e2e-buffer`: generic `MessageBuilder` path from business object to wire.
- `builder-fixed-layout-buffer`: fixed-layout hot path from business object to wire.
- `builder-fixed-layout-hybrid-buffer`: fixed-layout hot path plus extra-field hybrid path from business object to wire.

All three use fixed `SendingTime` and `encode_to_buffer()` so they stay on the same comparison boundary as QuickFIX's buffer-based metric.

### QuickFIX Comparison Suite

- `quickfix-parse`: FIX44 wire frame to `FIX44::NewOrderSingle`.
- `quickfix-encode`: business object to `FIX44::NewOrderSingle`, then serialize to a fresh string.
- `quickfix-encode-buffer`: same serializer, but write into a reused caller-owned output buffer.

QuickFIX C++ exposes one real FIX serializer with two call styles (`toString()` and `toString(buffer)`). Both metrics use fixed `SendingTime` to match the FastFix object-to-wire compare block.

## Latest Full Compare Snapshot

The full default suite was rerun on 2026-04-06 with `./bench/bench.sh compare`. The raw output from that run is saved at `build/bench/latest-compare.txt`.

### Cross-Engine Summary

| Comparable boundary | FastFix metric | FastFix avg_ns | QuickFIX metric | QuickFIX avg_ns | Delta vs QuickFIX | FastFix alloc/op | QuickFIX alloc/op |
|------|------|--------|------|--------|------------------|------------------|-------------------|
| parse | `parse` | 1314.35 | `quickfix-parse` | 846.74 | +55.2% slower | 2.00 | 15.00 |
| generic object-to-wire | `builder-generic-e2e-buffer` | 1337.90 | `quickfix-encode-buffer` | 1263.39 | +5.9% slower | 9.00 | 29.00 |
| fixed-layout object-to-wire | `builder-fixed-layout-buffer` | 684.05 | `quickfix-encode-buffer` | 1263.39 | 45.9% faster | 13.00 | 29.00 |
| hybrid object-to-wire | `builder-fixed-layout-hybrid-buffer` | 754.13 | `quickfix-encode-buffer` | 1263.39 | 40.3% faster | 15.00 | 29.00 |

QuickFIX still leads the raw parse path, but the current FastFix fixed-layout and hybrid order-to-wire paths are ahead of QuickFIX's buffer serializer on the same business-object-to-wire boundary.

### FastFix Suite Snapshot

| Metric | avg_ns | p50_ns | p99_ns | alloc/op | ops/sec |
|--------|--------|--------|--------|----------|---------|
| `encode` | 1471.15 | 1383 | 1864 | 14.00 | 679,741 |
| `encode-buffer` | 1490.47 | 1453 | 1684 | 5.00 | 670,930 |
| `encode-buffer-time-fixed` | 1412.62 | 1382 | 1743 | 5.00 | 707,906 |
| `encode-buffer-template` | 1224.99 | 1172 | 1393 | 5.00 | 816,336 |
| `encode-buffer-time-fixed-template` | 1185.23 | 1142 | 1363 | 5.00 | 843,718 |
| `encode-buffer-time-fixed-template-no-float` | 1179.28 | 1122 | 1422 | 5.00 | 847,973 |
| `encode-buffer-precompiled-table` | 1430.97 | 1382 | 1603 | 9.00 | 698,828 |
| `builder-generic-e2e-buffer` | 1337.90 | 1333 | 1543 | 9.00 | 747,443 |
| `builder-fixed-layout-buffer` | 684.05 | 651 | 752 | 13.00 | 1,461,876 |
| `builder-fixed-layout-hybrid-buffer` | 754.13 | 721 | 771 | 15.00 | 1,326,035 |
| `peek` | 198.26 | 180 | 191 | 0.00 | 5,043,875 |
| `parse` | 1314.35 | 1283 | 1313 | 2.00 | 760,833 |
| `session-inbound` | 2980.57 | 2765 | 4688 | 2.00 | 335,507 |
| `replay` | 546176.55 | 532382 | 622737 | 257.00 | 1,831 |
| `loopback-roundtrip` | 26486.70 | 25657 | 32269 | 7.00 | 37,755 |

The same run also reported `335,506.73` inbound messages/s on `session-inbound` and `234,356.46` replay frames/s across the 128-frame resend batches.

### QuickFIX Suite Snapshot

| Metric | avg_ns | p50_ns | p99_ns | alloc/op | ops/sec |
|--------|--------|--------|--------|----------|---------|
| `quickfix-parse` | 846.74 | 761 | 1152 | 15.00 | 1,181,002 |
| `quickfix-encode` | 1297.66 | 1202 | 1332 | 30.00 | 770,620 |
| `quickfix-encode-buffer` | 1263.39 | 1163 | 1213 | 29.00 | 791,521 |

## Metric Fields

Each result line prints the same metric schema.

- `count`: number of benchmark iterations.
- `total_ns`: wall-clock nanoseconds for the full benchmark run.
- `cpu_ns`: process CPU nanoseconds consumed during the run.
- `cpu_pct`: `cpu_ns / total_ns` as a percentage.
- `allocs`: heap allocation count captured through the benchmark-local `operator new` hooks.
- `alloc_bytes`: total bytes allocated through those hooks.
- `avg_ns`: average wall-clock nanoseconds per iteration.
- `min_ns`, `p50_ns`, `p95_ns`, `p99_ns`, `p999_ns`, `max_ns`: percentile summary over the per-iteration samples.
- `ops_per_sec`: iterations per second derived from wall time.
- `cache_miss`, `branch_miss`: Linux `perf_event_open` counters when available, otherwise `n/a`.
- `<work_label>` and `<work_label>_per_sec`: additional work counters used by benchmarks such as replay where one iteration may emit multiple outbound frames.

## Output Artifacts

- `build/bench/quickfix_FIX44.ffd`: FastFix text dictionary generated from QuickFIX `FIX44.xml`.
- `build/bench/quickfix_FIX44.art`: compiled FastFix artifact for the main FIX44 comparison suite.
- `build/sample-basic-overlay.art`: auxiliary merged sample profile artifact still generated during `build`; it is not required by the current FIX44 QuickFIX comparison suite.
- `build/bench/quickfix-cpp-bench`: compiled QuickFIX comparison binary.

If you need the wider development workflow or tool inventory, see `docs/development.md`. This file is only about the benchmark subsystem.