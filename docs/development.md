# FastFix Development Guide

This guide covers everything you need to contribute to or extend FastFix: building, testing, profiling, adding protocol support, and using the toolchain.

---

## Build System

FastFix supports both offline CMake and offline xmake flows. CMake is the primary path for shared environments and the RHEL targets; xmake remains available for local development.

### Prerequisites

- C++20 compiler (GCC 12+ or Clang 15+)
- CMake 3.20+ and a native build backend (`make` or Ninja)
- xmake (latest) if you prefer the optional xmake path
- Linux x86_64 (primary platform)

### Build Targets

| Target | Kind | Description |
|--------|------|-------------|
| `fastfix` | static library | Core engine library |
| `fastfix-tests` | binary | Catch2 test suite |
| `fastfix-bench` | binary | Performance benchmarks |
| `fastfix-dictgen` | binary | Dictionary → artifact compiler |
| `fastfix-xml2ffd` | binary | QuickFIX XML → `.ffd` converter |
| `fastfix-initiator` | binary | CLI FIX client |
| `fastfix-acceptor` | binary | CLI FIX server (echo) |
| `fastfix-soak` | binary | Stress tester with fault injection |
| `fastfix-interop-runner` | binary | Interop scenario runner |
| `fastfix-fuzz-codec` | binary | Codec/admin fuzzer |
| `fastfix-fuzz-config` | binary | Config parser fuzzer |
| `fastfix-fuzz-dictgen` | binary | Dictionary parser fuzzer |

### Common Commands

```bash
cmake --preset dev-release   # Configure the default offline build
cmake --build --preset dev-release
ctest --preset dev-release

bash ./scripts/offline_build.sh --preset dev-release --bench smoke

xmake f -m release -y        # Optional xmake path
xmake build fastfix-tests
xmake clean
```

Preset-based CMake builds write executables to `./build/cmake/<preset>/bin/`. The examples below assume `BIN_DIR=./build/cmake/dev-release/bin` for the CMake path and invoke binaries directly for reproducibility and predictable argument handling. The xmake path still writes to `./build/linux/x86_64/release/`.

GitHub Actions CI covers the named RHEL presets through `ubi8/ubi:8.10` + `gcc-toolset-12` and `ubi9/ubi:9.7` + `gcc-toolset-14` container jobs, so those environments are regression-tested even when they are not reproduced locally.

### Dependencies

Pinned as Git submodules for offline builds:

- **Catch2 v3.13.0** — `deps/src/Catch2`
- **pugixml v1.15** — `deps/src/pugixml`
- **QuickFIX commit 00dd20837c97578e725072e5514c8ffaa0e141d4** — `bench/vendor/quickfix`

Initialize them with `git submodule update --init --recursive` before the first configure/build run.

### Compiler Defines

| Define | Purpose |
|--------|---------|
| `FASTFIX_PROJECT_DIR` | Set to `$(projectdir)` for test data path resolution |

---

## Source Layout

```
include/fastfix/
├── base/       Status, Result, SpscQueue, InlineSplitVector, TimerWheel
├── codec/      DecodeFixMessageView, EncodeFixMessage, SIMD scan, FrameEncodeTemplate
├── message/    Message, MessageView, MessageBuilder, TypedMessageView, FixedLayoutWriter, GroupView
├── profile/    LoadedProfile, NormalizedDictionaryView, ProfileLoader, ProfileRegistry
├── session/    SessionCore, AdminProtocol, ResendRecovery
├── store/      MemorySessionStore, MmapSessionStore, DurableBatchStore
├── transport/  TcpConnection, TcpAcceptor
└── runtime/    Engine, LiveInitiator, LiveAcceptor, ShardedRuntime, Metrics, Trace
```

Each module has a corresponding source directory under `src/` and tests under `tests/`.

---

## Testing

### Framework

Tests use **Catch2 v3**. The test binary is `fastfix-tests`.

```bash
BIN_DIR=./build/linux/x86_64/release

# Run all tests
$BIN_DIR/fastfix-tests

# Run by tag
$BIN_DIR/fastfix-tests "[fix-codec]"
$BIN_DIR/fastfix-tests "[session-core]"
$BIN_DIR/fastfix-tests "[negative]"

# Run by name
$BIN_DIR/fastfix-tests "Heartbeat round-trip"

# List all tags
$BIN_DIR/fastfix-tests --list-tags
```

### Available Tags

| Tag | Module |
|-----|--------|
| `[fix-codec]` | Core FIX encoding/decoding |
| `[simd-scan]` | SIMD byte scanning |
| `[negative]` | Error and edge-case handling |
| `[session-core]` | Session state machine |
| `[legal]` / `[illegal]` | Valid / invalid state transitions |
| `[timeout]` | Timeout behavior |
| `[edge]` | Edge cases (disconnect/reconnect) |
| `[recovery]` / `[gap]` | Message recovery and gap fill |
| `[lifecycle]` | Full session lifecycle |
| `[transport-fault]` | Network fault injection |
| `[store-recovery]` | Store persistence and recovery |
| `[socket-loopback]` | TCP loopback |
| `[sharded-runtime]` | Multi-shard engine |
| `[profile-loader]` | Artifact loading |
| `[timer-wheel]` | Timer scheduling |
| `[message-api]` | Message builder/view API |
| `[typed-message]` | Typed message accessors |
| `[normalized-dictionary]` | FIX dictionary structures |
| `[dictgen]` | Dictionary compilation |
| `[overlay-merge]` | Profile overlay merging |
| `[parser-surface]` | Input format parsing |
| `[resend-recovery]` | Resend request handling |
| `[metrics-trace]` | Metrics and trace collection |
| `[runtime-config]` | Configuration parsing |
| `[admin-fuzz-corpus]` | Admin protocol corpus |
| `[application-queue]` | App message queueing |
| `[application-poller]` | App polling |
| `[live-runtime]` | End-to-end runtime |
| `[interop-harness]` | Interop scenarios |
| `[live-initiator]` | Live initiator mode |

### Test Utilities

**`tests/test_support.h`** provides:

```cpp
// Convert pipe-delimited text to FIX bytes (| → SOH)
auto Bytes(std::string_view text) -> std::vector<std::byte>;

// Build a valid FIX frame with auto-computed BodyLength and Checksum
auto EncodeFixFrame(std::string_view body_fields,
                    std::string_view begin_string = "FIX.4.4",
                    char delimiter = '|') -> std::vector<std::byte>;
```

**`tests/test_cases.h`** contains shared test data (pre-built FIX messages, session keys, common fixtures).

### Generated Test Fixtures

`tests/generated/sample_basic_builders_fixture.h` — auto-generated from the sample profile. Provides profile constants (profile ID, schema hash) for test usage.

---

## Protocol Profiles

### Key Concepts

- **Profile**: A normalized description of a FIX protocol variant. Each profile has a unique `profile_id` (uint64). Profiles are loaded at engine startup and referenced by sessions. One engine can load multiple profiles simultaneously (e.g., FIX 4.2 and FIX 4.4). A profile can be loaded from a precompiled `.art` file or built in memory from `.ffd` files at startup.

- **`.ffd` (FastFix Dictionary)**: A text file defining baseline or incremental dictionary content for one FIX protocol variant — fields, messages, and groups. The first `.ffd` establishes the profile, and additional `.ffd` files can extend it when multiple files are passed to dictgen or Engine.

- **`.art` (Artifact)**: The compiled binary output of dictgen (optional precompilation step). It's a flat, mmap-loadable file containing string tables, field/message/group definitions, validation rules, and lookup tables. The engine can load `.art` files at runtime, or load `.ffd` files directly.

- **`schema_hash`**: A 64-bit hash of the dictionary content, embedded in the `.art` header. Used internally as part of the codec template cache key to ensure cached templates are invalidated when the dictionary changes. Auto-computed by dictgen from the dictionary content — users don't need to set it manually.

### `.ffd` Format Specification

The file format is line-based, pipe-delimited text. Lines starting with `#` are comments. Blank lines are ignored.

**Header lines** (key=value):

```
profile_id=<uint64>       # Unique profile identifier (required)
schema_hash=<hex_uint64>  # Auto-computed by dictgen; ignored if present in hand-written files
```

**Field definitions:**

```
field|<tag>|<name>|<type>|<flags>
```

- `tag`: FIX tag number (uint32)
- `name`: Human-readable field name
- `type`: One of `string`, `int`, `char`, `float`, `boolean`, `timestamp`, `unknown`
- `flags`: Bitfield — `0` = standard field, `1` = custom/vendor field

**Message definitions:**

```
message|<msg_type>|<name>|<admin_flag>|<field_rules>
```

- `msg_type`: FIX MsgType value (e.g. `D` for NewOrderSingle, `0` for Heartbeat)
- `name`: Human-readable message name
- `admin_flag`: `1` = admin message, `0` = application message
- `field_rules`: Comma-separated `tag:modifier` pairs where modifier is `r` (required) or `o` (optional)

**Group definitions:**

```
group|<count_tag>|<delimiter_tag>|<name>|<flags>|<field_rules>
```

- `count_tag`: The FIX tag that holds the group count (e.g. `453` for NoPartyIDs)
- `delimiter_tag`: First field in each group entry (the field that marks entry boundaries)
- `name`: Human-readable group name
- `flags`: Reserved, use `0`
- `field_rules`: Same comma-separated `tag:modifier` format as messages

**Full example:**

```
profile_id=1001

field|8|BeginString|string|0
field|9|BodyLength|int|0
field|10|CheckSum|string|0
field|11|ClOrdID|string|0
field|35|MsgType|string|0
field|38|OrderQty|int|0
field|44|Price|float|0
field|49|SenderCompID|string|0
field|54|Side|char|0
field|56|TargetCompID|string|0
field|453|NoPartyIDs|int|0
field|448|PartyID|string|0
field|447|PartyIDSource|char|0
field|452|PartyRole|int|0

message|0|Heartbeat|1|35:r,49:r,56:r
message|A|Logon|1|35:r,49:r,56:r
message|D|NewOrderSingle|0|35:r,49:r,56:r,11:r,54:r,38:r,44:o,453:o

group|453|448|Parties|0|448:r,447:r,452:r
```

### `.ffo` Overlay Format — Removed

The separate `.ffo` overlay format has been removed. Multiple `.ffd` files can be merged instead. The first `.ffd` provides the baseline (`profile_id` required), additional `.ffd` files extend it (adding fields, extending messages, overriding field rules). No separate overlay format needed.

Example of an incremental `.ffd` overlay (no header lines needed — `profile_id` comes from the base `.ffd`):

```
# Add venue-specific custom fields
field|5001|VenueOrderType|string|1
field|5002|VenueAccount|string|1

# Extend NewOrderSingle with custom fields
message|D|NewOrderSingle|0|5001:r,5002:o
```

### Compiling Profiles

```bash
$BIN_DIR/fastfix-dictgen \
    --input samples/basic_profile.ffd \
    --merge samples/basic_overlay.ffd \
    --output build/sample-basic.art \
    --cpp-builders build/generated/sample_basic_builders.h
```

**Flags:**

| Flag | Required | Description |
|------|----------|-------------|
| `--input` | yes | Base dictionary file (`.ffd`) |
| `--merge` | no | Additional `.ffd` file(s) to merge, can specify multiple (also accepts `--overlay` for backwards compatibility) |
| `--output` | yes | Output artifact path (`.art`) |
| `--cpp-builders` | no | Generate C++ header with typed writer classes and profile constants |

**Note:** `.art` precompilation is optional. The Engine can load `.ffd` files directly at runtime via the `dictionary=` config line or `config.profile_dictionaries`.

**Output:**

- `.art` — Binary artifact (mmap-loadable). Contains string table, field/message/group definitions, validation rules, lookup tables.
- C++ header (optional) — Typed writer classes backed by `FixedLayoutWriter` (compile-time type-safe, O(1) slot writes), Tag constants, profile ID and schema hash.

### Adding a New FIX Version

1. Obtain the QuickFIX XML data dictionary for your FIX version (e.g. `FIX44.xml` from the [QuickFIX repository](https://github.com/quickfix/quickfix/tree/master/spec)).
2. Convert it to `.ffd` format:
   ```bash
    $BIN_DIR/fastfix-xml2ffd \
       --xml FIX44.xml \
       --output my_fix44.ffd \
       --profile-id 1001 \
       --cpp-builders generated_builders.h
   ```
3. (Optional) Write additional `.ffd` files for venue-specific custom fields and pass them with `--merge`.
4. Compile with `fastfix-dictgen`.
5. Reference the `.art` in `EngineConfig.profile_artifacts`. Or skip `.art` and reference `.ffd` directly in `EngineConfig.profile_dictionaries`.

You can also write `.ffd` files by hand for full control.

### Generated Profile Header

The `--cpp-builders` flag produces a header with typed writer classes and profile constants:

```cpp
#include "build/generated/sample_basic_builders.h"

using namespace fastfix::generated::profile_1001;

// Build a FixedLayout for the message type, then use the generated writer.
auto layout = message::FixedLayout::Build(dictionary_view, NewOrderSingleWriter::kMsgType);
NewOrderSingleWriter writer(layout.value());
writer.bind_session("FIX.4.4", "SENDER", "TARGET");

// Hot loop — clear + fill + encode.
writer.clear();
writer.set_venue_order_type("LIT")
      .set_venue_account("ACC-1");
writer.add_parties()
      .set_party_id("PTY1")
      .set_party_id_source('D')
      .set_party_role(1);
writer.encode_to_buffer(dictionary_view, options, &buf);
```

---

## Benchmarking

### Running Benchmarks

```bash
./bench/bench.sh build
./bench/bench.sh fastfix
./bench/bench.sh fastfix-ffd
./bench/bench.sh quickfix
./bench/bench.sh builder
```

All of those benchmark entrypoints intentionally consume the pinned QuickFIX 4.4 inputs, either through `bench/vendor/quickfix/spec/FIX44.xml` or the generated `build/bench/quickfix_FIX44.ffd` / `build/bench/quickfix_FIX44.art` outputs. The benchmark-specific breakdown now lives in [bench/README.md](../bench/README.md), including what each suite measures, the object-to-wire timing boundary, and the meaning of every printed metric. This section stays intentionally short and points to the canonical bench-local workflow.

### What Gets Measured

| Tier | What | Key Metric |
|------|------|------------|
| peek | Header extraction only | Latency percentiles, alloc/op, perf counters |
| parse | Full decode to MessageView | Latency percentiles, alloc/op, perf counters |
| encode | Build FIX frame (new buffer) | Latency percentiles, alloc/op, perf counters |
| encode-buffer | Build FIX frame (reuse buffer) | Latency percentiles, alloc/op, perf counters |
| session-inbound | Decode + seq check + store | End-to-end inbound hot-path latency |
| replay | Recover + re-encode 128 msgs | Replay latency and throughput |
| loopback | Full TCP round-trip | Socket-to-socket round-trip latency |

### Instrumentation

- **Allocation tracking**: Global `operator new` override counts heap allocations per iteration.
- **CPU counters**: Linux `perf_event_open` captures cache misses, branch mispredictions, instructions.
- **Percentiles**: per-iteration `steady_clock` samples → p50/p95/p99/p999.

---

## Stress Testing (Soak)

The soak tool runs multiple sessions with configurable fault injection:

```bash
$BIN_DIR/fastfix-soak --profile build/sample-basic.art \
    --workers 4 --sessions 10 --iterations 10000 \
    --gap-every 100 \
    --duplicate-every 200 \
    --drop-every 500 \
    --reorder-every 300 \
    --disconnect-every 1000 \
    --trace
```

### Fault Injection Options

| Option | Effect |
|--------|--------|
| `--gap-every N` | Skip sequence numbers to create gaps |
| `--duplicate-every N` | Send duplicate messages |
| `--replay-every N` | Trigger replay requests |
| `--reorder-every N` | Deliver messages out of order |
| `--drop-every N` | Drop messages silently |
| `--jitter-every N` | Add timing jitter |
| `--jitter-step-ms N` | Jitter magnitude (ms) |
| `--disconnect-every N` | Force transport disconnection |
| `--timer-pulse-every N` | Pulse timer advancement |
| `--timer-step-sec N` | Timer step duration |

Output includes counts of all injected events, resend requests, gap fills, reconnects, and application messages processed.

---

## Fuzz Testing

Three fuzzing harnesses target different input surfaces:

### Codec Fuzzer

```bash
$BIN_DIR/fastfix-fuzz-codec \
    --artifact build/sample-basic.art \
    --input tests/data/fuzz/wire_codec \
    --mode codec     # or: admin
```

- `codec` mode: fuzzes `DecodeFixMessageView` and `PeekSessionHeaderView`
- `admin` mode: fuzzes `AdminProtocol::OnInbound` with decoded messages

### Config Fuzzer

```bash
$BIN_DIR/fastfix-fuzz-config --input tests/data/fuzz/
```

Fuzzes the `.ffcfg` engine configuration file parser.

### Dictionary Fuzzer

```bash
$BIN_DIR/fastfix-fuzz-dictgen \
    --input tests/data/fuzz/
```

Fuzzes `.ffd` file parsing.

### Corpus

Test data lives in `tests/data/fuzz/`:

```
wire_codec/          FIX frame corpus (valid + malformed)
admin_protocol/      Admin message sequence corpus (.fixseq)
dictgen_*.ffd        Dictionary corpus
overlay_*.ffd        Overlay corpus
runtime_config_*.ffcfg  Config corpus
```

---

## Live Tools

### Initiator (Client)

```bash
$BIN_DIR/fastfix-initiator \
    --artifact build/sample-basic.art \
    --host exchange.example.com --port 9876 \
    --sender MY_FIRM --target VENUE_A \
    --begin-string FIX.4.4 \
    --validation-mode strict \
    --reconnect \
    --reconnect-initial-ms 1000 \
    --reconnect-max-ms 30000
```

### Acceptor (Server)

```bash
# Direct mode
$BIN_DIR/fastfix-acceptor \
    --artifact build/sample-basic.art \
    --bind 0.0.0.0 --port 9876 \
    --sender VENUE_A --target MY_FIRM

# Config file mode
$BIN_DIR/fastfix-acceptor --config engine.ffcfg --listener main
```

### Local Testing

```bash
# Terminal 1: start acceptor
$BIN_DIR/fastfix-acceptor \
    --artifact build/sample-basic.art \
    --bind 127.0.0.1 --port 9001 \
    --sender SERVER --target CLIENT

# Terminal 2: connect initiator
$BIN_DIR/fastfix-initiator \
    --artifact build/sample-basic.art \
    --host 127.0.0.1 --port 9001 \
    --sender CLIENT --target SERVER
```

---

## Configuration File Format (`.ffcfg`) — Internal

The `.ffcfg` format is used by FastFix's built-in tools (`fastfix-acceptor`, `fastfix-interop-runner`) and tests. It is **not** part of the library's public API. Applications should populate `EngineConfig` directly from their own configuration source (TOML, YAML, JSON, database, etc.) and call `Engine::Boot(config)`.

```
engine.worker_count=2
engine.enable_metrics=true
engine.trace_mode=ring
engine.trace_capacity=8
profile=path/to/profile.art
dictionary=path/to/profile.ffd
counterparty|name|session_id|profile_id|begin_string|sender|target|store_mode|durable_dir|validation|dispatch|heartbeat_sec|is_initiator
```

Load in code:

```cpp
// Internal tool use only — #include "fastfix/runtime/config_io.h"
auto config = fastfix::runtime::LoadEngineConfigFile("engine.ffcfg");
```

---

## Interop Testing

### Internal Interop

```bash
$BIN_DIR/fastfix-interop-runner --scenario tests/data/interop/loopback.ffscenario
```

Runs scripted FIX message exchange scenarios and validates behavior.

### External Interop (QuickFIX)

The `tools/external-interop/` directory contains Docker-based scripts for cross-testing against QuickFIX:

- `run_quickfix_python_matrix.py` — Runs a matrix of QuickFIX Python acceptor/initiator scenarios against FastFix endpoints
- `build/external-interop/quickfix-acceptor/` and `quickfix-initiator/` — Docker build contexts

---

## Key Internal APIs for Extension

### Adding a New Store Backend

Implement `SessionStore`:

```cpp
class MyStore : public fastfix::store::SessionStore {
    auto SaveOutbound(const MessageRecord& record) -> Status override;
    auto SaveInbound(const MessageRecord& record) -> Status override;
    auto LoadOutboundRange(uint64_t session_id, uint32_t begin, uint32_t end)
        -> Result<std::vector<MessageRecord>> override;
    auto SaveRecoveryState(const SessionRecoveryState& state) -> Status override;
    auto LoadRecoveryState(uint64_t session_id)
        -> Result<SessionRecoveryState> override;
    auto Flush() -> Status override;
    auto Rollover() -> Status override;
};
```

### Adding Application Callbacks

Implement `ApplicationCallbacks`:

```cpp
class MyApp : public fastfix::runtime::ApplicationCallbacks {
    auto OnSessionEvent(const RuntimeEvent& event) -> Status override;
    auto OnAdminMessage(const RuntimeEvent& event) -> Status override;
    auto OnAppMessage(const RuntimeEvent& event) -> Status override;
};
```

### Custom Validation

Set `ValidationPolicy` on session config:

| Mode | Behavior |
|------|----------|
| `kStrict` | Reject unknown tags, enforce field order, no duplicates |
| `kCompatible` | Allow unknown tags, relaxed ordering |
| `kPermissive` | Allow unknown tags + duplicates + order violations |
| `kRawPassThrough` | Accept any legal FIX byte stream |

---

## Project Conventions

- **Error handling**: Return `Status` or `Result<T>` — no exceptions on hot paths.
- **Memory**: Zero-copy views on the hot path. Owned `Message` only when mutation is needed.
- **Threading**: Single-writer per session. Cross-thread via `SpscQueue`. Mutexes only for cold-path operations.
- **Naming**: `snake_case` for functions and variables, `PascalCase` for types, `kPascalCase` for enum values.
- **Headers**: Each module has a public include directory under `include/fastfix/`. Internal helpers stay in `src/`.
