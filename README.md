# NimbleFIX

An embedded C++20 FIX engine under development, following the
[rewrite design](docs/rewrite-design.md). The current slice implements the
stateless FIX 4.4 codec and compact message slots. Session, mmap journal, TCP,
and `Engine` are scheduled in the subsequent slices; `codec-demo` exercises
encoding and parsing locally.

开发按[设计文档第 16 节](docs/rewrite-design.md#16-實作順序與完成條件)推进。
当前交付 codec / slot 切片；这里的示例还不进行网络登录或交易。
中文使用说明见 [入门说明](docs/getting-started.zh-CN.md)。

## Build and check

Linux, a C++20 compiler, xmake 2.9.8 or newer, and Python 3.10 or newer for the
optional XML generator and its tests. The core library has no third-party
dependencies and does not download packages. GCC 16.2 and Clang 23 are the
currently verified compilers.

```sh
xmake f -m release
xmake check
xmake run codec-demo
```

`xmake check` builds the library, runs four C++ test executables, checks the XML
generator and expected compiler diagnostics, and runs `codec-demo --self-test`.
Set `--tests=n` to build only the core and example. The sample schema is
generated into the build directory only when a target needs it.

```cpp
#include <array>
#include <nimblefix/codec.hpp>

using namespace nimblefix::literals;

auto order = nimblefix::message<"D">()
    .set<11>("DEMO-001")
    .set<55>("AAPL")
    .set<54>('1')
    .set<60>("20261026-12:00:00.000")
    .set<38>(100)
    .set<40>('2')
    .set<44>("150.25"_dec);

std::array<char, 1024> buffer;
auto encoded = nimblefix::encode_fix44(order,
    {"BUY", "SELL", 1, "20261026-12:00:00.000"}, buffer);
// Check encoded.ok() before using encoded.value().
```

The compilable example is [examples/codec.cpp](examples/codec.cpp). These are
explicit codec header values; a future session owns sequencing and timestamps.
Flat messages preserve declared field order and do not validate business rules.
Setters copy strings before returning. Tags are compile-time values; managed
header/trailer tags and duplicate fluent tags produce compiler diagnostics.

Fluent builders own a bounded growing allocation: the defaults are 64 fields
and 4096 value bytes. They are move-only; allocation failures and capacity
limits become a sticky `Status`. The explicit `FixedMessage` / `Fields` path
stores only its declared slots, uses a fixed byte arena, and supports
allocation-free reuse with `reset()`. Replacing text reclaims its previous
arena segment. See [contracts and limits](docs/codec-slice.md).

`inspect_frame` handles incomplete/coalesced input. `parse_flat` fills a
caller-owned `FieldView` span and preserves duplicates in wire order.
`find_unique` distinguishes missing and duplicate fields. Views borrow both the
frame and scratch; keep both alive and unchanged while using the view.
The built-in Length/Data table and optional `WireHints` support binary receive
values containing SOH; group boundaries require schema information.

## Optional typed schema

The checked-in XML describes two sample messages, rather than a complete FIX44
dictionary. Named setters and `set<Tag>()` share slots, validation and the
encoder. XML order determines canonical wire order. Required fields, enums,
and the supported `STRING`, `CHAR`, `INT`, `PRICE`, `QTY`, `UTCTIMESTAMP` types are
validated. Groups, components, typed receive getters, and conditional business
rules are outside this slice. Unsupported XML structures/types fail explicitly.

```sh
python3 tools/schema/generate.py --namespace venue \
  --output build/generated/venue.hpp schemas/fix44-sample.xml
```

Multiple XML inputs have equal roles: compatible definitions merge regardless
of input-file order; conflicts include source locations. External resources,
DTD/entities, excessive nesting and oversized inputs are rejected.

## Install and consume

CMake is provided for installation and external projects:

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j2
ctest --test-dir build-cmake --output-on-failure
cmake --install build-cmake --prefix /tmp/nimblefix-install
python3 tests/install_test.py
```

```cmake
find_package(NimbleFIX 0.1 CONFIG REQUIRED COMPONENTS core)
target_link_libraries(my_app PRIVATE NimbleFIX::nimblefix)
```

`NIMBLEFIX_BUILD_SAMPLE_SCHEMA=OFF` and `NIMBLEFIX_BUILD_TESTS=OFF` remove the
Python dependency from a core-only build. The installed
`nimblefix_generate_schema(TARGET ... NAMESPACE ... HEADER ... XML_FILES ...)`
helper generates custom headers and tracks XML dependencies. The installation
test also relocates the package, hides original source/build paths, compiles an
external consumer, and checks XML regeneration.

## Verification and measurements

```sh
xmake f -m debug --toolchain=clang --sanitize=y --benchmarks=n -o build-sanitize
ASAN_OPTIONS=detect_leaks=0 xmake check
xmake f -m release --toolchain=gcc --sanitize=n --benchmarks=y -o build
xmake build codec-bench
python3 bench/run.py --binary build/linux/x86_64/release/codec-bench \
  --output /tmp/nimblefix-codec-results
python3 tests/schema_size_test.py
```

LeakSanitizer needs to be disabled only in environments where ptrace prevents
it from running; AddressSanitizer and UndefinedBehaviorSanitizer remain enabled.
The optional `--fuzzers=y` Clang configuration builds `parser-fuzz` with
libFuzzer, ASan and UBSan.

The benchmark preserves compressed raw samples and environment metadata.
Allocation counts cover wrapped direct `malloc` and C++ `new` in the
executable; clock overhead and a byte-copy baseline are separate cases.
These are hot codec measurements, with no session, storage or network result.
QuickFIX interoperability and dedicated-machine performance gates remain
pending. See the [slice acceptance record](docs/codec-slice.md).

MIT licensed; the original project's copyright is retained in [LICENSE](LICENSE).
