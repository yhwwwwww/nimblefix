# NimbleFIX

**A lean, low-latency FIX engine for latency-sensitive trading systems.**

[中文文档](README_CN.md)

---

## What is NimbleFIX?

NimbleFIX is a C++20 FIX (Financial Information eXchange) protocol engine built from scratch for latency-sensitive trading infrastructure. It handles the full FIX session lifecycle — connection management, Logon/Logout handshake, sequence number tracking, heartbeat monitoring, gap detection, resend recovery — while keeping the hot path allocation-free and lock-free.

NimbleFIX operates as both **initiator** (client) and **acceptor** (server) using the same internal engine. It supports FIX 4.2, 4.3, 4.4, and FIXT.1.1 transport sessions.

## FIX Standards Coverage

NimbleFIX is intentionally focused on the classic low-latency FIX stack, not the entire FIX Family of Standards. Current coverage against the major FIX Trading standards families is:

| Standard family | Status | Current scope |
|---------|---------|---------|
| **Classic FIX session layer** | **Implemented** | FIX 4.x / FIXT.1.1 initiator and acceptor runtime with Logon/Logout, heartbeat, sequence tracking, gap detection, resend recovery, reconnect, and persistence |
| **FIX tagvalue encoding** | **Implemented** | Core codec, message builders, fixed-layout writers, raw pass-through, SIMD-assisted tag/value parsing |
| **FIX over TLS (FIXS)** | **Implemented** | Optional OpenSSL-backed TLS transport, enabled at runtime per initiator counterparty or acceptor listener |
| **FIX application-layer dictionaries** | **Partial** | Dictionary-driven `.nfd` / `.nfa` model with QuickFIX XML and FIX Orchestra XML import tooling; Orchestra behavior rules stay in separate `.nfct` sidecars |
| **FIX official session test cases** | **Partial** | Offline FIX Trading session-case manifest plus executable `.nfscenario` baseline; 73 of 85 official cases currently map to in-tree passing scenarios, and the remaining 12 optional cases are explicitly tracked as unsupported |
| **FIX Orchestra** | **Partial** | Offline `nimblefix-orchestra-import` generates structural `.nfd` plus `.nfct` contract sidecars, dump/markdown/interop augmentations, and cold-path runtime binding with explicit unsupported warnings |
| **FIXP / SOFH** | **Absent** | No FIX Performance Session Layer or Simple Open Framing Header support |
| **FIXML / SBE / FAST** | **Absent** | No alternate wire encodings beyond classic tag=value FIX |
| **JSON / GPB / ASN.1 FIX encodings** | **Absent** | No alternate serialized FIX encodings currently implemented |
| **FIXatdl / MMT** | **Absent** | Not currently in scope for the engine/runtime/tooling stack |

## Why NimbleFIX?

Existing open-source FIX engines (QuickFIX, QuickFIX/J, Fix8) are designed for correctness and broad compatibility, not for raw speed. They parse messages into dynamic maps, allocate on every message, and lock shared state across threads. This is fine for 99% of use cases — but not for firms where single-digit microsecond latency matters.

NimbleFIX was designed to answer: *what if every design decision optimized for the hot path?*

- **Dictionary-backed codec**: FIX metadata is normalized once at startup, either from `.nfd` dictionaries or precompiled `.nfa` artifacts. The hot path uses the same in-memory dictionary representation either way.
- **Zero-copy message views**: Parsed messages are lightweight views over the original byte buffer. No copies until you explicitly request one.
- **Single-writer session model**: Each session is owned by exactly one worker thread. No locks, no contention, no false sharing on the critical path.
- **Pre-compiled frame templates**: For high-frequency message types, header/trailer fragments are pre-built. Only variable fields, BodyLength, and Checksum are filled per message.
- **SIMD-accelerated parsing**: SSE2 vectorized scanning for SOH delimiters and `=` separators in the tokenizer hot loop.

## Highlights

| Feature | Details |
|---------|---------|
| **Codec latency** | Current FIX44 compare run: 130 ns header peek (p50), 511 ns full parse (p50), 371 ns typed writer encode (p50) |
| **Low allocation pressure** | Current compare run: encode/parse/session/replay all at 0 alloc/op; loopback at 3 alloc/op |
| **Session management** | Full Logon/Logout/Heartbeat/TestRequest/ResendRequest/SequenceReset |
| **Repeating groups** | Nested groups fully supported via dictionary metadata |
| **Reconnect with backoff** | Configurable exponential backoff with jitter for initiator reconnect |
| **Optional TLS transport** | OpenSSL-backed TLS can be compiled in and enabled per initiator counterparty or acceptor listener |
| **Dynamic session factory** | Acceptor can accept unknown CompIDs via callback or whitelist |
| **Pluggable persistence** | Memory, mmap, and durable batch stores with configurable rollover |
| **Worker sharding** | Per-worker event loops with CPU affinity pinning |
| **Busy-poll mode** | Optional zero-timeout polling for lowest possible latency |
| **SIMD tokenizer** | SSE2 byte scanning with automatic scalar fallback |
| **Observability** | Built-in metrics registry and ring-buffer trace recorder |
| **Soak testing** | Fault injection harness (gaps, duplicates, reorders, disconnects) |
| **Fuzz testing** | libFuzzer harnesses for codec, config, and dictionary inputs |

## How NimbleFIX Differs from QuickFIX

[QuickFIX](http://www.quickfixengine.org/) is the de facto open-source FIX engine. It's mature, broadly used, and correct. NimbleFIX differs in philosophy:

| Aspect | QuickFIX | NimbleFIX |
|--------|----------|---------|
| **Parsing** | XML data dictionary loaded at runtime; fields stored in `std::map` | Normalized dictionary loaded from `.nfa` or parsed from `.nfd` at startup; hot path uses contiguous lookup sections |
| **Message representation** | `FieldMap` with heap-allocated strings | Zero-copy `MessageView` over original bytes |
| **Allocations per message** | Multiple (map insertions, string copies) | Zero in buffer-reuse encode path; minimal in parse path |
| **Threading** | Global session lock, thread-per-session | Lock-free single-writer per session, sharded workers |
| **Group handling** | Dynamic nesting with map lookups | Dictionary-driven stack with pre-known structure |
| **Encoding** | Assemble fields → serialize | Pre-compiled frame template, fill variable fields only |
| **Language** | C++ (pre-C++11 origin) | C++20 |

---

## Getting Started

### Prerequisites

- C++20 compiler (GCC 12+, Clang 15+)
- `xmake` 3.0.0+ (preferred for the direct xmake path)
- or CMake 3.20+ plus Ninja (preferred CMake generator) or make (fallback when Ninja is unavailable)

### Offline Dependency Layout

NimbleFIX now keeps the required third-party sources as pinned Git submodules for offline builds:

- `deps/src/Catch2` — Catch2 v3.13.0
- `deps/src/pugixml` — pugixml v1.15
- `bench/vendor/quickfix` — QuickFIX pinned to commit `00dd20837c97578e725072e5514c8ffaa0e141d4`, including `spec/FIX44.xml`

Initialize them once after cloning:

```bash
git submodule update --init --recursive
```

After the submodules are initialized, no build step downloads Catch2, pugixml, QuickFIX, or FIX dictionaries from the network.

### Build

```bash
# Auto-detect build path: xmake -> cmake + Ninja -> cmake + make
bash ./scripts/offline_build.sh --bench smoke

# Full benchmark run with the same auto-detect order
bash ./scripts/offline_build.sh --bench full

# Direct xmake path
xmake f -m release --ccache=n -y
xmake build nimblefix-tests
xmake build nimblefix-bench

# Alternative CMake path
bash ./scripts/offline_build.sh --build-system cmake --preset dev-release --bench smoke

# Force the make fallback if Ninja is unavailable or undesirable
bash ./scripts/offline_build.sh --build-system cmake --cmake-generator make --preset dev-release --bench smoke

# Named CMake presets for the supported offline targets (Ninja-first)
cmake --preset rhel8-gcc12
cmake --build --preset rhel8-gcc12
ctest --preset rhel8-gcc12

cmake --preset rhel9-gcc14
cmake --build --preset rhel9-gcc14
ctest --preset rhel9-gcc14

# Manual CMake flow without presets
cmake -S . -B build/cmake/dev-release-manual -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake/dev-release-manual
ctest --test-dir build/cmake/dev-release-manual --output-on-failure

# Manual make fallback when Ninja is unavailable
cmake -S . -B build/cmake/dev-release-manual-make -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake/dev-release-manual-make
ctest --test-dir build/cmake/dev-release-manual-make --output-on-failure

# Direct xmake target build, using the pinned Catch2/pugixml submodules instead of package downloads
xmake f -m release --ccache=n -y
xmake build nimblefix-tests
```

TLS support is optional. Enabling it at build time only links OpenSSL and compiles the TLS transport; connections still use plain TCP unless their runtime TLS config has `enabled=true`.

```bash
# CMake TLS-capable build
cmake -S . -B build/cmake/tls-release -DCMAKE_BUILD_TYPE=Release -DNIMBLEFIX_ENABLE_TLS=ON
cmake --build build/cmake/tls-release

# xmake TLS-capable build
xmake f -m release --nimblefix_enable_tls=true --ccache=n -y
xmake build nimblefix-tests
```

The helper scripts auto-select `xmake >= 3.0.0`, then `cmake + Ninja`, then `cmake + make`. On Ubuntu 24.04 the distro xmake package is currently `2.8.7`, so the helper intentionally logs a fallback to CMake there unless you install a newer upstream xmake. Both helper scripts also default `NIMBLEFIX_XMAKE_CCACHE=n` on the xmake path to avoid Linux `.build_cache/... file busy` failures seen on large targets.

GitHub Actions CI uses that same auto-selection logic on Ubuntu and still exercises the named RHEL CMake presets via `ubi8/ubi:8.10` + `gcc-toolset-12` and `ubi9/ubi:9.7` + `gcc-toolset-14` container jobs on every push and pull request.

When `build/generated/`, `build/bench/`, or `build/sample-basic.nfa` have been removed, `xmake build nimblefix-tests` and `xmake build nimblefix-bench` now regenerate the required shared assets automatically.

The helper scripts auto-select `xmake`, then `cmake + Ninja`, then `cmake + make`. Default xmake builds write executables under `build/linux/x86_64/release/`. Ninja-based CMake presets write to `build/cmake/<preset>/bin/`, and make-based fallback presets write to `build/cmake/<preset>-make/bin/`. The helper scripts keep using the shared repository-local assets under `build/generated/` and `build/bench/` so tests and benchmarks resolve the same files regardless of build system.

### Run Tests

```bash
./build/linux/x86_64/release/nimblefix-tests
# Alternative one-shot flow: bash ./scripts/offline_build.sh --bench skip
# Alternative CMake binary: ./build/cmake/dev-release/bin/nimblefix-tests
# Make fallback binary: ./build/cmake/dev-release-make/bin/nimblefix-tests
```

### Integration

NimbleFIX currently compiles into a single static library `libnimblefix.a`. To use it in your own program:

1. **Build the library**: `xmake f -m release -y && xmake build nimblefix` or, if you need the alternative path, `cmake --build build/cmake/dev-release --target nimblefix` (or `build/cmake/dev-release-make` when forcing the make fallback)
2. **Compile a protocol profile**: Run `nimblefix-dictgen` to produce a `.nfa` binary artifact from your dictionary (optional — `.nfd` files can also be loaded directly at runtime)
3. **Link and include**: Point your compiler at `include/public/` for headers and link against `libnimblefix.a`

**xmake (as a subdependency):**

```lua
-- xmake.lua
includes("path/to/nimblefix")   -- path to the nimblefix source tree

target("my-trading-app")
    set_kind("binary")
    add_deps("nimblefix")        -- links libnimblefix.a + adds include/public/
    add_files("src/*.cpp")
```

**CMake / Makefile / other (alternative):**

```bash
# 1. Build nimblefix first
cd path/to/nimblefix && cmake -S . -B build/cmake/dev-release -DCMAKE_BUILD_TYPE=Release && cmake --build build/cmake/dev-release --target nimblefix

# 2. In your build system, add:
#    Include path:  path/to/nimblefix/include/public
#    Link library:  path/to/nimblefix/build/cmake/dev-release/lib/libnimblefix.a
#    Standard:      C++20
```

### Public Headers

External consumers should include only the exported headers under `include/public/nimblefix/`; `include/internal/nimblefix/` is repository-private.

Most applications only need these direct includes:

- `nimblefix/runtime/application.h`
- `nimblefix/runtime/config.h`
- `nimblefix/runtime/engine.h`
- `nimblefix/runtime/live_acceptor.h`
- `nimblefix/runtime/live_initiator.h`
- `nimblefix/message/message_builder.h`
- `nimblefix/message/message_view.h`
- `nimblefix/message/fixed_layout_writer.h`
- `nimblefix/codec/fix_codec.h`
- `nimblefix/codec/fix_tags.h`
- `nimblefix/profile/profile_loader.h`
- `nimblefix/store/memory_store.h`
- `nimblefix/store/mmap_store.h`
- `nimblefix/store/durable_batch_store.h`
- `nimblefix/session/session_handle.h`

Advanced consumers can add `nimblefix/session/admin_protocol.h`, `nimblefix/session/resend_recovery.h`, `nimblefix/runtime/sharded_runtime.h`, `nimblefix/runtime/metrics.h`, and `nimblefix/runtime/trace.h` as needed. The complete exported header policy is documented in [docs/public-api.md](docs/public-api.md).

---

## Key Concepts

### Profile

A **profile** is a compiled binary description of one FIX protocol variant — which fields exist, which messages are defined, what groups they contain. Each profile carries a `profile_id` (uint64) that you choose when writing the dictionary. Sessions reference profiles by this ID, and one engine can load multiple profiles simultaneously (e.g., FIX 4.2 and FIX 4.4).

### `.nfd` — NimbleFIX Dictionary

A text file defining all fields, messages, and groups for one FIX protocol variant. Contains a `profile_id` you assign (any unique number, e.g. `1001` for FIX 4.4). Multiple `.nfd` files can be passed to `nimblefix-dictgen` or `Engine` — the first provides the baseline, additional files serve as incremental overlays (adding fields, extending messages, overriding field rules).

```
profile_id=1001

field|35|MsgType|string|0
field|11|ClOrdID|string|0
field|38|OrderQty|int|0
field|453|NoPartyIDs|int|0
field|448|PartyID|string|0

message|D|NewOrderSingle|0|35:r,11:r,38:r,453:o
group|453|448|Parties|0|448:r,447:r,452:r
```

Format: line-based, pipe-delimited. Lines starting with `#` are comments. Header lines are `key=value`. Field definition: `field|tag|name|type|flags`. Message definition: `message|msg_type|name|admin_flag|field_rules`. Group definition: `group|count_tag|delimiter_tag|name|flags|field_rules`. Field rules use `tag:r` (required) or `tag:o` (optional).

### `.nfa` — Artifact

The compiled binary output of `nimblefix-dictgen`. It's a flat, mmap-loadable file containing string tables, field/message/group definitions, validation rules, and lookup tables. The `profile_id` from your `.nfd` is embedded in it. Loading an `.nfa` file avoids text parsing at startup — useful for production deployments or when startup time matters.

### `.nfct` — Contract Sidecar

The behavior companion generated by `nimblefix-orchestra-import`. `.nfct` stays separate from `.nfa` on purpose: `.nfa` remains the structural, mmap-friendly hot-path dictionary artifact, while `.nfct` stores cold-path-only behavior such as conditional required/forbidden fields, enum/code constraints, role and direction limits, service subsets, flow edges, source rule IDs, and importer warnings. Runtime code loads `.nfct` through `EngineConfig::profile_contracts` and applies deployment-selected service subsets through `CounterpartyConfig::contract_service_subsets`. Unsupported Orchestra semantics are emitted as importer warnings and are not interpreted on the steady-state codec/session hot path.

### Overlay

Multiple `.nfd` files can be merged to extend a base profile with venue-specific custom fields. The first `.nfd` provides the baseline; additional files add fields, extend messages, or override field rules:

```
# venue_extensions.nfd — overlay with custom fields
field|5001|VenueOrderType|string|1
field|5002|VenueAccount|string|1
message|D|NewOrderSingle|0|5001:r,5002:o
```

### MessageBuilder vs FixedLayoutWriter

NimbleFIX provides two ways to construct outbound messages:

- **`MessageBuilder`** — Generic, flexible API. Works with any message type. No upfront setup. Good for infrequent messages or prototyping.
- **`FixedLayoutWriter`** — Hot-path optimized. Requires a `FixedLayout` built once from the dictionary. Pre-computes tag-to-slot mapping for O(1) writes. Significantly faster for high-frequency message types.

### MessageView

Zero-copy read-only accessor over a parsed FIX message. References the original byte buffer — no copies, no allocations. Accessors return `std::optional` (absent if field not present).

### ValidationPolicy

Controls how strict the codec is during decode:

| Mode | Behavior |
|------|----------|
| `kStrict` | Reject unknown tags, enforce field order, no duplicates |
| `kCompatible` | Allow unknown tags, relax ordering slightly |
| `kPermissive` | Allow unknown tags, duplicates, order violations |
| `kRawPassThrough` | Accept any valid FIX byte stream |

---

## Tools

| Tool | Purpose |
|------|---------|
| `nimblefix-dictgen` | Compile `.nfd` dictionaries into binary `.nfa` profiles (supports merging multiple `.nfd` files and generating C++ builder headers) |
| `nimblefix-xml2nfd` | Convert QuickFIX XML data dictionaries to `.nfd` format; also generates C++ builder headers from `.nfd` |
| `nimblefix-orchestra-import` | Convert FIX Orchestra XML into structural `.nfd` input plus `.nfct` contract sidecars; can also dump, render markdown, and emit interop augmentations from an existing sidecar |
| `nimblefix-initiator` | CLI FIX initiator for testing and interop |
| `nimblefix-acceptor` | CLI FIX acceptor (echo server) |
| `nimblefix-soak` | Stress test with fault injection (gaps, duplicates, reorders, disconnects) |
| `nimblefix-bench` | Latency/throughput benchmark with allocation and CPU counter tracking |
| `nimblefix-fuzz-codec` | libFuzzer harness for codec and admin protocol |
| `nimblefix-fuzz-config` | libFuzzer harness for `.nfcfg` config parser |
| `nimblefix-fuzz-dictgen` | libFuzzer harness for `.nfd` dictionary parser |
| `nimblefix-interop-runner` | Bidirectional interoperability scenario runner |

### Compile a Profile

```bash
./build/linux/x86_64/release/nimblefix-dictgen \
    --input samples/basic_profile.nfd \
    --merge samples/basic_overlay.nfd \
    --output build/sample-basic.nfa \
    --cpp-builders build/generated/sample_basic_builders.h
```

| Flag | Required | Description |
|------|----------|-------------|
| `--input` | yes | Base dictionary file (`.nfd`) |
| `--merge` | no | Additional `.nfd` file(s) to merge (repeatable) |
| `--output` | yes | Output artifact path (`.nfa`) |
| `--cpp-builders` | no | Generate C++ header with profile constants |

### Convert from QuickFIX XML

```bash
./build/linux/x86_64/release/nimblefix-xml2nfd \
    --xml FIX44.xml \
    --output my_profile.nfd \
    --profile-id 1001 \
    --cpp-builders generated_builders.h
```

Components are inlined, groups are extracted, XML types are mapped to NimbleFIX types, and `schema_hash` is auto-computed.

### Import from FIX Orchestra XML

```bash
./build/linux/x86_64/release/nimblefix-orchestra-import \
    --xml FIXOrchestra.xml \
    --profile-id 4400 \
    --output-nfd build/fix44-orchestra.nfd \
    --output-contract build/fix44-orchestra.nfct

./build/linux/x86_64/release/nimblefix-dictgen \
    --input build/fix44-orchestra.nfd \
    --output build/fix44-orchestra.nfa
```

Use `--contract <file> --dump`, `--markdown <file>`, or `--interop-dir <dir>` to inspect the generated sidecar and emit contract-driven `.nfscenario` augmentations. Unsupported Orchestra rules are retained as warnings in the sidecar instead of being interpreted on the runtime hot path.

---

## Usage: Initiator

### Standard Initiator

```cpp
#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/live_initiator.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/runtime/application.h"

class MyApp : public nimble::runtime::ApplicationCallbacks {
    auto OnSessionEvent(const nimble::runtime::RuntimeEvent& event)
        -> nimble::base::Status override {
        if (event.session_event == nimble::runtime::SessionEventKind::kActive) {
            // Session is live — start sending orders
            auto msg = nimble::message::MessageBuilder{"D"}
                .set_string(11, "ORD-001")
                .set_char(54, '1')       // Side=Buy
                .set_float(44, 150.25)   // Price
                .set_int(38, 100)        // OrderQty
                .build();
            event.handle.SendTake(std::move(msg));
        }
        return nimble::base::Status::Ok();
    }

    auto OnAppMessage(const nimble::runtime::RuntimeEvent& event)
        -> nimble::base::Status override {
        auto view = event.message_view();
        auto msg_type = view.msg_type();      // "8" for ExecutionReport
        auto exec_id = view.get_string(17);   // ExecID
        return nimble::base::Status::Ok();
    }
};

int main() {
    nimble::runtime::EngineConfig config;
    config.worker_count = 1;
    config.profile_artifacts = {"build/sample-basic.nfa"};
    // Or load .nfd directly:
    // config.profile_dictionaries = {{"samples/basic_profile.nfd"}};
    config.counterparties = {{
        .name = "venue-a",
        .session = {
            .session_id = 1,
            .key = {"FIX.4.4", "MY_FIRM", "VENUE_A"},
            .profile_id = 1001,
            .heartbeat_interval_seconds = 30,
            .is_initiator = true,
        },
        .reconnect_enabled = true,
        .reconnect_initial_ms = 1000,
        .reconnect_max_ms = 30000,
    }};

    nimble::runtime::Engine engine;
    engine.LoadProfiles(config);
    engine.Boot(config);

    auto app = std::make_shared<MyApp>();
    nimble::runtime::LiveInitiator initiator(&engine, {
        .application = app,
    });

    initiator.OpenSession(1, "exchange.example.com", 9876);
    initiator.Run();
}
```

### Hot-Path Initiator with FixedLayoutWriter

For latency-critical order submission, use `FixedLayoutWriter` with a pre-built `FixedLayout`:

```cpp
#include "nimblefix/message/fixed_layout_writer.h"
#include "nimblefix/codec/fix_codec.h"

// Build once at startup:
auto layout = nimble::message::FixedLayout::Build(dictionary, "D").value();
auto templates = nimble::codec::PrecompiledTemplateTable{};

// ... inside OnSessionEvent when session becomes active:

class HotPathApp : public nimble::runtime::ApplicationCallbacks {
    nimble::message::FixedLayout layout_;
    nimble::codec::EncodeBuffer encode_buffer_;   // reuse across calls
    nimble::codec::EncodeOptions encode_options_;

    auto OnSessionEvent(const nimble::runtime::RuntimeEvent& event)
        -> nimble::base::Status override {
        if (event.session_event == nimble::runtime::SessionEventKind::kActive) {
            SendOrder(event.handle);
        }
        return nimble::base::Status::Ok();
    }

    void SendOrder(nimble::session::SessionHandle handle) {
        nimble::message::FixedLayoutWriter writer(layout_);
        writer.set_string(11, "ORD-001");    // ClOrdID
        writer.set_char(54, '1');             // Side=Buy
        writer.set_int(38, 100);             // OrderQty
        writer.set_float(44, 150.25);        // Price
        writer.set_string(55, "AAPL");       // Symbol

        // Add repeating group
        writer.add_group_entry(453)
            .set_string(448, "PARTY-A")
            .set_char(447, 'D')
            .set_int(452, 3);

        // Encode directly to reusable buffer — zero allocation
        auto status = writer.encode_to_buffer(dictionary_, encode_options_, &encode_buffer_);
        // ... send encoded bytes via handle
    }
};
```

The `FixedLayoutWriter` path is the lowest-allocation encode path in the library and is the same hot path measured as `encode` in the side-by-side benchmark section later in this README.

### Hybrid Path — FixedLayout + Extra Fields

When most fields are known at compile time but some are dynamic (e.g., venue-specific extensions):

```cpp
nimble::message::FixedLayoutWriter writer(layout_);
// Known fields via O(1) slot writes
writer.set_string(11, "ORD-001");
writer.set_char(54, '1');
writer.set_int(38, 100);

// Dynamic/extension fields not in the layout
writer.set_extra_string(5001, "LIMIT");      // VenueOrderType
writer.set_extra_string(5002, "ACCT-XYZ");   // VenueAccount

auto status = writer.encode_to_buffer(dictionary, options, &buffer);
```

---

## Usage: Acceptor

### Standard Acceptor with Dynamic Session Factory

```cpp
#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/live_acceptor.h"
#include "nimblefix/runtime/config.h"

nimble::runtime::EngineConfig config;
config.worker_count = 2;
config.profile_artifacts = {"build/sample-basic.nfa"};
config.listeners = {{
    .name = "main",
    .host = "0.0.0.0",
    .port = 9876,
}};
config.accept_unknown_sessions = true;

nimble::runtime::Engine engine;
engine.Boot(config);

// Accept sessions dynamically based on inbound Logon:
engine.SetSessionFactory([](const nimble::session::SessionKey& key)
    -> nimble::base::Result<nimble::runtime::CounterpartyConfig> {
    return nimble::runtime::CounterpartyConfig{
        .name = key.sender_comp_id,
        .session = {
            .profile_id = 1001,
            .key = key,
            .heartbeat_interval_seconds = 30,
        },
    };
});

auto app = std::make_shared<MyApp>();
nimble::runtime::LiveAcceptor acceptor(&engine, {
    .application = app,
});
acceptor.OpenListeners("main");
acceptor.Run();
```

### Acceptor with Pre-Configured Counterparties

```cpp
nimble::runtime::EngineConfig config;
config.worker_count = 1;
config.profile_artifacts = {"build/sample-basic.nfa"};
config.listeners = {{
    .name = "main",
    .host = "0.0.0.0",
    .port = 9876,
}};
config.counterparties = {{
    .name = "client-a",
    .session = {
        .session_id = 1,
        .key = {"FIX.4.4", "SERVER", "CLIENT_A"},
        .profile_id = 1001,
        .heartbeat_interval_seconds = 30,
        .is_initiator = false,
    },
    .store_mode = nimble::runtime::StoreMode::kDurableBatch,
    .store_path = "/var/lib/nimblefix/client-a",
}};

nimble::runtime::Engine engine;
engine.LoadProfiles(config);
engine.Boot(config);

auto app = std::make_shared<MyApp>();
nimble::runtime::LiveAcceptor acceptor(&engine, {
    .application = app,
});
acceptor.OpenListeners("main");
acceptor.Run();
```

### Hot-Path Acceptor: Reading Inbound Messages

In the `OnAppMessage` callback, use zero-copy `MessageView` for minimal overhead:

```cpp
auto OnAppMessage(const nimble::runtime::RuntimeEvent& event)
    -> nimble::base::Status override {
    auto view = event.message_view();

    // Zero-copy field access — no allocations
    auto cl_ord_id = view.get_string(11);   // → optional<string_view>
    auto side = view.get_char(54);           // → optional<char>
    auto qty = view.get_int(38);            // → optional<int64_t>
    auto price = view.get_float(44);        // → optional<double>

    // Repeating group access
    if (auto group = view.group(453)) {
        for (std::size_t i = 0; i < group->size(); ++i) {
            auto entry = group->entry(i);
            auto party_id = entry.get_string(448);
        }
    }

    // Build and send a response
    auto response = nimble::message::MessageBuilder{"8"}
        .set_string(17, "EXEC-001")        // ExecID
        .set_string(11, cl_ord_id.value_or(""))
        .set_char(150, '0')                // ExecType=New
        .set_int(14, 0)                    // CumQty
        .build();
    event.handle.SendTake(std::move(response));

    return nimble::base::Status::Ok();
}
```

---

## Configuration Reference

### EngineConfig

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `worker_count` | uint32 | 1 | Number of worker threads |
| `poll_mode` | enum | `kBlocking` | `kBlocking` (epoll with timeout) or `kBusy` (spin-loop, lowest latency) |
| `enable_metrics` | bool | true | Enable built-in metrics collection |
| `trace_mode` | enum | `kDisabled` | `kDisabled` or `kRing` (ring-buffer trace) |
| `trace_capacity` | uint32 | 0 | Ring trace buffer size |
| `profile_artifacts` | paths | — | Paths to compiled `.nfa` profile files |
| `profile_dictionaries` | path[][] | — | Paths to `.nfd` dictionary file groups to load directly at runtime (each inner vector is merged as base + overlays) |
| `profile_contracts` | paths | — | Paths to `.nfct` contract sidecars loaded on cold paths and matched by `profile_id` |
| `profile_madvise` | bool | false | Call `madvise(MADV_WILLNEED)` on loaded artifacts |
| `profile_mlock` | bool | false | Call `mlock()` on loaded artifacts (requires sufficient RLIMIT) |
| `front_door_cpu` | uint32? | — | Pin acceptor front-door thread to CPU core |
| `worker_cpu_affinity` | uint32[] | — | Pin worker threads to CPU cores |
| `queue_app_mode` | enum | `kCoScheduled` | `kCoScheduled` (drain on worker thread) or `kThreaded` (dedicated app thread per worker) |
| `app_cpu_affinity` | uint32[] | — | Pin app threads to CPU cores (when `kThreaded`) |
| `io_backend` | enum | `kEpoll` | `kEpoll` or `kIoUring` (Linux I/O backend) |
| `accept_unknown_sessions` | bool | false | Allow `SessionFactory` to handle unknown inbound Logons after static counterparties fail to match |
| `listeners` | list | — | TCP listener configurations (acceptor only) |
| `counterparties` | list | — | Pre-configured session counterparties |

### ListenerConfig

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `name` | string | — | Listener identifier (used in `OpenListeners()`) |
| `host` | string | `"0.0.0.0"` | Bind address |
| `port` | uint16 | 0 | Listen port |
| `worker_hint` | uint32 | 0 | Routing hint for worker pool |
| `tls_server` | TlsServerConfig | disabled | Optional TLS server policy for this listener |

### CounterpartyConfig

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `name` | string | — | Human-readable counterparty name |
| `session.session_id` | uint64 | — | Unique session identifier |
| `session.key` | SessionKey | — | `{begin_string, sender_comp_id, target_comp_id}` |
| `session.profile_id` | uint64 | — | Profile artifact ID to use |
| `session.heartbeat_interval_seconds` | uint32 | 30 | FIX heartbeat interval |
| `session.is_initiator` | bool | false | true = initiator, false = acceptor |
| `session.default_appl_ver_id` | string | — | Default ApplVerID for FIXT.1.1 sessions |
| `supported_app_msg_types` | string[] | empty | Optional inbound application MsgType allowlist; if a bound contract sidecar is present this set must stay within the contract receive subset |
| `contract_service_subsets` | string[] | empty | Optional deployment-selected Orchestra service subsets; valid only when a matching `.nfct` sidecar is loaded |
| `application_messages_available` | bool | true | When false, known application messages are answered with `BusinessMessageReject(380=4)` |
| `store_mode` | enum | `kMemory` | `kMemory`, `kMmap`, or `kDurableBatch` |
| `store_path` | path | — | Directory for mmap/durable store files |
| `recovery_mode` | enum | `kMemoryOnly` | `kMemoryOnly`, `kWarmRestart`, or `kColdStart` |
| `dispatch_mode` | enum | `kInline` | `kInline` (callback on worker thread) or `kQueueDecoupled` (SPSC queue) |
| `validation_policy` | policy | `Strict()` | `Strict()`, `Compatible()`, `Permissive()`, or `RawPassThrough()` |
| `reconnect_enabled` | bool | false | Enable initiator auto-reconnect |
| `reconnect_initial_ms` | uint32 | 1000 | Initial backoff delay (ms) |
| `reconnect_max_ms` | uint32 | 30000 | Maximum backoff delay (ms) |
| `reconnect_max_retries` | uint32 | 0 | Max reconnect attempts (0 = unlimited) |
| `durable_flush_threshold` | uint32 | 0 | Batch flush threshold for durable store |
| `durable_rollover_mode` | enum | `kUtcDay` | `kUtcDay`, `kLocalTime`, or `kExternal` |
| `durable_archive_limit` | uint32 | 0 | Max archived store segments (0 = unlimited) |
| `durable_local_utc_offset_seconds` | int32 | 0 | UTC offset for local-time rollover |
| `durable_use_system_timezone` | bool | true | Use system timezone for rollover |
| `reset_seq_num_on_logon` | bool | false | Reset sequence numbers on logon |
| `reset_seq_num_on_logout` | bool | false | Reset sequence numbers on logout |
| `reset_seq_num_on_disconnect` | bool | false | Reset sequence numbers on disconnect |
| `refresh_on_logon` | bool | false | Reload persisted recovery state before logon |
| `send_next_expected_msg_seq_num` | bool | false | Include tag 789 on Logon |
| `session_schedule` | struct | — | Session time window (start/end time/day, logon/logout windows) |
| `day_cut` | struct | — | Day-cut mode and timing for sequence reset |
| `tls_client` | TlsClientConfig | disabled | Optional TLS client policy for initiator connections |
| `acceptor_transport_security` | enum | `kAny` | Accept-side requirement: any, plain-only, or TLS-only |

### TLS Runtime Config

TLS is negotiated before FIX Logon. Acceptor TLS therefore belongs to `ListenerConfig`; initiator TLS belongs to the initiator `CounterpartyConfig`. TLS does not change `SessionKey`, sequence numbers, store keys, profile selection, or replay semantics.

```cpp
nimble::runtime::TlsServerConfig server_tls;
server_tls.enabled = true;
server_tls.certificate_chain_file = "/etc/nimblefix/server-chain.pem";
server_tls.private_key_file = "/etc/nimblefix/server-key.pem";
server_tls.ca_file = "/etc/nimblefix/client-ca.pem";
server_tls.verify_peer = true;
server_tls.require_client_certificate = true;

config.listeners.push_back(
    nimble::runtime::ListenerConfigBuilder::Named("tls-main")
        .bind("0.0.0.0", 9877)
        .tls_server(std::move(server_tls))
        .build());

nimble::runtime::TlsClientConfig client_tls;
client_tls.enabled = true;
client_tls.server_name = "fix.example.com";
client_tls.expected_peer_name = "fix.example.com";
client_tls.ca_file = "/etc/nimblefix/ca.pem";
client_tls.min_version = nimble::runtime::TlsProtocolVersion::kTls12;

config.counterparties.push_back(
    nimble::runtime::CounterpartyConfigBuilder::Initiator(
        "buy-side-tls", 1001U,
        nimble::session::SessionKey{ .sender_comp_id = "BUY1", .target_comp_id = "SELL1" },
        4400U)
        .tls_client(std::move(client_tls))
        .build());
```

For acceptors, use `acceptor_transport_security(kTlsOnly)` when a sensitive session must not bind on a plain listener. `verify_peer=false` is intended only for controlled tests; production deployments should keep peer verification enabled and provide CA files or paths.

### Tool Runtime Config (`.nfcfg`)

The built-in binaries (`nimblefix-acceptor`, `nimblefix-interop-runner`, and tests) also accept an internal `.nfcfg` format. It is a convenience layer over `EngineConfig`, not a stable public library API.

```text
engine.worker_count=1
engine.enable_metrics=true
engine.trace_mode=disabled
profile=build/sample-basic.nfa
listener|main|127.0.0.1|9921|0
counterparty|fix44-demo|4201|1001|FIX.4.4|SELL|BUY|memory||memory|inline|30|false
```

`profile=` may be repeated. `dictionary=` may also be repeated, and each `dictionary=` line accepts a comma-separated base-plus-overlay `.nfd` set that is loaded as one merged dictionary group.

For ready-to-run examples, see `tests/data/interop/loopback-runtime.nfcfg` and `tests/data/interop/runtime-multiversion.nfcfg`. `dispatch_mode` selects inline vs queue-decoupled delivery per counterparty. `queue_app_mode` is an engine-level knob and only matters when at least one counterparty uses `dispatch_mode=queue`. The full record order and advanced columns are documented in `docs/development.md`.

---

## Threading Model

NimbleFIX does not create threads behind your back. Every thread is explicitly configured, explicitly started, and pinnable to a specific CPU core.

### Thread Topology

**Acceptor:**

| Thread | Name | Count | Role | CPU Pin |
|--------|------|-------|------|---------|
| Front-door | `ff-acc-main` | 1 | Accept TCP connections, read Logon, route to least-loaded worker | `front_door_cpu` |
| Session worker | `ff-acc-w{N}` | `worker_count` | Own sessions exclusively: decode, sequence, timer, encode, send | `worker_cpu_affinity[N]` |
| App worker | `ff-app-w{N}` | `worker_count` | Drain SPSC queues, invoke business callbacks | `app_cpu_affinity[N]` |

**Initiator:** Same as acceptor but without the front-door thread. Session workers are named `ff-ini-w{N}`.

When `worker_count=1`, the single worker runs on the caller's thread — no extra thread is spawned. App workers only exist when `queue_app_mode = kThreaded`.

### Session Ownership

Each session belongs to exactly one worker thread. That worker alone handles all protocol state: decode, sequence numbers, timers, store persistence, encode, and write. **No locks on the hot path.**

Cross-thread session access still goes through `SessionHandle`, but the command path behind `SendCopy()` / `SendTake()` / `SendEncodedCopy()` / `SendEncodedTake()` is one SPSC queue per worker. `Snapshot()` and `Subscribe()` are safe query/stream APIs from any thread. Send paths are single-producer: the runtime binds each handle's send queue to the first producer thread that uses it, and later producers fast-fail with `kInvalidArgument` instead of silently corrupting the SPSC queue.

```cpp
// Safe query paths from any thread:
session_handle.Snapshot();
session_handle.Subscribe();

// Cross-thread send path:
// one producer thread per SessionHandle send queue.
session_handle.SendTake(std::move(msg));

// Owning-worker / inline-callback only.
// Outside inline callbacks this returns kInvalidArgument.
session_handle.SendInlineBorrowed(view);
message_view.get_string(11);
```

### Application Callback Modes

| Mode | Where your code runs | Constraint |
|------|---------------------|------------|
| `kInline` (default) | On session worker thread, inside the I/O loop | Must not block |
| `kQueueDecoupled` + `kCoScheduled` | On session worker thread, during explicit poll call | Must not block |
| `kQueueDecoupled` + `kThreaded` | On dedicated app worker thread (`ff-app-wN`) | May block (within reason) |

**Inline mode** gives the lowest latency — your `OnAppMessage()` callback receives a zero-copy `MessageView` directly from the decode buffer. But it must return quickly; any blocking will stall all sessions on that worker.

**Queue-decoupled / threaded mode** adds one SPSC queue hop but fully isolates I/O from business logic. Queue overflow policies: `kCloseSession`, `kBackpressure`, `kDropNewest`.

### Typical Low-Latency Deployment

```
Core 0:  Front-door (accept only)
Core 1:  Session worker 0  ← hot path, isolated
Core 2:  Session worker 1
Core 3:  Session worker 2
Core 4:  App worker 0      (if kThreaded)
Core 5:  App worker 1
Core 6:  App worker 2
```

```cpp
config.worker_count = 3;
config.front_door_cpu = 0;
config.worker_cpu_affinity = {1, 2, 3};
config.app_cpu_affinity = {4, 5, 6};
config.poll_mode = nimble::runtime::PollMode::kBusy;
config.queue_app_mode = nimble::runtime::QueueAppThreadingMode::kThreaded;
```

---

## Benchmark

### Why Compare with QuickFIX?

QuickFIX is still the reference implementation most teams already know and already run. NimbleFIX therefore keeps an in-tree, pinned QuickFIX side-by-side suite so the comparison stays on the same FIX44 business-order fixture, the same dictionary lineage, and the same command wrapper.

### Test Methodology

- Command: `./bench/bench.sh compare`
- Default compare args: `--iterations 100000 --loopback 1000 --replay 1000 --replay-span 128`
- Dictionary lineage: QuickFIX `bench/vendor/quickfix/spec/FIX44.xml` → `nimblefix-xml2nfd` → `build/bench/quickfix_FIX44.nfd` → `nimblefix-dictgen` → `build/bench/quickfix_FIX44.nfa`
- Business fixture: one neutral FIX44 `NewOrderSingle` with a single `NoPartyIDs=1` group entry
- Encode fairness: both engines pin `SendingTime` to the fixture timestamp so the encode tiers measure object-to-wire work, not per-iteration clock formatting
- Allocation tracking: global `operator new` interception counts heap allocations per iteration
- CPU counters: Linux `perf_event_open` feeds cache-miss and branch-miss columns when available

### Current Side-By-Side Snapshot (2026-04-14)

| | |
|---|---|
| **CPU** | AMD Ryzen 7 7840HS with Radeon 780M Graphics |
| **OS** | Linux 6.19.10-1-cachyos x86_64 |
| **Compiler** | `g++ (GCC) 15.2.1 20260209` |
| **Build helper** | `xmake v3.0.8+20260324` via `./bench/bench.sh compare` |

#### Cross-Engine Shared Boundaries

| Boundary | NimbleFIX metric | QuickFIX metric | NimbleFIX p50 | NimbleFIX p95 | QuickFIX p50 | QuickFIX p95 | NimbleFIX alloc/op | QuickFIX alloc/op |
|----------|----------------|-----------------|-------------|-------------|--------------|--------------|------------------|-------------------|
| object → wire (reused buffer) | `encode` | `quickfix-encode-buffer` | 371 ns | 401 ns | 1.24 us | 1.43 us | 0 | 29 |
| wire → object | `parse` | `quickfix-parse` | 511 ns | 521 ns | 1.29 us | 1.33 us | 0 | 20 |
| session inbound | `session-inbound` | `quickfix-session-inbound` | 1.65 us | 1.94 us | 2.38 us | 2.75 us | 0 | 18 |
| replay (`replay_span=128`) | `replay` | `quickfix-replay` | 15.66 us | 16.81 us | 231.20 us | 269.07 us | 0 | 4117 |
| TCP loopback round-trip | `loopback-roundtrip` | `quickfix-loopback` | 17.58 us | 20.75 us | 20.55 us | 24.68 us | 3 | 77 |

#### NimbleFIX-Only Tier

| Metric | p50 | p95 | p99 | alloc/op | ops/sec |
|--------|-----|-----|-----|----------|---------|
| `peek` | 130 ns | 141 ns | 141 ns | 0 | 6.58M |

Key observations:

- NimbleFIX currently leads every shared tier in the checked-in side-by-side run: about 3.3x on object-to-wire encode, 2.5x on parse, 1.4x on session-inbound, 14.8x on replay, and 1.2x on loopback RTT.
- The replay gap is the largest structural difference: NimbleFIX re-encodes from store state with 0 alloc/op, while QuickFIX replay allocates about 4,117 times per measured iteration.
- Loopback remains the closest tier because both engines are partly bounded by the same Linux TCP floor once the message leaves userspace.
- `quickfix-encode` (fresh string) is still printed by the benchmark, but `quickfix-encode-buffer` is the tighter apples-to-apples comparison with NimbleFIX's reused `EncodeBuffer` path.

### Run Benchmarks Yourself

```bash
./bench/bench.sh build
./bench/bench.sh nimblefix        # NimbleFIX main suite (artifact)
./bench/bench.sh nimblefix-nfd    # NimbleFIX suite (direct .nfd loading)
./bench/bench.sh quickfix       # QuickFIX comparison
./bench/bench.sh builder        # Object-to-wire compare only
./bench/bench.sh compare        # Full cross-engine comparison
```

Every benchmark command above intentionally uses the pinned QuickFIX 4.4 inputs: `bench/vendor/quickfix/spec/FIX44.xml`, `build/bench/quickfix_FIX44.nfd`, or `build/bench/quickfix_FIX44.nfa`.

For exact measurement boundaries, per-metric start/end points, and flow diagrams that mark where each metric sits in the pipeline, see [bench/README.md](bench/README.md).

---

## Further Documentation

- [docs/architecture.md](docs/architecture.md) — Internal architecture, module dependency graph, data flow diagrams, threading model details
- [docs/development.md](docs/development.md) — Development guide, testing, `.nfd` format specification, profiling, stress/fuzz testing, extending the engine
- [bench/README.md](bench/README.md) — Benchmark infrastructure, measurement boundaries, full result tables

## Project Status

NimbleFIX is under active development. The core engine, session management, codec, and persistence layers are implemented and tested (206 test cases, 2760+ assertions). The hot-path encode pipeline continues to be optimized toward the design targets.

## License

See [LICENSE](LICENSE) for details.
