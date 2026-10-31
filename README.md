# NimbleFIX 0.2

A C++20 FIX engine with bounded messages, generated schemas, recoverable mmap
journals and nonblocking TCP/TLS. Development and tests use **xmake**; CMake
also provides a relocatable installed package.

[中文入门](docs/getting-started.zh-CN.md) · [Supported behavior](docs/session-support.md) ·
[Design](docs/rewrite-design.md) · [Release evidence](docs/release-0.2.md)

## Build and exchange an order

```sh
xmake f -m release
xmake build hello-fix
xmake run hello-fix --self-test --typed --restart
```

The paired harness performs Logon, NewOrderSingle, ExecutionReport and Logout,
then restarts with the same persistent state. For separate processes:

```sh
xmake build acceptor-example initiator-example
# Terminal 1
xmake run acceptor-example --state ./state/sell --port 9876
# Terminal 2
xmake run initiator-example --state ./state/buy --port 9876 --cl-ord-id DEMO-001
```

Keep each side's state directory across restarts. Use a new ClOrdID for a new
order. Business idempotency across callback recovery remains application logic.
Numeric IPv4/IPv6 endpoints are supported.

## Configure and operate

```sh
xmake build nimblefix-cli
xmake run nimblefix-cli init --directory ./demo
xmake run nimblefix-cli validate --config ./demo/acceptor.toml --json
xmake run nimblefix-cli run --config ./demo/acceptor.toml
# Another terminal:
xmake run nimblefix-cli run --config ./demo/initiator.toml
```

The CLI observes business messages; the order/report applications are the
examples above. Configuration version is `1`. Paths resolve against the
configuration file directory; unknown keys are errors. `validate` reads
configuration, dictionaries and certificates offline, without binding sockets
or opening journals. Secrets use explicit `password_env`/`value_env` references.
`run` prints structured session/error events with `--json`; SIGINT/SIGTERM use
bounded Logout shutdown and preserve exit status `128 + signal`.

TLS and runtime XML are optional system dependencies:

```sh
xmake f -m release --tls=y --xml=y
xmake build nimblefix-cli
xmake run nimblefix-cli doctor --json
xmake run nimblefix-cli schema summary --xml schemas/standard/FIX44.xml --json
```

TLS needs OpenSSL 3 or newer; XML needs libxml2 development files. Neither is
downloaded during a build. TOML parsing is fixed to vendored toml++ 3.4.0.
TLS verifies the server's DNS/IP identity and supports mutual authentication.
See [deployment settings](docs/getting-started.zh-CN.md) for configuration.

## Embed

`Engine<>` selects flat messages and Inline business callbacks. A complete
FIX44 binding is generated from the vendored dictionary:

```cpp
#include <nimblefix/engine.hpp>
#include <nimblefix/fix44.hpp>

using Schema = nimblefix::fix44::Schema;
auto created = nimblefix::Engine<Schema>::create({.storage={.directory="./state"}});
// Check created.ok(), then move out the engine.
// engine.on<nimblefix::fix44::ExecutionReport>(
//     [](nimblefix::SessionContext&, const auto& report) -> nimblefix::Status { ... });
auto order = nimblefix::fix44::NewOrderSingle::make()
    .cl_ord_id("DEMO-001").symbol("AAPL").side('1')
    .transact_time("20261031-12:00:00.000").order_qty(100).ord_type('1');
// In a callback: context.send(order).
```

Named setters and `set<Tag>()` use the same slots and encoder. Generated views
have typed named getters and `get<Tag>()`; groups have bounded, owned builders
and borrowed receive views. Validation levels are structure, required and
strict (types/enums as well as required fields). `DynamicMessage` is the
explicit API for runtime tags. Header fields remain engine managed.

Use `Engine<Schema, Queued>` to run business callbacks on a bounded worker
executor. One session has at most one in-flight callback; its reply append
commands finish before its delivery-complete marker. Ordinary
`handle.sender<Schema>()` is safe for multiple producer threads and copies
accepted content into bounded storage. Rejected sends retain the message and
do not consume a FIX sequence. Advanced producer lanes explicitly select a
single producer and an independent bounded SPSC channel.

`run()` owns the protocol loop on its calling thread. Embedders can instead
call `start()`, then `poll_once(max_wait_ms)` on that same thread until
`running()` is false. `request_stop()` is thread safe. Session snapshots are
safe to query from other threads; error observers run through a separate
bounded diagnostic channel. Received views and `SessionContext` are valid
only for the callback; save owned data or use a Sender for later work.

## Storage and recovery

Successful Inline `context.send` means **appended**. Cross-thread or Queued
send initially returns **accepted**; query its bounded receipt for appended,
written, failed or expired. Written means the transport accepted bytes.

Input is journaled before delivery. Delivery is at least once; after process
death an unfinished callback runs with `is_recovery()`. Its dependent replies
must repeat the same order and content, allowing recovery to reuse committed
outputs. External side effects require application idempotency.

The default session uses segmented mmap storage. Index/checkpoint files can
be rebuilt from committed records. Retention requires peer replay progress,
completed local delivery and released reader/mapping leases. It fails with
`storage_full` instead of overwriting required history. Readers have explicit
lag limits. Reset is an explicit authenticated policy and creates an audited
epoch. No memory store or automatic sequence reset is used.

`appended` covers same-host process death while the OS/filesystem keeps
operating. There is no per-message fsync/msync or power-loss durability claim.
Storage failure disables the affected session; a failed journal is never
silently recreated with sequence one.

## Test, install and measure

```sh
xmake check
# Optional independent QuickFIX installation:
xmake f --quickfix_dir=/path/to/quickfix
xmake check
python3 tests/install_test.py --features --examples
python3 tests/install_test.py --core-only --examples
python3 tests/xmake_schema_test.py
```

CMake exports core, optional XML/configuration components and generated standard
schemas, plus schema generation helpers for both CMake and xmake:

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release \
  -DNIMBLEFIX_ENABLE_TLS=ON -DNIMBLEFIX_ENABLE_XML=ON
cmake --build build-cmake -j2
ctest --test-dir build-cmake --output-on-failure
cmake --install build-cmake --prefix "$PWD/install"
```

A core build can disable tests, examples, sample/standard generation, CLI,
TLS and XML. It then needs only a C++20 compiler, threading support and the
platform's file/socket APIs; Python and the optional libraries are unnecessary.

```sh
xmake f -m release --benchmarks=y
xmake build runtime-bench
python3 bench/runtime_run.py --binary build/linux/x86_64/release/runtime-bench \
  --output build/runtime-report --samples 2000 --runs 3
```

The runtime suite measures real appended sessions, Inline/Queued delivery,
MPSC/lane submissions, wire round trips and rotation. Timing and instrumented
passes are separate; raw CSVs, settings, source hashes and limitations accompany
reports. Historical results remain in `bench/results/codec-initial` and
`bench/results/stage3`; the 0.2 results are under `bench/results/stage7`.

The public source API is frozen for **0.2.x**. Rebuild dependent binaries when
upgrading; no C++ ABI promise is made. Persistent format v1 remains supported
explicitly, while normal Engines create v2 segmented stores. Dependencies and
required acknowledgements are in [third_party/README.md](third_party/README.md).
