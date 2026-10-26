# Journal, inline session and TCP acceptance record

This closes stages 2–3 of design §16 alongside the
[stage 1 codec record](codec-slice.md). The current deliverable runs a real
FIX44 order/report flow through both roles with appended mmap storage,
bounded nonblocking I/O and restart recovery. The
[support matrix](session-support.md) defines the implemented subset.

## Architecture and contracts

`SessionCore` consumes value events and produces bounded actions. It has no
clock reads, sockets, filesystem access or callbacks. `SessionDriver` supplies
time, applies actions, appends the journal, tracks transport offsets/receipts
and invokes inline callbacks. Engine owns a poll loop and one driver per
configured session; transport operations are nonblocking.

Each session has a preallocated MAP_SHARED journal and one writer lock. The
format uses a 4 KiB versioned/identity-checked superblock, aligned record
headers, explicit little-endian fields, CRC32C and a release-store commit word.
Record ids are stable file offsets. Startup scans committed records and
rebuilds sequence, outbound and delivery indexes. Normal append does not scan
the entire history. The initial implementation uses one fixed segment;
rotation/checkpoints/retention are stage 5.

Outbound frames are committed before entering TX order. Valid inbound frames
and the associated next sequence (including GapFill) are committed before
callback delivery. Successful callback completion appends a delivery marker.
Replies carry the input record id; recovery invokes unfinished callbacks and
reuses previously committed replies by send ordinal. Changed or omitted
recovery replies fail visibly. This gives repeatable at-least-once delivery
without pretending to make external business effects exactly once.

`context.send()` returns `appended`. Driver receipts distinguish written,
failed and expired states. Short writes retain offsets; EAGAIN waits for
readiness. Rejected sends retain caller ownership and do not allocate a FIX
sequence. Count/byte limits, admin reserve, low-water notification, age limits
and stall deadlines bound outstanding work. Replay precedes unsent higher
sequences but cannot split an already started frame.

Engine configuration and callback registration stay cold until `run()`.
The acceptor matches a complete Logon against static local/remote CompIDs
before opening that identity's journal. Its stable identity excludes display
name, listener/source port and connection details. Round-robin work budgets
serve other sessions while a peer stops reading; complete buffered frames
remain runnable without waiting for another socket edge. `request_stop()`
uses eventfd to wake the owner and attempts bounded Logout shutdown.

## Acceptance evidence

| Design gate | Executable evidence |
| --- | --- |
| Real mmap append/reopen, format, identity, lock, capacity and corruption | `journal-test`: parent creation, outbound/delivery index rebuild, full record/byte limits, wrong identity/version/truncated store, committed CRC corruption and holes fail closed. |
| Append/commit death boundaries | `journal-test` forks and SIGKILLs after header, payload, before commit and after commit; reopen retains only committed data. |
| Send/delivery death boundaries | `session_boundary-test` SIGKILLs after inbound commit, outbound commit before enqueue, callback reply commit, and before/after delivery commit; checks exact recovered sequence/callback/dependency and replay flags. |
| Deterministic protocol and transport recovery | `session-test`: fake times, short writes, EAGAIN, transport error, reconnect, original sequence replay, deferred high Logon, real two-driver missing business/admin ranges, GapFill, duplicate suppression, deferred ResendRequest completion and reply consistency. |
| Heartbeat and Logout timing | `session-test`: TestRequest deadline starts after transport write, no stale-deadline spin while blocked; local Logout awaits acknowledgement and peer Logout closes after response write. Limits/network tests assert a single timeout event and failed shutdown result if the peer never acknowledges. |
| Resource admission and receipts | `session_limits-test`: independent message/byte saturation, queue_full without sequence/ownership change, expired/written receipts, low-water notification, reserved Logout and finite stall error. |
| Real slow peer and fairness | `network-test`: three allowed identities, silent pending Logon, rejected unknown/duplicate peer, fragmented/coalesced input, 1,200-frame burst, 80 large replies to an unread peer while another receives replies, half-frame disconnect/reconnect, 1,200 complete frames followed by half/full-close, existing-Logout shutdown and isolated callback failure. |
| Independent initiator receive recovery | `initiator_reconnect_test.py`: peer closes mid-report, relogons high, receives missing-range request, replays original report and verifies fresh post-recovery traffic. |
| Independent initiator send recovery | `outbound_reconnect_test.py`: peer closes mid-order, requests old sequence after reconnect, checks PossDup/OrigSendingTime and that replay precedes new order. |
| Real order/report/restart | `hello-fix --self-test --restart`, generated-send variant `--typed`, and `endpoint_test.py` using separate public acceptor/initiator processes with persistent client state. |
| Independent FIX implementation | `quickfix-test`: 1,000 dictionary-validated bidirectional frames and bad-checksum rejection. `quickfix_network-test`: QuickFIX SocketInitiator/FileStore exchanges valid order/report/Logout twice, retaining both stores and changing server port. |
| Installed public consumer and deployment | `install_test.py --examples` and `--core-only --examples`: read-only source build, prefix relocation, hidden original paths, standalone linked consumers, XML regeneration, installed hello and separate endpoint processes outside source. |

Primary commands:

```sh
xmake f -m release --toolchain=gcc --benchmarks=y
xmake check
python3 tests/compile_fail_test.py --cxx clang++
python3 tests/install_test.py --examples
python3 tests/install_test.py --core-only --examples
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j2
ctest --test-dir build-cmake --output-on-failure
```

The schema suite has 20 tests and compile-fail contracts have 15 cases with a
positive control, including unsupported typed Engine and invalid callback
signatures. Sanitizer and parser fuzz smoke use Clang:

```sh
xmake f -c -m debug --toolchain=clang --sanitize=y --fuzzers=y --benchmarks=n -o build-sanitize
ASAN_OPTIONS=detect_leaks=0 xmake check
ASAN_OPTIONS=detect_leaks=0 xmake run parser-fuzz -runs=100000 -max_len=1024
```

`detect_leaks=0` is specific to the local ptrace environment; ASan/UBSan stay
enabled. CI defines GCC/Clang contracts, relocated deployment and benchmark
smoke plus sanitizer/fuzz jobs. Remote CI execution is not claimed.

The final local matrix passed on 2026-10-27: GCC release xmake checks including
both optional QuickFIX tests, Clang ASan/UBSan xmake checks, all 17 CTest
entries, GCC full/core-only relocated deployments and a Clang full deployment.
The paired harness also passed 100 repeated restart runs (50 flat, 50
generated-send), and a valid-frame-seeded parser fuzz run completed 100,000
executions without a sanitizer finding. These checks cover the declared
stage 3 subset, not full FIX44 certification.

QuickFIX is optional and absent from normal core builds. The independent
checks use official tag v1.16.0, commit
`2ce8a60667d95a55cdc57a210f165e19cb757126`, a Release static build without
SSL and its shipped FIX44.xml, configured through
`xmake f --quickfix_dir=/path/to/installed/quickfix`. Its FileStore run is a
correctness oracle; its sync semantics and performance are not compared.

## Review and measurement boundary

Integration review corrected replay ordering around partial writes, deferred
high-Logon activation/range boundaries, TestRequest write timing, error
reporting, callback reply reuse and the atomic GapFill sequence decision.
It also aligned offline Engine budgets with Driver admission, rejected empty
and duplicate registrations, made unsupported typed Engine use fail at
compile time, made example replies deterministic across recovery, preserved
buffered input on TCP hangup, and removed duplicate Logout on simultaneous
shutdown. Simultaneous Logout now waits for the local Logout frame to finish
writing before closing. Journal completion/outbound append failures fail and
report consistently, and shutdown timeout/transport failure reaches both the
event and run result.

`bench/run.py` extends codec measurements with an independent minimal
parse/encode case, controlled declared/present field and payload sizes, and
an 8,192-message rotating working set. The original
[codec baseline](../bench/results/codec-initial/README.md) remains a historical
record for commit `059c3f3`.

`bench/session_run.py` measures two real Drivers and journals, inline callbacks
and fake-transport round trips with fixed-shape messages. Executable allocator
hooks and wrapped fsync/fdatasync/msync assert zero calls per measured round
trip. Page-fault deltas are recorded. These hooks do not observe all shared
library internals or constitute syscall tracing; fluent message construction
still allocates. No general zero-allocation application claim is made.

`bench/tcp_run.py` uses an independent Python open-loop generator, measuring
scheduled arrival through complete parsed reply with real Engine/journal or
a minimal nonblocking raw TCP echo. It preserves per-request samples and
offered/observed rates. Python scheduling/framing, loopback and polling costs
are included. The two p99 values must not be subtracted to claim engine cost.
This harness uses minimal flat D traffic and an 8 reply carrying tag 11;
it measures session/transport work with a fluent reply builder. The full
order/report examples and dictionary oracle cover application message validity
separately. L2 uses fixed-shape messages and a different allocation boundary.
Journal state defaults to the workspace filesystem, not `/tmp` tmpfs.

The corresponding stage 3 raw measurements are recorded under
`bench/results/stage3/`, with exact code revision, binary/source hashes,
compiler, CPU affinity, filesystem and workload conditions. They are an
initial development-host baseline, with no numerical SLO or QuickFIX speedup
claim. Dedicated-machine repeatability, full saturation curves, hardware
counters, lock/syscall tracing and long-running background-write/rotation
tails remain later performance and release gates.

The pre-runtime rollback point is `5b7e0c6`. This stage closes the first TCP
demonstration boundary; full release acceptance still requires stages 4–5
and stage 7 as specified in the design.
