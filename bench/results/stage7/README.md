# Stage 7 release measurements

These are development-host baselines for the 0.2 source API freeze. Code and
harness revision: `2b1392acac1ddac2d4e2d9dfc4e4f31f1aa2c2f0`. The prior schema profile is
`9b37ad85c3a80162f56a937a171f4f4252d26d5b`. Reports record source/binary hashes,
commands, configuration, raw samples and per-run results. Measurements ran
sequentially from a clean source tree under ignored `build/` output; this tree
was then relocated here. The later results commit changes evidence only.
Archive link normalization additionally uses `e1ff3a8`; that postprocessing
fix resolves report-relative filenames without changing raw sample bytes.

Three runs use 5,000 measured samples each. Tables show the median of the three
run quantiles, rather than a percentile of pooled samples. Maximums are the
worst observed across runs. Small samples do not support p99.99 estimates.

## Environment and boundaries

AMD Ryzen 7 7840HS (8 cores / 16 threads), Linux 7.2.8, GCC 16.2.1,
C++20 / xmake release `-O3`, glibc 2.44. Storage is btrfs on a LUKS-mapped
SHPP41-2000GM NVMe device; NimbleFIX session runs use appended mmap journals.
The existing performance governor, SMT and boost were kept. Microbenchmarks
use CPU 0; network/runtime processes inherit CPUs 0–3. This is a shared laptop,
not an isolated performance server. [host.json](host.json) records settings.

There is no per-message fsync/fdatasync/msync and no power-loss guarantee.
Timing is user-space monotonic time, with no hardware wire timestamps. The
independent Python peers include their scheduling, framing and parsing costs.
Scheduled arrival timestamps are retained even when a send window delays
actual generation; lateness and actual-origin RTT are reported separately.
All planned requests must complete for a run to pass.

## Real runtime: Inline, Queued and three send entries

[Runtime report](runtime/report.json): full Engine/driver/codec, loopback TCP,
v2 journal, FlatSchema, 32 payload bytes, 2,000 requests/s, 100 warmups, four
producer threads / four Queued workers, window 64. The callback workload is
D→8, scheduled Python request to final parsed peer reply. Sender/lane workloads
are D→8, scheduled producer time to the inbound reply callback in the Engine.
These boundaries explain why callback and producer rows differ.

The journal budget is 256 MiB, with sixteen 1 MiB segment slots, retained
outbound window 8,192, and max(65,536, 8×(samples+warmup)+1,024) record slots.
Receipts and sample buffers are preallocated for the run. This sizing is part
of the benchmark configuration, not a change to normal Engine defaults.

| Mode/source | p50 μs | p99 μs | p99.9 μs | worst μs | completed/s | diagnostic peak RSS MiB |
| --- | --- | --- | --- | --- | --- | --- |
| inline/callback | 583.572 | 1,161.557 | 1,695.609 | 3,928.802 | 1,999.6 | 44.2 |
| inline/mpsc | 118.159 | 191.670 | 815.104 | 2,370.411 | 2,000.3 | 44.2 |
| inline/lane | 128.070 | 193.791 | 678.228 | 1,495.953 | 2,000.3 | 44.2 |
| queued-journal/callback | 608.832 | 1,166.902 | 1,243.769 | 2,867.077 | 1,999.9 | 44.2 |
| queued-journal/mpsc | 120.344 | 194.430 | 651.429 | 1,510.687 | 2,000.3 | 44.2 |
| queued-journal/lane | 118.231 | 188.534 | 498.055 | 1,489.178 | 2,000.3 | 44.2 |
| queued-ring/callback | 611.849 | 1,166.805 | 1,336.038 | 3,242.076 | 1,999.6 | 44.2 |
| queued-ring/mpsc | 126.357 | 200.315 | 927.873 | 1,854.475 | 2,000.3 | 44.2 |
| queued-ring/lane | 158.778 | 213.962 | 663.493 | 1,593.391 | 2,000.3 | 44.3 |

Allocation, mutex, sync and PMU counters come from separate diagnostic
processes. They include phase coordination, producer wakeup/polling, receipt
polling and segment activation. They are not pure per-message service costs.
Peak RSS includes startup and preallocated samples/indexes. Linker hooks cover
executable/static objects; calls wholly inside shared libraries can bypass
hooks. The timed executable retains the disabled send-wrapper flag check.

| Mode/source | allocation calls / whole phase | mutex calls / sample | fsync / fdatasync / msync |
| --- | --- | --- | --- |
| inline/callback | 16 | 0.0000 | 0/0/0 |
| inline/mpsc | 21 | 6.9988 | 0/0/0 |
| inline/lane | 16 | 0.0000 | 0/0/0 |
| queued-journal/callback | 16 | 11.9232 | 0/0/0 |
| queued-journal/mpsc | 21 | 14.9966 | 0/0/0 |
| queued-journal/lane | 16 | 7.9966 | 0/0/0 |
| queued-ring/callback | 16 | 11.8836 | 0/0/0 |
| queued-ring/mpsc | 21 | 14.9972 | 0/0/0 |
| queued-ring/lane | 16 | 7.9968 | 0/0/0 |

The ordinary Sender uses a bounded MPSC mutex queue. Explicit ProducerLane
uses one producer per lane and atomics for its queue, wake admission and
receipt publication. Inline lane/callback phases show no pthread mutex-lock
calls. Nonzero phase allocation totals are preserved; the zero-allocation
claim is limited to the tested fixed-message codec/session paths below.

Per-sample kernel-send diagnostics preserve API start, a timestamp sampled
after API return, and a timestamp after the successful send returning the
frame's final bytes. Internal acceptance lies between the two API samples.
The report therefore gives lower/upper bounds to the send-return observation,
with signed deltas and writes before the post-call observation counted.
It does not claim an exact internal acceptance instant or peer receipt time.
The bounded tracker accumulates successful partial writes and matches MsgType
and ClOrdID. Every measured diagnostic frame must be accounted for.

Keep `descriptor_ring` as the default Queued candidate. Both candidates meet
the same boundedness, recovery and completion contracts; the repeated results
show similar callback throughput and scheduling-dominated tails. These data
do not establish an unconditional winner. `journal_cursor` remains selectable.

## Schema profile and bounded slot costs

[Full FIX44 report](schema/report.json) includes 912 fields, 93 messages and
90 group descriptors. [Before profile](profiles/schema-before/report.json)
retains the same 27 cases and three raw runs at the prior revision. The only
performance change is reuse of per-level group discovery; a bounded stack
list handles common dictionaries and wider levels retain the original search.
Validation still scans in wire order and enforces the same rules. Added
regressions cover 20 groups, final Trailer boundaries and error precedence.

| Identical schema workload | before p50 μs | after p50 μs | p50 before / after |
| --- | --- | --- | --- |
| Strict validation | 111.689 | 7.363 | 15.17× |
| Parse + strict + typed getters | 113.361 | 7.054 | 16.07× |
| Three-entry group complete round trip | 243.995 | 22.462 | 10.86× |

| Boundary | p50 μs | p99 μs | allocs/op | requested heap bytes/op |
| --- | --- | --- | --- | --- |
| Generated fluent build + encode | 0.461 | 0.511 | 3 | 560 |
| Runtime tags build + encode | 0.451 | 0.491 | 11 | 913 |
| Prebuilt dynamic encode only | 0.301 | 0.311 | 0 | 0 |
| Flat frame parse | 0.321 | 0.331 | 0 | 0 |
| Strict validation only | 7.363 | 11.261 | 0 | 0 |
| Parse + strict + typed getters | 7.054 | 11.742 | 0 | 0 |
| Group build/encode/parse/validate/project | 22.462 | 26.520 | 22 | 2446 |
| Direct writer reference | 0.130 | 0.141 | 0 | 0 |

Fluent and dynamic builders own growing storage. The dynamic encoder has
no dictionary enum/required-field validation; its wire is checked against the
static fixture before timing. The direct writer is test-only, with fixed
field order and a narrower validation domain. Its smaller duration is not an
equivalent-validation speedup. Group construction includes owned payload work.
Rotating 1,024-message working sets and S/K/B matrices are in the raw report.

Synthetic references below measure reset/fill/enumeration only, with N=912,
S=32, K=2 and B=8; they are not complete public-message encoders.

| Reference | p50 ns | instance bytes | init/reset bytes | visited slots | sort comparisons |
| --- | --- | --- | --- | --- | --- |
| Shape-local | 40 | 1024 | 1024 | 32 | 0 |
| Full-schema dense | 751 | 29184 | 29184 | 912 | 0 |
| Active-index sparse | 50 | 96 | 80 | 2 | 1 |

The N/tag-gap probe holds a two-field shape fixed while adding 1,000
unused XML fields and then spacing their tags by 1,000. Object size remains
40 bytes, declared slot storage 32 bytes and probe binary 16,232 bytes.
This is a type/storage/code-size probe, not a runtime allocation measurement.
Shared generated metadata and compilation still grow with the dictionary.

| Dictionary | generation s | typed syntax check s | header MiB |
| --- | --- | --- | --- |
| FIX42.xml | 0.074 | 0.742 | 1.20 |
| FIX43.xml | 0.102 | 0.966 | 2.78 |
| FIX44.xml | 0.182 | 1.468 | 5.29 |
| FIX50SP2.xml | 2.083 | 6.152 | 32.33 |
| FIXT11.xml | 0.049 | 0.455 | 0.08 |

Clang's single-consumer trace reports 4,337 class and 3,111 function instantiation events; this is a compiler-specific proxy. Object size is 363,608 bytes. PMU data collect six supported hardware events in a separate whole-process run, including setup/warmups/output; they are not attributed per case.

## Codec, appended journal and restart

[Codec report](codec/report.json) retains the original 15-case baseline,
including fixed reuse, fluent/name/tag, framing, sparse shapes and an
8,192-message rotating set. [Session report](session/report.json) measures
two real Drivers with six appended records per one-field round trip, using
the explicitly retained legacy v1 format and fake transport. It is not the
v2 TCP result above. Its three runs assert zero allocations and sync calls
for every measured round trip.

The Driver microbenchmark median-run p50/p99 is 4.989 / 10.951 μs.

[Journal report](journal/report.json) uses the real v2 API with 5,000
inbound/outbound/delivery triplets (15,000 records), 128-byte input/output
payloads, 64 KiB segments and 128 slots. One reader starts before append,
reaches lag 15,000, then drains to zero. Payload is copied before the next
borrowed read; copy time is outside reader_next timing. Normal recovery
closes and reopens the same store five times per run, verifying checkpoint,
record and outbound indexes. The OS page cache is not cleared. Missing-index
rebuild is functionally tested, but is not a cold-storage timing claim here.
Checkpoint has one observation and reopen five per run; their percentile
columns describe those observations rather than a stable tail estimate.

| v2 operation | p50 μs | p99 μs | worst μs |
| --- | --- | --- | --- |
| append/inbound | 0.911 | 2.454 | 671.021 |
| append/outbound | 0.912 | 2.114 | 7.454 |
| append/delivery | 0.361 | 1.553 | 7.875 |
| reader_next/inbound | 0.030 | 0.031 | 5.119 |
| checkpoint/checkpoint_and_outbound_index | 634.423 | 634.423 | 712.789 |
| reopen/normal_checkpoint_index_recovery | 17,340.866 | 17,775.786 | 17,983.414 |

Phase page-fault/RSS counters include benchmark bookkeeping and sample
buffers; per-operation rows preserve page-fault fields where measured.
The journal API requests no per-record sync. Runtime hooks independently
observe the sync-call totals above. Full-storage, committed-corruption,
checkpoint/index damage and process-kill cases are correctness tests, rather
than successful performance samples.

## Independent peer curve and session isolation

[Comparison reports](comparison/) use one Python open-loop peer, CPUs 0–3,
TCP_NODELAY, 5,000 messages and zero business warmups equally for all servers.
NimbleFIX is Inline/Flat with appended v2 storage. QuickFIX 1.16.0 is pinned
to `2ce8a60667d95a55cdc57a210f165e19cb757126`, static Release `-O3 -DNDEBUG`,
SSL enabled, SocketAcceptor/FileStore, NullLog and UseDataDictionary=N.
Both FIX servers reply D→8 with ClOrdID only. FileStore source explicitly
fflushes message/header/sequence files and has no explicit per-message
fsync/fdatasync; that is source inspection, not a traced durability equivalence.
The null server is raw byte echo with no codec/session/store.

| Server | offered/s | p50 μs | p99 μs | completed/s |
| --- | --- | --- | --- | --- |
| nimblefix | 2,000 | 567.892 | 1,131.042 | 2,000 |
| null-echo | 2,000 | 570.539 | 1,104.284 | 1,999 |
| quickfix | 2,000 | 569.559 | 1,142.931 | 2,000 |
| nimblefix | 20,000 | 656.896 | 1,696.743 | 19,929 |
| null-echo | 20,000 | 599.728 | 1,283.773 | 19,952 |
| quickfix | 20,000 | 93.387 | 2,120.525 | 19,974 |
| nimblefix | 60,000 | 49.034 | 2,395.857 | 59,746 |
| null-echo | 60,000 | 30.287 | 2,049.838 | 59,763 |
| quickfix | 60,000 | 6,597.996 | 13,204.401 | 51,410 |

60,000/s is the highest offered load here, not a discovered absolute
capacity. Python batching/wakeup effects make the curve non-monotonic. Do not
subtract separate p99 values to claim engine overhead, or generalize this
configuration to full typed business-message performance.

[Initiator reports](initiator/) separately measure the deployed Inline CLI
initiator: peer-scheduled TestRequest to matching Heartbeat, with appended v2
storage. This is an administrative boundary with no business callback.

Initiator median-run p50/p99 is 564.629 / 1,140.901 μs at 2,000/s.

[Isolation reports](isolation/) use two acceptor sessions on one Inline
owner. LAG sends 256 requests then stops reading 50 KiB replies; its pending
limit is 12 messages / 512 KiB, with a 2.5 s write deadline. BUY concurrently
sends 2,000 scheduled requests at 1,000/s. LAG is intentionally closed only
after measurement; its resulting I/O error is reported separately.

| Run | healthy replies | healthy failures | slow queue_full | healthy p99 μs | slow pending HWM | slow bytes HWM KiB | observed RSS MiB |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 2000 | 0 | 242 | 1,166.614 | 3 | 150.3 | 54.0 |
| 2 | 2000 | 0 | 242 | 1,210.240 | 3 | 150.3 | 56.0 |
| 3 | 2000 | 0 | 242 | 3,163.081 | 3 | 150.3 | 54.0 |

## Sustained rotation and background writeback

[Long report](rotation-long/report.json): one Queued descriptor-ring callback
session, 64,000 measured requests at 1,000/s after 100 warmups, first timed
and then repeated in a separate diagnostic process. Eight 8 KiB segment slots
and a 16-message retained window force repeated physical-slot reuse. Each
phase crosses the host's unchanged 15 s writeback and 30 s dirty-expiry periods.

Timed interval: 64.000 s; p50/p99/p99.9/max: 670.611 / 1,472.070 / 1,629.068 / 3,235.269 μs. Live segments: 8; maximum persistent segment index: 8,743, versus initial 7. Actual index advancement proves rotation beyond precreated files. Timed minor/major faults: 26,175/0; diagnostic peak RSS: 179.6 MiB (includes the larger preallocated sample/index capacities).

Host vmstat changed by 139,449 dirtied pages and 88,384 written pages over both phases. These are machine-wide observations with concurrent activity, not per-process write attribution or a measurement of kernel writeback latency. No forced flush or dirty-tunable change was performed. Sync-hook counts remain zero; appended confirmation remains a process-death boundary.

## Regression budget and reproduction

[Runtime budget](runtime/budget.json) is derived only after three valid runs:
p50 ceiling = 1.25× worst repeated p50; p99 ceiling = 1.5× worst repeated p99;
throughput floor = 0.8× lowest repeated throughput. It records repeat variation.
Candidates require at least three runs with matching affinity, kernel, payload,
rate, producers, window, filesystem and journal settings. Receipt completeness
and zero sync calls are also checked. This is a host-specific development
budget, not a portable latency SLO or a shared-CI pass/fail clock target.

```sh
# Optional independent oracle, built outside this repository:
git clone --branch v1.16.0 --depth 1 https://github.com/quickfix/quickfix /tmp/nimblefix-quickfix-v4
cmake -S /tmp/nimblefix-quickfix-v4 -B /tmp/nimblefix-quickfix-v4-build \
  -DCMAKE_BUILD_TYPE=Release -DHAVE_SSL=ON -DQUICKFIX_SHARED_LIBS=OFF \
  -DQUICKFIX_EXAMPLES=OFF -DQUICKFIX_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX=/tmp/nimblefix-quickfix-v4-install
cmake --build /tmp/nimblefix-quickfix-v4-build --parallel 4
cmake --install /tmp/nimblefix-quickfix-v4-build
xmake f -m release --tls=y --xml=y --benchmarks=y \
  --quickfix_dir=/tmp/nimblefix-quickfix-v4-install
xmake -j4
xmake check
python3 bench/release_run.py --bin-dir build/linux/x86_64/release \
  --output build/release-stage7-final
python3 bench/check_regression.py --report build/release-stage7-final/runtime/report.json \
  --budget bench/results/stage7/runtime/budget.json
```

The release runner uses btrfs as an explicit gate for this host; choose the
same recorded filesystem for comparison or run individual harnesses with a
separately documented environment. Raw CSV and per-sample diagnostic JSON are
deterministically gzip-compressed. [raw-manifest.json](raw-manifest.json)
contains uncompressed sizes/SHA-256; decompression is verified during archiving.
Report links rewritten by the archive are relative to this results root;
local `run-N.csv.gz` names in codec/schema/session/journal reports are relative
to that report. [commands.json](commands.json) records the executed matrix.
[Release validation](../../../docs/release-0.2.md) gives the functional,
sanitizer, fuzz and installed-consumer evidence.
