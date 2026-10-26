# Initial codec baseline

Measured implementation: `059c3f345b5703eec917bac2c0a8fc6f4ff45526`.
Source digest: `238235426a9d47f08042a1c61d9e338413d1bc4cf6dfc1f559bff0d1b42aa502`.
The [JSON report](report.json) includes environment information and all three
runs; each gzip file contains 80,000 individual CSV samples.

These are exploratory L1 codec measurements on an unpinned AMD Ryzen 7 7840HS
Linux development machine, GCC 16.2.1, xmake release (`-O3`, C++20).
SMT/turbo/governor and other host workloads were not controlled. Each case has
1,000 warmup operations and 10,000 measured operations per run. Time boundaries
are userspace `steady_clock` and include clock/allocation-hook overhead.
No session, journal, network, wire timestamp or SLO is measured.

The table takes the median of each run's percentile, rather than pooling runs
or selecting the fastest run. Differences between generated names and generated
tags have no statistical interpretation on this host; they use the same plan
and validation and should be remeasured on a controlled machine.

| Case | p50 ns | p99 ns | p99.9 ns | Allocations/op | Allocated bytes/op |
| --- | ---: | ---: | ---: | ---: | ---: |
| Clock baseline | 40 | 41 | 41 | 0 | 0 |
| Fixed-frame byte copy | 40 | 51 | 71 | 0 | 0 |
| Fixed shape encode | 351 | 421 | 821 | 0 | 0 |
| Fixed shape reset/build/encode | 391 | 430 | 661 | 0 | 0 |
| Flat fluent build/encode | 421 | 451 | 722 | 3 | 560 |
| Generated named build/encode | 591 | 912 | 1122 | 3 | 560 |
| Generated static-tag build/encode | 592 | 721 | 2024 | 3 | 560 |
| Inspect + flat parse | 601 | 612 | 1903 | 0 | 0 |

Allocation bytes sum all three geometric allocations, not peak live storage.
The hook covers direct executable `malloc` and replaced C++ `new`, excluding
shared-library internals and aligned allocation. The driver rejects allocation
regressions in the fixed and flat-parse cases. Throughput, maxima and minor/major
page-fault deltas remain in the JSON; syscall, lock and hardware cache counters
were not available. The parser revalidates the inspected frame, so this case
includes two checksum validations. Do not subtract percentile values to infer
engine overhead. The null case is byte copying, without a standalone minimal
parser/encoder implementation.

The [schema probe](schema-size.json) fixes a two-field shape and adds either zero
or 1,000 unused optional fields. Builder size remains 40 bytes and its two slots
remain 32 bytes, including when unused tag IDs are much farther apart. Generated
header size grows from 3,479 to 421,167/432,525 bytes. The probe binary is 16,232
bytes, but it only observes compile-time size/slot metadata; it does not measure
an instantiated encoder's binary footprint or per-message arena allocation.
The complete benchmark executable is 63,936 bytes. Compile/generation timings
are one-off observations; no compile-time regression budget is inferred.

Reproduce after building with xmake:

```sh
xmake f -m release --toolchain=gcc --sanitize=n --fuzzers=n --benchmarks=y -o build
xmake build codec-bench
python3 bench/run.py --binary build/linux/x86_64/release/codec-bench \
  --samples 10000 --runs 3 --output /tmp/nimblefix-codec-results
python3 tests/schema_size_test.py --output /tmp/nimblefix-codec-results/schema-size.json
```

`worktree_dirty=true` reflects the untracked measurement files produced while
collecting these results. No implementation source changed after the recorded
code commit. Full FIX44/QuickFIX interoperability, cold/mixed working sets,
the complete shape/presence/payload matrix and controlled-machine profiling
remain pending. Fluent allocation is a measured limitation to address before
claiming an allocation-free default engine path.
