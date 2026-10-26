# Stage 3 development baseline

Measured code: `e5bd312378fe0d3e1b298fceee165cc0d9ed3268`.
All 14 benchmark reports identify source SHA-256
`8078a940c297608c80ebcaa1980eda1dd0ee84fefef1a831c2554a73629f8507`.
The first collection started from a clean worktree; subsequent changes only
add these results. Binary hashes are in the individual reports.

This is an initial baseline on a running development desktop, with no latency
SLO, wire timestamp, relative speedup claim or QuickFIX performance comparison.
The [acceptance record](../../../docs/session-tcp-slice.md) and
[support matrix](../../../docs/session-support.md) define the functional scope.

## Conditions

- AMD Ryzen 7 7840HS, 8 cores / 16 logical CPUs; affinity 0–15, unpinned.
- GCC 16.2.1, xmake release `-O3`, C++20, default static NimbleFIX library;
  Linux 7.2.8-9-omarchy, glibc 2.44. Full `lscpu` output is in the reports.
- CPU governor `performance`, boost and SMT enabled; see
  [host settings](host-settings.json). Background desktop activity was present.
- Journal files on workspace **btrfs**, under `build/bench-state`;
  fresh preallocated 16 MiB journals per run, confirmation `appended`.
  No per-message fsync/fdatasync/msync and no power-loss durability claim.
- Codec: 10,000 samples/case/run, three runs, 1,000 warmups/case.
  Session: 2,000 round trips/run, three runs, 100 warmups/run.
- TCP/echo: 2,000 requests/run, three runs at each offered rate (2,000 and
  20,000/s), alternating engine and echo; Logon precedes load, with no business
  warmup. TCP_NODELAY on both peers. Python uses open-loop scheduled arrivals.

## Observations

Values below are medians of the three **per-run** percentiles. Keep the
individual reports and raw samples when comparing; percentile aggregation
does not describe one combined distribution.

| Boundary/case | p50 | p99 | Allocation/sync observation |
| --- | ---: | ---: | --- |
| L1 fixed encode | 210 ns | 221 ns | 0 observed allocation calls/op |
| L1 fixed reset/build/encode | 230 ns | 301 ns | 0 observed allocation calls/op |
| L1 fluent build/encode | 250 ns | 310 ns | 3 allocation calls/op |
| L1 generated named build/encode | 331 ns | 361 ns | 3 allocation calls/op |
| L1 frame inspection + flat parse | 321 ns | 321 ns | 0 observed allocation calls/op |
| L1 independent minimal parse/encode | 261 ns | 271 ns | Separate implementation and work boundary |
| L1 8,192-message rotating fixed encode | 220 ns | 340 ns | 0 observed allocation calls/op |
| L2 two-driver journaled round trip | 5.391 µs | 15.149 µs | 0 allocation and 0 sync calls/round trip |
| L3 TCP, offered 2,000/s | 0.584 ms | 1.259 ms | All 2,000 replies/run, server errors=0 |
| L0 raw echo, offered 2,000/s | 0.572 ms | 1.215 ms | All 2,000 replies/run; no codec/session/store |
| L3 TCP, offered 20,000/s | 0.681 ms | 3.749 ms | All 2,000 replies/run, server errors=0 |
| L0 raw echo, offered 20,000/s | 0.639 ms | 3.533 ms | All 2,000 replies/run; no codec/session/store |

The fixed-session path uses two real Drivers, two journals and two inline
callbacks, with six committed records per round trip and fake transport.
Each run recorded 486 minor faults and zero major faults during measurement;
these costs are included. Allocator hooks observe executable malloc/new,
with the shared-library/aligned-allocation limitations described in the
acceptance record. Sync hooks assert zero wrapped fsync/fdatasync/msync calls;
this is not a complete syscall trace.

TCP latency runs from the generator's scheduled arrival until a complete
reply is parsed. Python framing, scheduling, polling and loopback are included.
The workload is minimal flat D traffic and an 8 response carrying tag 11,
using a fluent reply builder; it is separate from the full dictionary-checked
order/report acceptance flow. Observed rates were 1,998–2,001/s for the low
rate and 19,772–19,915/s for the high rate across both servers. The high-rate
tails vary substantially between runs. Echo and engine p99 must not be
subtracted to infer engine overhead or used as a speed ranking.

The controlled S/K/B cases are in [codec/report.json](codec/report.json):
S=2/K=2/B=8, S=8/K=8/B=8, S=32/K=2/B=8, S=32/K=32/B=8 and
S=8/K=8/B=128, with zero observed allocation calls for all five cases.
The [schema-size probe](schema-size.json) keeps a two-field shape at 40 builder
bytes / 32 slot bytes and a 16,232-byte probe binary while adding 1,000 unused
optional fields and widening tag spacing. Shared generated-header size and
compile time increase independently. This probe compiles with `-O2` and is
structural evidence rather than another latency result. Its builder size is
`sizeof` and excludes the fluent builder's owned heap backing.

## Reproduce

```sh
xmake f -c -m release --toolchain=gcc --sanitize=n --fuzzers=n --benchmarks=y -o build
xmake build
python3 bench/run.py --binary build/linux/x86_64/release/codec-bench \
  --samples 10000 --runs 3 --output build/results/codec
python3 bench/session_run.py --binary build/linux/x86_64/release/session-bench \
  --samples 2000 --runs 3 --output build/results/session
python3 bench/tcp_run.py --binary build/linux/x86_64/release/tcp-bench-server \
  --samples 2000 --rate 2000 --output build/results/tcp
python3 bench/tcp_run.py --binary build/linux/x86_64/release/null-tcp-server \
  --echo --samples 2000 --rate 2000 --output build/results/echo
python3 tests/schema_size_test.py --output build/results/schema-size.json
```

Repeat the last two network commands three times with separate output paths,
then repeat at `--rate 20000`. Run measurements sequentially after other build
and test work completes. Codec/session raw CSV is gzip-compressed; each TCP
and echo run contains a CSV and JSON report. Dedicated-machine performance,
full saturation/slow-peer latency curves, hardware counters, lock/syscall
tracing and long-duration background-write/rotation costs remain later gates.
