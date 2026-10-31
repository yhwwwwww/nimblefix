# 0.2 release record: design stages 4–7

This record accompanies the source API freeze for 0.2.x. Historical stage 1
and stage 2–3 records describe their original commits; the current behavior
is in the [support matrix](session-support.md).

## Implementation

Stage 4 adds full generated FIX44 metadata, named/static builders and views,
scalar/group validation, Queued worker delivery, ordinary bounded MPSC Sender,
explicit SPSC producer lanes and the remaining declared session actions.
Both execution policies retain one protocol owner and the same journal driver.

Stage 5 adds v2 segmented storage, index/checkpoint recovery, audited sequence
epochs, safe retention floors, independent readers/leases, nonblocking verified
TLS, authentication before acceptor store acquisition, UTC schedules, per-session
flow policies and bounded off-owner diagnostics. TOML v1 and the CLI provide
offline validation and deployed operation.

Stage 6 adds full standard dictionaries for FIX42/FIX43/FIX44/FIXT11/FIX50SP2,
custom multi-input XML generation, automatic metadata fingerprints, optional
libxml2 runtime loading, explicit DynamicMessage and caller-owned start/poll_once.
Installed CMake/xmake helpers regenerate on their inputs and expose optional
package capabilities.

Stage 7 keeps raw measurements and their exact boundaries with the source.
[Stage 7 results](../bench/results/stage7/README.md) describe profile findings,
measured candidates, rotation/background-write behavior and regression budgets.
No performance conclusion uses a memory store or conflates accepted commands
with committed/written messages.

## Verification commands

```sh
xmake f -m debug --tls=y --xml=y --benchmarks=y \
  --quickfix_dir=/tmp/nimblefix-quickfix-v4-install
xmake check
python3 tests/install_test.py --features --examples
python3 tests/install_test.py --core-only --examples
python3 tests/xmake_schema_test.py
```

QuickFIX is an optional locally installed v1.16.0 oracle; the checked dictionaries
are pinned to commit `2ce8a60667d95a55cdc57a210f165e19cb757126` with its license
and acknowledgement. Its port-zero acceptor routing is unsuitable for the
independent acceptor fixture, so that fixture probes a free loopback port before
starting QuickFIX. NimbleFIX listeners use OS-assigned port zero directly.

The tests exercise partial/coalesced frames and binary DATA; dictionary enums,
calendar/time precision and nested group structure; real TCP fairness/EOF;
Logon/authentication/Reset/369/replay/Reject/Logout; callback and receipt ordering;
process-kill journal commit boundaries; checkpoint/index damage and committed
corruption; repeated rotation/retention; reader/destructor concurrency; TLS
SAN/mTLS/EAGAIN/close_notify and handshake timeout; off-owner observer overflow
and failure; manual owner-thread checks; Unicode/spaced paths; secret redaction;
relocated installed consumers and dictionary incrementality.

Independent raw peers exercise FIX42/FIX43/FIX44/FIXT11 in both initiator and
acceptor roles with Inline and Queued, including Logon, a business message,
FIX369, TestRequest/Heartbeat, admin replay GapFill and bounded Logout.
QuickFIX exercises full FIX44 business messages with persistent state in both
roles, including typed Inline/Queued initiators and restart.

Sanitizer and performance outcomes are recorded with their actual commands
and limitations in the final validation section below.

## Final validation

The final implementation passed xmake check in Debug and Release with TLS,
runtime XML, benchmarks and the independent QuickFIX oracle enabled. The
CMake Release build passed all 43 tests, including journal/schema/runtime
benchmark smoke checks. Those smoke checks validate the harness, not latency.

Separate RelWithDebInfo builds passed 13 ASan/UBSan checks with leak detection
and six TSan checks with halt_on_error. The sanitizer set covers journal
readers/rotation, protocol behavior, schema/dynamic messages, runtime XML,
Queued delivery/shutdown, ordinary Sender/lane expiry, producer-lane receipt
churn, manual lifecycle and TLS. TSan covers the reader, Queued runtime,
Engine lifecycle, producer lane, shutdown and expiry tests. GCC reports its
usual limitation in modeling explicit atomic_thread_fence operations; the
race checks complement the release/acquire ownership review.

```sh
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/nimblefix-v4-asan --output-on-failure -j1 \
  -R 'producer_lane|sender_expiry|queued_shutdown|queued_runtime|engine_features|journal_reader|journal_rotation|protocol_session|schema_codec|dynamic_message|runtime-xml|engine-tls|tls-transport'
TSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/nimblefix-v4-tsan --output-on-failure -j1 \
  -R 'producer_lane|sender_expiry|queued_shutdown|queued_runtime|engine_features|journal_reader'
```

The full FIX44 parser/strict-validator fuzz target, built with Clang and
ASan/UBSan, completed 1,592,580 executions in a 16-second bounded run without
a finding. This is a smoke campaign, not an exhaustive protocol proof.

Both installed-consumer modes passed from a read-only source copy after
relocating the prefix and hiding the source/build paths. Feature mode exercises
TLS, runtime XML, full FIX44, generated custom bindings, manual Queued driving,
Sender/lane and installed examples/CLI. Core-only mode disables Python,
OpenSSL and libxml2 package discovery and still builds and runs the core/CLI.
All five standard dictionaries also pass sequential generated typed-consumer
syntax checks; generator/XML normalization fingerprints agree.

Review fixes include a Queued completion ownership cycle exposed by the
shutdown leak test, concurrent terminal lane receipt publication, stale
cross-epoch FIX369 progress, accepted-command age expiry without consuming
sequence numbers, and safe bounded cleanup after worker completion. The
performance review replaced full receipt-ring scans with active indexes,
removed idle Sender mutex acquisition and kept SPSC wakeup/receipt paths free
of shared mutexes. Full-schema field lookup now reuses a bounded list of group
descriptors, with an unrestricted fallback for wider dictionaries. Tests also
cover 20 independent groups, final-group Trailer boundaries and unchanged error
precedence. The stage 7 results retain the original and optimized schema runs;
the reported improvement is scoped to those schema microbenchmarks.

## Compatibility and operating boundary

The order-entry examples were subsequently corrected to use explicit business
commands through a retained Sender on a thread separate from the protocol owner.
Logon/reconnect create no orders, and one report does not terminate the process.
The initiator accepts multiple limit/market requests, tracks local receipts
separately from counterparty reports, and drains on quit/EOF/signal with a
deadline. Later partial/full trade reports for the same ClOrdID are supported.
The old example-only --cl-ord-id flag is replaced by per-command IDs. The core
library API and the measured runtime implementation are unchanged.

An independent raw FIX peer verifies idle Logon, explicit order parameters and
fresh TransactTime, continued operation after reports, reconnect without a new
order, and write/report completion before Logout. The revised examples passed
xmake check, all 43 CMake Release tests, three typed ASan/UBSan example checks,
two flat TSan example checks, and both relocated installation modes with
examples. The protocol/restart self-test also submits through an external
Sender and uses distinct business IDs across restarts.

The readable walkthrough now keeps typed and flat entry points in separate
source files. Console callbacks explicitly use Queued and ordinary standard
containers for request correlation; custom event queues and the larger
order-entry support layer were removed. Receipt progress is queried by status
and during shutdown. The paired protocol self-test belongs to tests/hello_fix.cpp.

Source compatibility is scoped to 0.2.x; rebuild applications when changing the
library. Generated headers and loaded XML must have equal metadata fingerprints
when used together. Flat mode and typed mode are explicit. Optional XML/TLS and
CLI components do not become hidden core dependencies.

Normal Engines create segmented v2 stores. Explicit low-level segment_bytes=0
continues v1 behavior; no silent migration/reinitialization occurs. Store
identity includes transport version, oriented CompIDs and qualifier. Session
display names and socket ports are not persistent identity.

The confirmation boundary is appended under process death with the host OS
still alive. There is no per-message sync or power-loss promise. Business
callbacks can run again during recovery, and external side effects require
application idempotency. Inline callbacks must be bounded; Queued callbacks
must eventually return. The engine cannot safely preempt arbitrary C++ code.

Logon reset remains explicitly opt-in. A persistent reset_sequence=true setting
requests reset on every fresh connection and needs bilateral agreement; the
default reconnect/schedule path preserves sequence state.
