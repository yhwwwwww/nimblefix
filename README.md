# NimbleFIX

An embedded C++20 FIX engine under development. Stages 1–3 of the
[rewrite design](docs/rewrite-design.md#16-實作順序與完成條件) provide compact
message slots, a recoverable appended mmap journal, inline sessions, and
nonblocking TCP initiator/acceptor flows. Every session uses a file-backed
journal. The default confirmation is `appended`, without per-message disk sync.

中文使用说明见[入门说明](docs/getting-started.zh-CN.md)。当前支持范围、验收证据和后续阶段见
[阶段 2–3 记录](docs/session-tcp-slice.md)及[协议支持表](docs/session-support.md)。

## First round trip

Linux, a C++20 compiler, xmake 2.9.8+, and Python 3.10+ for sample generation
and tests. The core has no third-party dependencies and downloads nothing.
GCC 16.2 and Clang 23 are the verified compilers.

```sh
xmake f -m release
xmake build hello-fix
xmake run hello-fix --self-test --restart
xmake run hello-fix --self-test --typed --restart
```

Each command creates paired TCP endpoints, exchanges Logon, NewOrderSingle,
a matching New ExecutionReport and Logout, then reopens the same journals
and repeats with continuing sequence numbers. The harness owns its temporary
state directory. `--typed` uses generated sample send builders and the same
encoder; receive callbacks currently use `FlatMessageView` through `Engine<>`.

For two separate terminals, build `acceptor-example` and `initiator-example`,
then run:

```sh
# Terminal 1: waits for clients; Ctrl-C requests a bounded Logout shutdown.
xmake run acceptor-example --state ./state/sell --port 9876
# Terminal 2: sends one order, checks the matching report, and exits.
xmake run initiator-example --state ./state/buy --port 9876 --cl-ord-id DEMO-001
```

Keep each side's state directory across restarts. A new initiator process
submits a new business order; choose a new ClOrdID when that is intended.
Application idempotency across process restarts remains application logic.
Endpoints accept numeric IPv4/IPv6 addresses. The examples' `--help` lists
their timeout and address options.

## Embed and send

The complete public-API examples are [initiator](examples/initiator.cpp),
[acceptor](examples/acceptor.cpp), and [paired harness](examples/hello_fix.cpp).
Create `Engine<>` with an explicit storage directory, register callbacks,
add endpoints, validate, and run it on one owner thread. `request_stop()`
can be called from another application thread. Callback context and received
views are borrowed for that callback only; callback work must stay bounded.

```cpp
using namespace nimblefix::literals;

auto order = nimblefix::message<"D">()
    .set<11>("DEMO-001").set<55>("AAPL").set<54>('1')
    .set<60>("20261027-12:00:00.000").set<38>(100)
    .set<40>('2').set<44>("150.25"_dec);
// Inside an active callback:
auto accepted = context.send(order);
// Check accepted.ok(); success means encoded and journaled, not peer receipt.
```

Strings are copied by setters. Fluent builders are move-only and bounded
(64 fields / 4096 value bytes by default), with growing owned storage.
`FixedMessage` provides a fixed arena and allocation-free reset/build/encode.
The stateless `encode_fix44` API remains available for explicit headers and
caller buffers; sessions own their sequence numbers and SendingTime.
See the [codec contracts](docs/codec-slice.md).

The optional XML generator supports two sample messages, required fields,
enums and six scalar types. Named setters and `set<Tag>()` share slots and
encoding plans. It is a sample FIX44 binding; full FIX44, typed receive
dispatch, repeating groups, queued callbacks and MPSC senders are stage 4.
Authentication, TLS, rotation and diagnostic tooling follow in stage 5.

## Check, install and deploy

```sh
xmake check
python3 tests/install_test.py --examples
python3 tests/install_test.py --core-only --examples
```

`xmake check` covers codec/schema contracts, journal kill boundaries,
deterministic session recovery, real TCP fairness and independent reconnect
peers, plus separate example processes. Optional independent QuickFIX tests
use a locally installed prefix: `xmake f --quickfix_dir=/path/to/quickfix`.
They are separate from the dependency-free core build.

CMake supplies the installed package and executables:

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j2
ctest --test-dir build-cmake --output-on-failure
cmake --install build-cmake --prefix /tmp/nimblefix-install
cd /tmp
/tmp/nimblefix-install/bin/hello-fix --self-test --restart
```

```cmake
find_package(NimbleFIX 0.1 CONFIG REQUIRED COMPONENTS core)
target_link_libraries(my_app PRIVATE NimbleFIX::nimblefix)
```

Turn off `NIMBLEFIX_BUILD_SAMPLE_SCHEMA` and `NIMBLEFIX_BUILD_TESTS` for a
core-only build without Python. The installed `nimblefix_generate_schema`
helper tracks custom XML inputs. Install tests use a read-only source copy,
relocate the prefix, hide original source/build paths, compile external
consumers and run the installed TCP examples elsewhere.

## Measurements

```sh
xmake f -m release --benchmarks=y
xmake build
python3 bench/run.py --binary build/linux/x86_64/release/codec-bench --output build/results/codec
python3 bench/session_run.py --binary build/linux/x86_64/release/session-bench --output build/results/session
python3 bench/tcp_run.py --binary build/linux/x86_64/release/tcp-bench-server --output build/results/tcp
python3 bench/tcp_run.py --binary build/linux/x86_64/release/null-tcp-server --echo --output build/results/echo
```

The harnesses preserve raw samples and environment metadata, separating codec,
real-journal session, open-loop TCP and byte-echo boundaries. Journal state
defaults to `build/bench-state` on the workspace filesystem. The
[acceptance record](docs/session-tcp-slice.md) explains allocation/sync checks,
measured conditions and remaining performance work. Development-host baselines
carry no latency SLO or relative speed claim.

MIT licensed; the original project's copyright is retained in [LICENSE](LICENSE).
