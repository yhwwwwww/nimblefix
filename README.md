# FastFix

**A low-latency FIX protocol engine for high-frequency trading systems.**

[中文文档](README_CN.md)

---

## What is FastFix?

FastFix is a C++20 FIX (Financial Information eXchange) protocol engine built from scratch for latency-sensitive trading infrastructure. It handles the full FIX session lifecycle — connection management, Logon/Logout handshake, sequence number tracking, heartbeat monitoring, gap detection, resend recovery — while keeping the hot path allocation-free and lock-free.

FastFix operates as both **initiator** (client) and **acceptor** (server) using the same internal engine. It supports FIX 4.2, 4.3, 4.4, and FIXT.1.1 transport sessions.

## Why FastFix?

Existing open-source FIX engines (QuickFIX, QuickFIX/J, Fix8) are designed for correctness and broad compatibility, not for raw speed. They parse messages into dynamic maps, allocate on every message, and lock shared state across threads. This is fine for 99% of use cases — but not for firms where single-digit microsecond latency matters.

FastFix was designed to answer: *what if every design decision optimized for the hot path?*

- **Dictionary-backed codec**: FIX metadata is normalized once at startup, either from `.ffd` dictionaries or precompiled `.art` artifacts. The hot path uses the same in-memory dictionary representation either way.
- **Zero-copy message views**: Parsed messages are lightweight views over the original byte buffer. No copies until you explicitly request one.
- **Single-writer session model**: Each session is owned by exactly one worker thread. No locks, no contention, no false sharing on the critical path.
- **Pre-compiled frame templates**: For high-frequency message types, header/trailer fragments are pre-built. Only variable fields, BodyLength, and Checksum are filled per message.
- **SIMD-accelerated parsing**: SSE2 vectorized scanning for SOH delimiters and `=` separators in the tokenizer hot loop.

## Highlights

| Feature | Details |
|---------|---------|
| **Codec latency** | FIX44 benchmark: 180 ns header peek (p50), 992 ns full parse (p50), 461 ns typed writer encode (p50) |
| **Low allocation pressure** | Parse path: 0 alloc/op; encode path: 5 alloc/op |
| **Session management** | Full Logon/Logout/Heartbeat/TestRequest/ResendRequest/SequenceReset |
| **Repeating groups** | Nested groups fully supported via dictionary metadata |
| **Reconnect with backoff** | Configurable exponential backoff with jitter for initiator reconnect |
| **Dynamic session factory** | Acceptor can accept unknown CompIDs via callback or whitelist |
| **Pluggable persistence** | Memory, mmap, and durable batch stores with configurable rollover |
| **Worker sharding** | Per-worker event loops with CPU affinity pinning |
| **Busy-poll mode** | Optional zero-timeout polling for lowest possible latency |
| **SIMD tokenizer** | SSE2 byte scanning with automatic scalar fallback |
| **Observability** | Built-in metrics registry and ring-buffer trace recorder |
| **Soak testing** | Fault injection harness (gaps, duplicates, reorders, disconnects) |
| **Fuzz testing** | libFuzzer harnesses for codec, config, and dictionary inputs |

## How FastFix Differs from QuickFIX

[QuickFIX](http://www.quickfixengine.org/) is the de facto open-source FIX engine. It's mature, broadly used, and correct. FastFix differs in philosophy:

| Aspect | QuickFIX | FastFix |
|--------|----------|---------|
| **Parsing** | XML data dictionary loaded at runtime; fields stored in `std::map` | Normalized dictionary loaded from `.art` or parsed from `.ffd` at startup; hot path uses contiguous lookup sections |
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
- `xmake` (preferred, used automatically when available)
- or CMake 3.20+ plus Ninja (preferred CMake generator) or make (fallback when Ninja is unavailable)

### Offline Dependency Layout

FastFix now keeps the required third-party sources as pinned Git submodules for offline builds:

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
xmake f -m release -y
xmake build fastfix-tests
xmake build fastfix-bench

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
xmake f -m release -y
xmake build fastfix-tests
```

GitHub Actions CI uses the default xmake path on Ubuntu and still exercises these two named RHEL CMake presets via `ubi8/ubi:8.10` + `gcc-toolset-12` and `ubi9/ubi:9.7` + `gcc-toolset-14` container jobs on every push and pull request.

When `build/generated/`, `build/bench/`, or `build/sample-basic.art` have been removed, `xmake build fastfix-tests` and `xmake build fastfix-bench` now regenerate the required shared assets automatically.

The helper scripts auto-select `xmake`, then `cmake + Ninja`, then `cmake + make`. Default xmake builds write executables under `build/linux/x86_64/release/`. Ninja-based CMake presets write to `build/cmake/<preset>/bin/`, and make-based fallback presets write to `build/cmake/<preset>-make/bin/`. The helper scripts keep using the shared repository-local assets under `build/generated/` and `build/bench/` so tests and benchmarks resolve the same files regardless of build system.

### Run Tests

```bash
./build/linux/x86_64/release/fastfix-tests
# Alternative one-shot flow: bash ./scripts/offline_build.sh --bench skip
# Alternative CMake binary: ./build/cmake/dev-release/bin/fastfix-tests
# Make fallback binary: ./build/cmake/dev-release-make/bin/fastfix-tests
```

### Integration

FastFix compiles into a single static library `libfastfix.a`. To use it in your own program:

1. **Build the library**: `xmake f -m release -y && xmake build fastfix` or, if you need the alternative path, `cmake --build build/cmake/dev-release --target fastfix` (or `build/cmake/dev-release-make` when forcing the make fallback)
2. **Compile a protocol profile**: Run `fastfix-dictgen` to produce a `.art` binary artifact from your dictionary (optional — `.ffd` files can also be loaded directly at runtime)
3. **Link and include**: Point your compiler at `include/` for headers and link against `libfastfix.a`

**xmake (as a subdependency):**

```lua
-- xmake.lua
includes("path/to/fastfix")   -- path to the fastfix source tree

target("my-trading-app")
    set_kind("binary")
    add_deps("fastfix")        -- links libfastfix.a + adds include path
    add_files("src/*.cpp")
```

**CMake / Makefile / other (alternative):**

```bash
# 1. Build fastfix first
cd path/to/fastfix && cmake -S . -B build/cmake/dev-release -DCMAKE_BUILD_TYPE=Release && cmake --build build/cmake/dev-release --target fastfix

# 2. In your build system, add:
#    Include path:  path/to/fastfix/include
#    Link library:  path/to/fastfix/build/cmake/dev-release/lib/libfastfix.a
#    Standard:      C++20
```

---

## Key Concepts

### Profile

A **profile** is a compiled binary description of one FIX protocol variant — which fields exist, which messages are defined, what groups they contain. Each profile carries a `profile_id` (uint64) that you choose when writing the dictionary. Sessions reference profiles by this ID, and one engine can load multiple profiles simultaneously (e.g., FIX 4.2 and FIX 4.4).

### `.ffd` — FastFix Dictionary

A text file defining all fields, messages, and groups for one FIX protocol variant. Contains a `profile_id` you assign (any unique number, e.g. `1001` for FIX 4.4). Multiple `.ffd` files can be passed to `fastfix-dictgen` or `Engine` — the first provides the baseline, additional files serve as incremental overlays (adding fields, extending messages, overriding field rules).

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

### `.art` — Artifact

The compiled binary output of `fastfix-dictgen`. It's a flat, mmap-loadable file containing string tables, field/message/group definitions, validation rules, and lookup tables. The `profile_id` from your `.ffd` is embedded in it. Loading an `.art` file avoids text parsing at startup — useful for production deployments or when startup time matters.

### Overlay

Multiple `.ffd` files can be merged to extend a base profile with venue-specific custom fields. The first `.ffd` provides the baseline; additional files add fields, extend messages, or override field rules:

```
# venue_extensions.ffd — overlay with custom fields
field|5001|VenueOrderType|string|1
field|5002|VenueAccount|string|1
message|D|NewOrderSingle|0|5001:r,5002:o
```

### MessageBuilder vs FixedLayoutWriter

FastFix provides two ways to construct outbound messages:

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
| `fastfix-dictgen` | Compile `.ffd` dictionaries into binary `.art` profiles (supports merging multiple `.ffd` files and generating C++ builder headers) |
| `fastfix-xml2ffd` | Convert QuickFIX XML data dictionaries to `.ffd` format; also generates C++ builder headers from `.ffd` |
| `fastfix-initiator` | CLI FIX initiator for testing and interop |
| `fastfix-acceptor` | CLI FIX acceptor (echo server) |
| `fastfix-soak` | Stress test with fault injection (gaps, duplicates, reorders, disconnects) |
| `fastfix-bench` | Latency/throughput benchmark with allocation and CPU counter tracking |
| `fastfix-fuzz-codec` | libFuzzer harness for codec and admin protocol |
| `fastfix-fuzz-config` | libFuzzer harness for `.ffcfg` config parser |
| `fastfix-fuzz-dictgen` | libFuzzer harness for `.ffd` dictionary parser |
| `fastfix-interop-runner` | Bidirectional interoperability scenario runner |

### Compile a Profile

```bash
./build/linux/x86_64/release/fastfix-dictgen \
    --input samples/basic_profile.ffd \
    --merge samples/basic_overlay.ffd \
    --output build/sample-basic.art \
    --cpp-builders build/generated/sample_basic_builders.h
```

| Flag | Required | Description |
|------|----------|-------------|
| `--input` | yes | Base dictionary file (`.ffd`) |
| `--merge` | no | Additional `.ffd` file(s) to merge (repeatable) |
| `--output` | yes | Output artifact path (`.art`) |
| `--cpp-builders` | no | Generate C++ header with profile constants |

### Convert from QuickFIX XML

```bash
./build/linux/x86_64/release/fastfix-xml2ffd \
    --xml FIX44.xml \
    --output my_profile.ffd \
    --profile-id 1001 \
    --cpp-builders generated_builders.h
```

Components are inlined, groups are extracted, XML types are mapped to FastFix types, and `schema_hash` is auto-computed.

---

## Usage: Initiator

### Standard Initiator

```cpp
#include "fastfix/runtime/engine.h"
#include "fastfix/runtime/live_initiator.h"
#include "fastfix/runtime/config.h"
#include "fastfix/runtime/application.h"

class MyApp : public fastfix::runtime::ApplicationCallbacks {
    auto OnSessionEvent(const fastfix::runtime::RuntimeEvent& event)
        -> fastfix::base::Status override {
        if (event.session_event == fastfix::runtime::SessionEventKind::kActive) {
            // Session is live — start sending orders
            auto msg = fastfix::message::MessageBuilder{"D"}
                .set_string(11, "ORD-001")
                .set_char(54, '1')       // Side=Buy
                .set_float(44, 150.25)   // Price
                .set_int(38, 100)        // OrderQty
                .build();
            event.handle.Send(std::move(msg));
        }
        return fastfix::base::Status::Ok();
    }

    auto OnAppMessage(const fastfix::runtime::RuntimeEvent& event)
        -> fastfix::base::Status override {
        auto view = event.message_view();
        auto msg_type = view.msg_type();      // "8" for ExecutionReport
        auto exec_id = view.get_string(17);   // ExecID
        return fastfix::base::Status::Ok();
    }
};

int main() {
    fastfix::runtime::EngineConfig config;
    config.worker_count = 1;
    config.profile_artifacts = {"build/sample-basic.art"};
    // Or load .ffd directly:
    // config.profile_dictionaries = {{"samples/basic_profile.ffd"}};
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

    fastfix::runtime::Engine engine;
    engine.LoadProfiles(config);
    engine.Boot(config);

    auto app = std::make_shared<MyApp>();
    fastfix::runtime::LiveInitiator initiator(&engine, {
        .application = app,
    });

    initiator.OpenSession(1, "exchange.example.com", 9876);
    initiator.Run();
}
```

### Hot-Path Initiator with FixedLayoutWriter

For latency-critical order submission, use `FixedLayoutWriter` with a pre-built `FixedLayout`:

```cpp
#include "fastfix/message/fixed_layout_writer.h"
#include "fastfix/codec/fix_codec.h"

// Build once at startup:
auto layout = fastfix::message::FixedLayout::Build(dictionary, "D").value();
auto templates = fastfix::codec::PrecompiledTemplateTable{};

// ... inside OnSessionEvent when session becomes active:

class HotPathApp : public fastfix::runtime::ApplicationCallbacks {
    fastfix::message::FixedLayout layout_;
    fastfix::codec::EncodeBuffer encode_buffer_;   // reuse across calls
    fastfix::codec::EncodeOptions encode_options_;

    auto OnSessionEvent(const fastfix::runtime::RuntimeEvent& event)
        -> fastfix::base::Status override {
        if (event.session_event == fastfix::runtime::SessionEventKind::kActive) {
            SendOrder(event.handle);
        }
        return fastfix::base::Status::Ok();
    }

    void SendOrder(fastfix::session::SessionHandle handle) {
        fastfix::message::FixedLayoutWriter writer(layout_);
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

The `FixedLayoutWriter` path benchmarks at **~684 ns** per encode (p50 = 651 ns), roughly **46% faster** than the generic `MessageBuilder` path on the same message shape.

### Hybrid Path — FixedLayout + Extra Fields

When most fields are known at compile time but some are dynamic (e.g., venue-specific extensions):

```cpp
fastfix::message::FixedLayoutWriter writer(layout_);
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
#include "fastfix/runtime/engine.h"
#include "fastfix/runtime/live_acceptor.h"
#include "fastfix/runtime/config.h"

fastfix::runtime::EngineConfig config;
config.worker_count = 2;
config.profile_artifacts = {"build/sample-basic.art"};
config.listeners = {{
    .name = "main",
    .host = "0.0.0.0",
    .port = 9876,
}};
config.accept_unknown_sessions = true;

fastfix::runtime::Engine engine;
engine.LoadProfiles(config);
engine.Boot(config);

// Accept sessions dynamically based on inbound Logon:
engine.SetSessionFactory([](const fastfix::session::SessionKey& key)
    -> fastfix::base::Result<fastfix::runtime::CounterpartyConfig> {
    return fastfix::runtime::CounterpartyConfig{
        .name = key.sender_comp_id,
        .session = {
            .profile_id = 1001,
            .key = key,
            .heartbeat_interval_seconds = 30,
        },
    };
});

auto app = std::make_shared<MyApp>();
fastfix::runtime::LiveAcceptor acceptor(&engine, {
    .application = app,
});
acceptor.OpenListeners("main");
acceptor.Run();
```

### Acceptor with Pre-Configured Counterparties

```cpp
fastfix::runtime::EngineConfig config;
config.worker_count = 1;
config.profile_artifacts = {"build/sample-basic.art"};
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
    .store_mode = fastfix::runtime::StoreMode::kDurableBatch,
    .store_path = "/var/lib/fastfix/client-a",
}};

fastfix::runtime::Engine engine;
engine.LoadProfiles(config);
engine.Boot(config);

auto app = std::make_shared<MyApp>();
fastfix::runtime::LiveAcceptor acceptor(&engine, {
    .application = app,
});
acceptor.OpenListeners("main");
acceptor.Run();
```

### Hot-Path Acceptor: Reading Inbound Messages

In the `OnAppMessage` callback, use zero-copy `MessageView` for minimal overhead:

```cpp
auto OnAppMessage(const fastfix::runtime::RuntimeEvent& event)
    -> fastfix::base::Status override {
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
    auto response = fastfix::message::MessageBuilder{"8"}
        .set_string(17, "EXEC-001")        // ExecID
        .set_string(11, cl_ord_id.value_or(""))
        .set_char(150, '0')                // ExecType=New
        .set_int(14, 0)                    // CumQty
        .build();
    event.handle.Send(std::move(response));

    return fastfix::base::Status::Ok();
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
| `profile_artifacts` | paths | — | Paths to compiled `.art` profile files |
| `profile_dictionaries` | path[][] | — | Paths to `.ffd` dictionary file groups to load directly at runtime (each inner vector is merged as base + overlays) |
| `profile_madvise` | bool | false | Call `madvise(MADV_WILLNEED)` on loaded artifacts |
| `profile_mlock` | bool | false | Call `mlock()` on loaded artifacts (requires sufficient RLIMIT) |
| `front_door_cpu` | uint32? | — | Pin acceptor front-door thread to CPU core |
| `worker_cpu_affinity` | uint32[] | — | Pin worker threads to CPU cores |
| `queue_app_mode` | enum | `kCoScheduled` | `kCoScheduled` (drain on worker thread) or `kThreaded` (dedicated app thread per worker) |
| `app_cpu_affinity` | uint32[] | — | Pin app threads to CPU cores (when `kThreaded`) |
| `accept_unknown_sessions` | bool | false | Allow dynamic session factory for unknown inbound CompIDs |
| `listeners` | list | — | TCP listener configurations (acceptor only) |
| `counterparties` | list | — | Pre-configured session counterparties |

### ListenerConfig

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `name` | string | — | Listener identifier (used in `OpenListeners()`) |
| `host` | string | `"0.0.0.0"` | Bind address |
| `port` | uint16 | 0 | Listen port |
| `worker_hint` | uint32 | 0 | Routing hint for worker pool |

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

---

## Threading Model

FastFix does not create threads behind your back. Every thread is explicitly configured, explicitly started, and pinnable to a specific CPU core.

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

Cross-thread session access goes through `SessionHandle`:

```cpp
// From any thread — thread-safe:
session_handle.Send(msg);           // Enqueue message → wakeup target worker
session_handle.Snapshot();          // Query-only
session_handle.Subscribe();         // Event subscription

// NOT thread-safe — only from owning worker / callback scope:
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
config.poll_mode = fastfix::runtime::PollMode::kBusy;
config.queue_app_mode = fastfix::runtime::QueueAppThreadingMode::kThreaded;
```

---

## Benchmark

### Why Compare with QuickFIX?

QuickFIX is the most widely deployed open-source FIX engine, making it the natural baseline. Comparing against QuickFIX shows where the latency differences actually come from — parsing strategy, memory allocation patterns, and threading model.

### Test Methodology

- **QuickFIX source**: `quickfix/quickfix` GitHub repository, commit `00dd20837c97578e725072e5514c8ffaa0e141d4`
- **Dictionary**: QuickFIX `spec/FIX44.xml`; FastFix side uses the same XML converted through `fastfix-xml2ffd` + `fastfix-dictgen`
- **Message**: Valid FIX.4.4 `NewOrderSingle` with `ClOrdID`, `Symbol`, `Side`, `TransactTime`, `OrderQty`, `OrdType`, `Price`, and one `NoPartyIDs=1` entry
- **Allocation tracking**: Global `operator new` interception counts every heap allocation
- **CPU counters**: Linux `perf_event_open` for cache misses and branch misses
- **Percentiles**: p50, p95, p99, p999 computed from per-iteration `steady_clock` samples

### Benchmark Tiers

| Tier | What it measures |
|------|-----------------|
| **encode** | Business object → generated typed writer → wire bytes (reused buffer) |
| **peek** | Extract session header (MsgType, CompIDs) without full decode |
| **parse** | Full message decode into zero-copy `MessageView` |
| **session-inbound** | Full inbound path: decode → sequence validation → store → admin protocol |
| **replay** | ResendRequest processing: retrieve + re-encode 128 stored messages |
| **loopback-roundtrip** | Full TCP round-trip: send `NewOrderSingle`, wait for `ExecutionReport` ack |

### Results

**Test environment:**

| | |
|---|---|
| **CPU** | AMD Ryzen 7 7840HS (8C/16T, boost to 5.1 GHz) |
| **RAM** | 28 GB DDR5 |
| **OS** | Linux 6.19.6 CachyOS (x86_64) |
| **Compiler** | GCC 15.2.1 20260209 |
| **Build** | Release (`-O2`) |
| **Iterations** | 100,000 codec iterations; 1,000 replay/loopback iterations |

#### Cross-Engine Comparable Tiers

| Boundary | FastFix metric | QuickFIX metric | FastFix p50 | FastFix p95 | QuickFIX p50 | QuickFIX p95 | FF alloc/op | QF alloc/op |
|----------|----------------|-----------------|-------------|-------------|--------------|--------------|-------------|-------------|
| encode (object → wire) | `encode` | `quickfix-encode-buffer` | 0.46 µs | 0.49 µs | 1.25 µs | 1.43 µs | 0 | 29 |
| parse (wire → object) | `parse` | `quickfix-parse` | 0.99 µs | 1.02 µs | 1.25 µs | 1.30 µs | 0 | 20 |
| session-inbound | `session-inbound` | `quickfix-session-inbound` | 2.58 µs | 2.71 µs | 2.32 µs | 2.44 µs | 0 | 18 |
| replay (128 msgs) | `replay` | `quickfix-replay` | 361 µs | 367 µs | 233 µs | 240 µs | 0 | 4,117 |
| loopback RTT | `loopback-roundtrip` | `quickfix-loopback` | 20.01 µs | 26.60 µs | 20.30 µs | 24.68 µs | 3 | 77 |

Key observations:
- **FastFix encode is ~2.7× faster than QuickFIX** — the generated typed writer with `FixedLayoutWriter` backend eliminates dynamic dispatch and most allocations.
- **QuickFIX leads the pure parse path** by a modest but consistent margin — FastFix's dictionary-driven decode carries more per-field work than QuickFIX's simpler map-insert approach.
- **Session-inbound is near parity** (≈10% difference), showing the overhead beyond codec is broadly similar between both engines.
- **Loopback RTT is essentially identical** — both engines are dominated by the ~20 µs Linux TCP kernel floor.
- **FastFix allocation pressure is materially lower**: encode is 0/op vs 29/op; parse is 0/op vs 20/op; loopback is 3/op vs 77/op.
- **QuickFIX replay is faster** (233 µs vs 361 µs p50) but at extreme allocation cost — 4,117 alloc/op vs 0/op for FastFix, because QuickFIX allocates fresh string copies per resent message rather than re-encoding from the store.

#### FastFix-Only Tiers

| Tier | p50 | p95 | p99 | Allocs/op | Ops/sec |
|------|-----|-----|-----|-----------|--------|
| peek | 180 ns | 190 ns | 191 ns | 0 | 4,950,000 |

### Run Benchmarks Yourself

```bash
./bench/bench.sh build
./bench/bench.sh fastfix        # FastFix main suite (artifact)
./bench/bench.sh fastfix-ffd    # FastFix suite (direct .ffd loading)
./bench/bench.sh quickfix       # QuickFIX comparison
./bench/bench.sh builder        # Object-to-wire compare only
./bench/bench.sh compare        # Full cross-engine comparison
```

Every benchmark command above intentionally uses the pinned QuickFIX 4.4 inputs: `bench/vendor/quickfix/spec/FIX44.xml`, `build/bench/quickfix_FIX44.ffd`, or `build/bench/quickfix_FIX44.art`.

For detailed benchmark methodology, metric definitions, and full result tables, see [bench/README.md](bench/README.md).

---

## Further Documentation

- [docs/architecture.md](docs/architecture.md) — Internal architecture, module dependency graph, data flow diagrams, threading model details
- [docs/development.md](docs/development.md) — Development guide, testing, `.ffd` format specification, profiling, stress/fuzz testing, extending the engine
- [bench/README.md](bench/README.md) — Benchmark infrastructure, measurement boundaries, full result tables

## Project Status

FastFix is under active development. The core engine, session management, codec, and persistence layers are implemented and tested (152 test cases, 2619 assertions). The hot-path encode pipeline continues to be optimized toward the design targets.

## License

See [LICENSE](LICENSE) for details.
