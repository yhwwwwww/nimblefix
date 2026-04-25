# NimbleFIX Development Guide

This guide covers everything you need to contribute to or extend NimbleFIX: building, testing, profiling, adding protocol support, and using the toolchain.

---

## Build System

NimbleFIX supports both offline xmake and offline CMake flows. xmake is the primary path for local Linux work when you have a recent upstream xmake; CMake is the portability and CI fallback and remains the canonical path for the named RHEL presets.

### Prerequisites

- C++20 compiler (GCC 12+ or Clang 15+)
- xmake 3.0.0+ (preferred direct path)
- or CMake 3.20+ plus Ninja (preferred CMake generator) or make (fallback when Ninja is unavailable)
- Linux x86_64 (primary platform)

### Build Targets

| Target | Kind | Description |
|--------|------|-------------|
| `nimblefix` | static library | Core engine library |
| `nimblefix-tests` | binary | Catch2 test suite |
| `nimblefix-bench` | binary | Performance benchmarks |
| `nimblefix-dictgen` | binary | Dictionary → artifact compiler |
| `nimblefix-xml2nfd` | binary | QuickFIX XML → `.nfd` converter |
| `nimblefix-initiator` | binary | CLI FIX client |
| `nimblefix-acceptor` | binary | CLI FIX server (echo) |
| `nimblefix-soak` | binary | Stress tester with fault injection |
| `nimblefix-interop-runner` | binary | Interop scenario runner |
| `nimblefix-fix-session-testcases` | binary | Official FIX session testcase runner and coverage report generator |
| `nimblefix-fuzz-codec` | binary | Codec/admin fuzzer |
| `nimblefix-fuzz-config` | binary | Config parser fuzzer |
| `nimblefix-fuzz-dictgen` | binary | Dictionary parser fuzzer |

### Common Commands

```bash
xmake f -c -m release --ccache=n -y
xmake build nimblefix-tests
xmake build nimblefix-bench
xmake build nimblefix-fix-session-testcases
xmake clean

bash ./scripts/offline_build.sh --bench smoke   # Auto: xmake >= 3.0.0 -> cmake + Ninja -> cmake + make

# Alternative CMake path
bash ./scripts/offline_build.sh --build-system cmake --preset dev-release --bench smoke
bash ./scripts/offline_build.sh --build-system cmake --cmake-generator make --preset dev-release --bench smoke
cmake --preset dev-release
cmake --build --preset dev-release
ctest --preset dev-release
```

The helper scripts auto-select `xmake >= 3.0.0`, then `cmake + Ninja`, then `cmake + make`. Default xmake builds write executables to `./build/linux/x86_64/release/`. Ninja-based CMake presets remain available at `./build/cmake/<preset>/bin/`, and make-based fallback presets live at `./build/cmake/<preset>-make/bin/`. The examples below assume `BIN_DIR=./build/linux/x86_64/release` for the default path and invoke binaries directly for reproducibility and predictable argument handling.

`offline_build.sh` now gates the repository's official FIX session baseline after the Catch2 or CTest pass. The default local and CI regression path therefore covers both the core unit/integration suite and `tests/data/fix-session/official-session-cases.nfcases`.

Both `offline_build.sh` and `bench.sh` default `NIMBLEFIX_XMAKE_CCACHE=n` on the xmake path. That mirrors the CI-safe setting and avoids Linux `.build_cache/... file busy` failures seen on the larger benchmark and QuickFIX targets.

GitHub Actions CI now uses the same helper logic on Ubuntu. On Ubuntu 24.04 the distro xmake package is currently `2.8.7`, so the helper intentionally falls back to CMake there unless a newer upstream xmake is installed. The named RHEL coverage still runs through `ubi8/ubi:8.10` + `gcc-toolset-12` and `ubi9/ubi:9.7` + `gcc-toolset-14` container jobs.

### Environment-Specific Notes

| Environment | Recommended path | What to watch for |
|-------------|------------------|-------------------|
| Local Linux workstation | xmake helper auto path or direct `xmake f -c -m release --ccache=n -y` | use xmake 3.0.0+; if you switch compilers or stale config leaks in, re-run with `-c` |
| Ubuntu 24.04 / GitHub Actions | helper auto path | distro xmake `2.8.7` is too old, so auto mode falls back to `cmake + Ninja`; explicit xmake requests need a newer upstream install |
| Root shell / container repro | prefer helper auto path or CMake | newer xmake versions may refuse running as root; CMake is the least surprising path for container debugging |
| RHEL 8.10 + GCC 12 | `cmake --preset rhel8-gcc12` | enable `gcc-toolset-12` first; use the RHEL preset instead of ad-hoc xmake |
| RHEL 9.7 + GCC 14 | `cmake --preset rhel9-gcc14` | enable `gcc-toolset-14`; keep `liburing-devel` aligned with the container image |

### Build Caches, Generators, And Submodules

- xmake helper flows always reconfigure with `xmake f -c` so stale compiler or cache state does not leak across runs. If you drive xmake manually and change compilers or cache policy, do the same.
- When switching the CMake generator between Ninja and make, either let the helper recreate the preset cache or clear the affected `build/cmake/<preset>*` directory yourself.
- Helper scripts auto-run `git submodule sync --recursive` and `git submodule update --init --recursive` when a checkout has `.git` metadata and required vendored sources are missing.
- Source archives without `.git` cannot self-heal their vendored dependencies; those archives must already contain `deps/src/*` and `bench/vendor/quickfix`.

### Dependencies

Pinned as Git submodules for offline builds:

- **Catch2 v3.13.0** — `deps/src/Catch2`
- **pugixml v1.15** — `deps/src/pugixml`
- **QuickFIX commit 00dd20837c97578e725072e5514c8ffaa0e141d4** — `bench/vendor/quickfix`

Initialize them with `git submodule update --init --recursive` before the first configure/build run.

### Compiler Defines

| Define | Purpose |
|--------|---------|
| `NIMBLEFIX_PROJECT_DIR` | Set to `$(projectdir)` for test data path resolution |

---

## Source Layout

```
include/
├── public/nimblefix/
│   ├── base/       Exported Status / Result surface
│   ├── codec/      Exported codec entry points
│   ├── message/    Exported message APIs
│   ├── profile/    Exported profile loading/query APIs
│   ├── session/    Exported session APIs
│   ├── store/      Exported store interfaces and implementations
│   ├── transport/  Exported transport surface
│   └── runtime/    Exported engine/runtime APIs
└── internal/nimblefix/
    └── ...         Repository-private headers for implementation, tests, and tools
```

The canonical exported headers live under `include/public/nimblefix/`. Repository-private headers live under `include/internal/nimblefix/`. Each module still has a corresponding source directory under `src/` and tests under `tests/`.

---

## Testing

### Framework

Tests use **Catch2 v3**. The test binary is `nimblefix-tests`.

```bash
BIN_DIR=./build/linux/x86_64/release

# Run all tests
$BIN_DIR/nimblefix-tests

# Run by tag
$BIN_DIR/nimblefix-tests "[fix-codec]"
$BIN_DIR/nimblefix-tests "[session-core]"
$BIN_DIR/nimblefix-tests "[negative]"

# Run by name
$BIN_DIR/nimblefix-tests "Heartbeat round-trip"

# List all tags
$BIN_DIR/nimblefix-tests --list-tags
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
| `[admin-protocol]` | Admin message handling (Logon, Heartbeat, etc.) |
| `[transport-fault]` | Network fault injection |
| `[store-recovery]` | Store persistence and recovery |
| `[socket-loopback]` | TCP loopback |
| `[sharded-runtime]` | Multi-shard engine |
| `[profile-loader]` | Artifact loading |
| `[timer-wheel]` | Timer scheduling |
| `[message-api]` | Message builder/view API |
| `[fixed-layout]` | FixedLayout / FixedLayoutWriter |
| `[generated-writer]` | Generated typed writer classes |
| `[typed-message]` | Typed message accessors |
| `[normalized-dictionary]` | FIX dictionary structures |
| `[dictgen]` | Dictionary compilation |
| `[overlay-merge]` | Profile overlay merging |
| `[parser-surface]` | Input format parsing |
| `[resend-recovery]` | Resend request handling |
| `[gap-transition]` | Gap state transition edge cases |
| `[metrics-trace]` | Metrics and trace collection |
| `[runtime-config]` | Configuration parsing |
| `[admin-fuzz-corpus]` | Admin protocol corpus |
| `[application-queue]` | App message queueing |
| `[application-poller]` | App polling |
| `[live-runtime]` | End-to-end runtime |
| `[live-backpressure]` | Backpressure handling |
| `[live-session-factory]` | Dynamic session factory |
| `[interop-harness]` | Interop scenarios |
| `[fix-session-testcases]` | Official FIX session manifest and importer coverage |
| `[live-initiator]` | Live initiator mode |
| `[live-initiator-queue]` | Queue-decoupled initiator |
| `[outbound-fragment]` | Outbound fragment encoding |
| `[raw-passthrough]` | Raw pass-through mode |
| `[precompiled-table]` | Precompiled template table |
| `[xml2nfd]` | QuickFIX XML to NFD conversion |
| `[wraparound]` | Sequence number wraparound |
| `[soak-smoke]` | Basic soak test |
| `[soak-long]` | Extended soak test |
| `[soak-multihour]` | Multi-hour soak test |
| `[soak-multiworker]` | Multi-worker soak test |
| `[reset-seq]` | Sequence reset behavior |
| `[timer-deadline]` | Timer deadline semantics |

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

- **Profile**: A normalized description of a FIX protocol variant. Each profile has a unique `profile_id` (uint64). Profiles are loaded at engine startup and referenced by sessions. One engine can load multiple profiles simultaneously (e.g., FIX 4.2 and FIX 4.4). A profile can be loaded from a precompiled `.nfa` file or built in memory from `.nfd` files at startup.

- **`.nfd` (NimbleFIX Dictionary)**: A text file defining baseline or incremental dictionary content for one FIX protocol variant — fields, messages, and groups. The first `.nfd` establishes the profile, and additional `.nfd` files can extend it when multiple files are passed to dictgen or Engine.

- **`.nfa` (Artifact)**: The compiled binary output of dictgen (optional precompilation step). It's a flat, mmap-loadable file containing string tables, field/message/group definitions, validation rules, and lookup tables. The engine can load `.nfa` files at runtime, or load `.nfd` files directly.

- **`.nfct` (Contract Sidecar)**: The cold-path behavior companion generated from FIX Orchestra input. It stores conditional required/forbidden rules, enum/code constraints, role and direction limits, service subsets, flow edges, source rule/case trace, and importer warnings. `.nfct` is loaded separately from `.nfa` through `EngineConfig::profile_contracts`; it is never consulted from the steady-state codec/session hot path.

- **`schema_hash`**: A 64-bit hash of the dictionary content, embedded in the `.nfa` header. Used internally as part of the codec template cache key to ensure cached templates are invalidated when the dictionary changes. Auto-computed by dictgen from the dictionary content — users don't need to set it manually.

### Artifact Layering

- `.nfd` / `.nfa` remain purely structural: fields, messages, groups, and lookup tables.
- `.nfct` carries behavior and contract metadata only: conditional field rules, role-direction restrictions, service subsets, flow edges, trace IDs, and importer warnings.
- `EngineConfig` is deployment glue: `profile_artifacts` / `profile_dictionaries` choose structure, `profile_contracts` chooses optional behavior sidecars, and `CounterpartyConfig::contract_service_subsets` selects which contract service subsets apply to one deployed session.
- Unsupported Orchestra features are downgraded during import into explicit warnings. Runtime hot paths do not parse XML or interpret Orchestra rules per message.

### `.nfd` Format Specification

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

### `.nfo` Overlay Format — Removed

The separate `.nfo` overlay format has been removed. Multiple `.nfd` files can be merged instead. The first `.nfd` provides the baseline (`profile_id` required), additional `.nfd` files extend it (adding fields, extending messages, overriding field rules). No separate overlay format needed.

Example of an incremental `.nfd` overlay (no header lines needed — `profile_id` comes from the base `.nfd`):

```
# Add venue-specific custom fields
field|5001|VenueOrderType|string|1
field|5002|VenueAccount|string|1

# Extend NewOrderSingle with custom fields
message|D|NewOrderSingle|0|5001:r,5002:o
```

### Compiling Profiles

```bash
$BIN_DIR/nimblefix-dictgen \
    --input samples/basic_profile.nfd \
    --merge samples/basic_overlay.nfd \
    --output build/sample-basic.nfa \
    --cpp-builders build/generated/sample_basic_builders.h
```

**Flags:**

| Flag | Required | Description |
|------|----------|-------------|
| `--input` | yes | Base dictionary file (`.nfd`) |
| `--merge` | no | Additional `.nfd` file(s) to merge, can specify multiple (also accepts `--overlay` for backwards compatibility) |
| `--output` | yes | Output artifact path (`.nfa`) |
| `--cpp-builders` | no | Generate C++ header with typed writer classes and profile constants |

**Note:** `.nfa` precompilation is optional. The Engine can load `.nfd` files directly at runtime via the `dictionary=` config line or `config.profile_dictionaries`.

**Output:**

- `.nfa` — Binary artifact (mmap-loadable). Contains string table, field/message/group definitions, validation rules, lookup tables.
- C++ header (optional) — Typed writer classes backed by `FixedLayoutWriter` (compile-time type-safe, O(1) slot writes), Tag constants, profile ID and schema hash.

### Importing FIX Orchestra

```bash
$BIN_DIR/nimblefix-orchestra-import \
    --xml FIXOrchestra.xml \
    --profile-id 4400 \
    --output-nfd build/fix44-orchestra.nfd \
    --output-contract build/fix44-orchestra.nfct

$BIN_DIR/nimblefix-dictgen \
    --input build/fix44-orchestra.nfd \
    --output build/fix44-orchestra.nfa
```

Use `--contract <file> --dump`, `--markdown <file>`, or `--interop-dir <dir>` to inspect the sidecar and emit contract-driven `.nfscenario` augmentations. The importer currently supports structural mappings, conditional required/forbidden field rules, enum/code constraints, service subsets, and basic flow edges. Unsupported Orchestra semantics are preserved as sidecar warnings.

### Adding a New FIX Version

1. Obtain the QuickFIX XML data dictionary for your FIX version (e.g. `FIX44.xml` from the [QuickFIX repository](https://github.com/quickfix/quickfix/tree/master/spec)).
2. Convert it to `.nfd` format:
   ```bash
    $BIN_DIR/nimblefix-xml2nfd \
       --xml FIX44.xml \
       --output my_fix44.nfd \
       --profile-id 1001 \
       --cpp-builders generated_builders.h
   ```
3. (Optional) Write additional `.nfd` files for venue-specific custom fields and pass them with `--merge`.
4. Compile with `nimblefix-dictgen`.
5. Reference the `.nfa` in `EngineConfig.profile_artifacts`. Or skip `.nfa` and reference `.nfd` directly in `EngineConfig.profile_dictionaries`.

You can also write `.nfd` files by hand for full control.

### Generated Profile Header

The `--cpp-builders` flag produces a header with typed writer classes and profile constants:

```cpp
#include "build/generated/sample_basic_builders.h"

using namespace nimble::generated::profile_1001;

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
./bench/bench.sh nimblefix
./bench/bench.sh nimblefix-nfd
./bench/bench.sh quickfix
./bench/bench.sh builder
./bench/bench.sh compare
```

`compare` is the command used for the published side-by-side README numbers. Its default args are `--iterations 100000 --loopback 1000 --replay 1000 --replay-span 128`.

All benchmark entrypoints intentionally consume the pinned QuickFIX 4.4 inputs, either through `bench/vendor/quickfix/spec/FIX44.xml` or the generated `build/bench/quickfix_FIX44.nfd` / `build/bench/quickfix_FIX44.nfa` outputs. The canonical benchmark-local breakdown now lives in [bench/README.md](../bench/README.md), including exact timing boundaries, diagrams, and the meaning of every printed metric.

### What Gets Measured

| Boundary | NimbleFIX metric | QuickFIX metric | What it means |
|----------|----------------|-----------------|---------------|
| header sniff | `peek` | — | NimbleFIX-only header extraction before full decode |
| object → wire | `encode` | `quickfix-encode-buffer` | closest shared encode boundary; both sides reuse an output buffer |
| object → wire (fresh string) | — | `quickfix-encode` | QuickFIX-only serializer path that returns a fresh string |
| wire → object | `parse` | `quickfix-parse` | full frame parse back into each engine's object/view model |
| session inbound | `session-inbound` | `quickfix-session-inbound` | decode + session/admin rules + store interaction on an inbound app frame |
| session outbound | `session-outbound` | — | `AdminProtocol::SendApplication(...)` — send path including session envelope, store, and encode |
| session outbound (pre-encoded) | `session-outbound-pre-encoded` | — | `SendEncodedApplication(...)` — same path but business body already serialized |
| replay | `replay` | `quickfix-replay` | ResendRequest handling across `replay_span=128` stored messages |
| TCP round-trip | `loopback-roundtrip` | `quickfix-loopback` | full userspace-to-kernel-to-userspace message RTT |

### Instrumentation

- **Allocation tracking**: Global `operator new` override counts heap allocations per iteration.
- **CPU counters**: Linux `perf_event_open` captures cache misses, branch mispredictions, instructions.
- **Percentiles**: per-iteration `steady_clock` samples → p50/p95/p99/p999.

---

## Stress Testing (Soak)

The soak tool runs multiple sessions with configurable fault injection:

```bash
$BIN_DIR/nimblefix-soak --profile build/sample-basic.nfa \
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
$BIN_DIR/nimblefix-fuzz-codec \
    --artifact build/sample-basic.nfa \
    --input tests/data/fuzz/wire_codec \
    --mode codec     # or: admin
```

- `codec` mode: fuzzes `DecodeFixMessageView` and `PeekSessionHeaderView`
- `admin` mode: fuzzes `AdminProtocol::OnInbound` with decoded messages

### Config Fuzzer

```bash
$BIN_DIR/nimblefix-fuzz-config --input tests/data/fuzz/
```

Fuzzes the `.nfcfg` engine configuration file parser.

### Dictionary Fuzzer

```bash
$BIN_DIR/nimblefix-fuzz-dictgen \
    --input tests/data/fuzz/
```

Fuzzes `.nfd` file parsing.

### Corpus

Test data lives in `tests/data/fuzz/`:

```
wire_codec/          FIX frame corpus (valid + malformed)
admin_protocol/      Admin message sequence corpus (.fixseq)
dictgen_*.nfd        Dictionary corpus
overlay_*.nfd        Overlay corpus
runtime_config_*.nfcfg  Config corpus
```

---

## Live Tools

### Initiator (Client)

```bash
$BIN_DIR/nimblefix-initiator \
    --artifact build/sample-basic.nfa \
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
$BIN_DIR/nimblefix-acceptor \
    --artifact build/sample-basic.nfa \
    --bind 0.0.0.0 --port 9876 \
    --sender VENUE_A --target MY_FIRM

# Config file mode
$BIN_DIR/nimblefix-acceptor --config engine.nfcfg --listener main
```

### Local Testing

```bash
# Terminal 1: start acceptor
$BIN_DIR/nimblefix-acceptor \
    --artifact build/sample-basic.nfa \
    --bind 127.0.0.1 --port 9001 \
    --sender SERVER --target CLIENT

# Terminal 2: connect initiator
$BIN_DIR/nimblefix-initiator \
    --artifact build/sample-basic.nfa \
    --host 127.0.0.1 --port 9001 \
    --sender CLIENT --target SERVER
```

---

## Configuration File Format (`.nfcfg`) — Internal

The `.nfcfg` format is used by NimbleFIX's built-in tools (`nimblefix-acceptor`, `nimblefix-interop-runner`, `nimblefix-fix-session-testcases`) and tests. It is **not** part of the library's public API. Applications should populate `EngineConfig` directly from their own configuration source (TOML, YAML, JSON, database, etc.) and call `Engine::Boot(config)`.

```
engine.worker_count=2
engine.enable_metrics=true
engine.trace_mode=ring
engine.trace_capacity=8
profile=path/to/profile.nfa
contract=path/to/profile.nfct
listener|main|127.0.0.1|9921|0
counterparty|fix44-demo|4201|1001|FIX.4.4|SELL|BUY|memory||memory|inline|30|false

# FIXT.1.1 adds DefaultApplVerID as the optional 14th field:
counterparty|fixt11-demo|5101|1001|FIXT.1.1|SELL|BUY|memory||memory|inline|30|false|9
```

Minimal counterparty record order (13 fields):

```text
counterparty|name|session_id|profile_id|begin_string|sender|target|store_mode|store_path|recovery_mode|dispatch_mode|heartbeat_sec|is_initiator
```

`profile=` may be repeated. `dictionary=` may also be repeated, and each `dictionary=` line accepts a comma-separated base-plus-overlay `.nfd` group that is loaded as one merged dictionary set. `contract=` may also be repeated; each line points to one `.nfct` sidecar keyed by `profile_id`.

`dispatch_mode` is per-counterparty (`inline` or `queue`). `engine.queue_app_mode` is engine-wide (`co-scheduled` or `threaded`) and only affects counterparties that use queue dispatch. When every counterparty uses `inline`, `engine.queue_app_mode` has no effect.

`SessionHandle::Snapshot()` / `Subscribe()` may be called from any thread. `SessionHandle::SendCopy()` / `SendTake()` / `SendEncodedCopy()` / `SendEncodedTake()` use a single-producer SPSC command queue per worker, so one producer thread must own a given handle's send path; a second producer thread will now get `kInvalidArgument`. `SendInlineBorrowed()` / `SendEncodedInlineBorrowed()` are narrower still: they are only valid inside inline runtime callbacks where the borrowed message/view lifetime is tied to the callback scope.

Ready-to-run examples live in:

- `tests/data/interop/loopback-runtime.nfcfg`
- `tests/data/interop/runtime-multiversion.nfcfg`

Full counterparty record order used by current tools/tests:

```text
counterparty|name|session_id|profile_id|begin_string|sender|target|store_mode|store_path|recovery_mode|dispatch_mode|heartbeat_sec|is_initiator|default_appl_ver_id|validation_mode|durable_flush_threshold|durable_rollover_mode|durable_archive_limit|reconnect_enabled|reconnect_initial_ms|reconnect_max_ms|reconnect_max_retries|durable_local_utc_offset_seconds|durable_use_system_timezone|day_cut_mode|day_cut_hour|day_cut_minute|day_cut_utc_offset|reset_seq_num_on_logon|reset_seq_num_on_logout|reset_seq_num_on_disconnect|refresh_on_logon|send_next_expected_msg_seq_num|use_local_time|non_stop_session|start_time|end_time|start_day|end_day|logon_time|logout_time|logon_day|logout_day|sending_time_threshold_seconds|supported_app_msg_types|application_messages_available|contract_service_subsets
```

The newer FIX-behavior columns expose the QuickFIX-style session controls now wired through runtime config and `CounterpartyConfig`:

- `reset_seq_num_on_logon`, `reset_seq_num_on_logout`, `reset_seq_num_on_disconnect`: reset the persisted session state and next inbound/outbound sequences back to `1` on the corresponding lifecycle edge.
- `refresh_on_logon`: reload persisted recovery state from the bound store right before logon processing/generation.
- `send_next_expected_msg_seq_num`: include tag `789` on Logon and honor the peer's advertised next-expected outbound range.
- `start_time`, `end_time`, `start_day`, `end_day`: session window.
- `logon_time`, `logout_time`, `logon_day`, `logout_day`: optional narrower logon window.
- `use_local_time`: interpret the schedule in local wall clock time; otherwise UTC is used.
- `non_stop_session`: disable schedule gating entirely. It must not be combined with explicit session/logon window fields.
- `supported_app_msg_types`: optional inbound app-message allowlist. When a matching `.nfct` sidecar is loaded, this list must remain within the contract receive subset.
- `application_messages_available`: explicit maintenance flag. When false, known application messages are rejected with `BusinessMessageReject(380=4)`.
- `contract_service_subsets`: deployment-selected Orchestra service subsets for the bound profile. These are resolved only on cold paths and never trigger per-message Orchestra interpretation.

Time fields use `HH:MM:SS`. Day fields accept `sun`..`sat` (or full names). Empty trailing columns keep the default behavior.

Load in code:

```cpp
// Internal tool use only — #include "nimblefix/runtime/internal_config_parser.h"
auto config = nimble::runtime::LoadEngineConfigFile("engine.nfcfg");
```

---

## Interop Testing

### Internal Interop

```bash
$BIN_DIR/nimblefix-interop-runner --scenario tests/data/interop/loopback.nfscenario
```

Runs scripted FIX message exchange scenarios and validates behavior.

For scenario authoring or mismatch debugging, `nimblefix-interop-runner` also supports `--dump-report`:

```bash
$BIN_DIR/nimblefix-interop-runner --scenario tests/data/fix-session/cases/2s-first-message-not-logon.nfscenario --dump-report
```

### Official FIX Session Cases

```bash
$BIN_DIR/nimblefix-fix-session-testcases --manifest tests/data/fix-session/official-session-cases.nfcases
$BIN_DIR/nimblefix-fix-session-testcases --manifest tests/data/fix-session/official-session-cases.nfcases --filter 1S
$BIN_DIR/nimblefix-fix-session-testcases --manifest tests/data/fix-session/official-session-cases.nfcases --report audits/20260425-fix-session-coverage.md
$BIN_DIR/nimblefix-fix-session-testcases --import-html /path/to/session-cases.html --output /tmp/official-session-cases.nfcases
```

The repository keeps the canonical Phase 11 baseline as `tests/data/fix-session/official-session-cases.nfcases` plus mapped `.nfscenario` files under `tests/data/fix-session/cases/`. Raw FIX Trading HTML is not required at build or test time; if you obtain an updated official page offline, use `--import-html` to bootstrap a local manifest and then map cases onto the existing interop harness.

### External Interop (QuickFIX)

The `tools/external-interop/` directory contains Docker-based scripts for cross-testing against QuickFIX:

- `run_quickfix_python_matrix.py` — Runs a matrix of QuickFIX Python acceptor/initiator scenarios against NimbleFIX endpoints
- `build/external-interop/quickfix-acceptor/` and `quickfix-initiator/` — Docker build contexts

---

## Key Internal APIs for Extension

### Adding a New Store Backend

Implement `SessionStore`:

```cpp
class MyStore : public nimble::store::SessionStore {
    auto SaveOutbound(const MessageRecord& record) -> Status override;
    auto SaveInbound(const MessageRecord& record) -> Status override;
    auto LoadOutboundRange(uint64_t session_id, uint32_t begin, uint32_t end)
        -> Result<std::vector<MessageRecord>> override;
    auto SaveRecoveryState(const SessionRecoveryState& state) -> Status override;
    auto LoadRecoveryState(uint64_t session_id)
        -> Result<SessionRecoveryState> override;
    auto Refresh() -> Status override;
    auto ResetSession(uint64_t session_id) -> Status override;
    auto Flush() -> Status override;
    auto Rollover() -> Status override;
};
```

### Adding Application Callbacks

Implement `ApplicationCallbacks`:

```cpp
class MyApp : public nimble::runtime::ApplicationCallbacks {
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
- **Headers**: Exported headers live under `include/public/nimblefix/`. Repository-private headers live under `include/internal/nimblefix/` and are allowed only for NimbleFIX itself, tests, and internal tools.
