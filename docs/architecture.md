# FastFix Architecture

This document describes the internal architecture of FastFix: how code is organized, how data flows through the system, and how the major components interact.

---

## Module Map

```
fastfix/
├── base/          Low-level utilities shared by all modules
├── codec/         FIX message parsing and encoding
├── message/       Message data model (builder, view, typed view)
├── profile/       Protocol profile loading, dictionary, artifact format
├── session/       Session state machine, admin protocol, recovery
├── store/         Message persistence (memory, mmap, durable batch)
├── transport/     TCP socket I/O
└── runtime/       Engine orchestration, workers, pollers, metrics, trace
```

### Dependency Graph

```
runtime ──► session ──► codec ──► message ──► profile
   │            │                      │
   │            └──────► store         └──► base
   │
   └──► transport ──► base
   └──► store
   └──► profile
```

Modules depend downward only. `base` has no FIX-specific dependencies. `transport` and `store` are independent of each other and of higher-level FIX semantics.

---

## Layer-by-Layer

### 1. Base (`include/fastfix/base/`)

Foundation types used everywhere:

| Type | Purpose |
|------|---------|
| `Status` | Error-or-OK result with message string |
| `Result<T>` | Either a value or a `Status` error |
| `SpscQueue<T>` | Lock-free single-producer-single-consumer queue with fixed capacity |
| `InlineSplitVector<T, N>` | Inline storage for ≤N elements, overflows to heap vector |
| `TimerWheel` | Hierarchical timer scheduler for heartbeat/timeout management |

`Status` error codes:

```
kOk, kInvalidArgument, kIoError, kBusy, kFormatError,
kVersionMismatch, kNotFound, kAlreadyExists
```

### 2. Profile (`include/fastfix/profile/`)

Protocol metadata system. FastFix keeps XML out of the hot path, but protocol metadata can arrive either as precompiled `.art` artifacts or as `.ffd` dictionaries parsed once at startup. Both paths normalize into the same runtime dictionary layout before workers start.

**Pipeline:**

```
.ffd (dictionary text)  ──►  dictgen  ──►  .art (binary artifact)  [optional precompilation]
                        or
.ffd (dictionary text)  ──►  Engine parses once at startup
                                         │
                                         ├── StringTable
                                         ├── FieldDefs (tag → type, name, flags)
                                         ├── MessageDefs (MsgType → name, flags, field rules)
                                         ├── GroupDefs (count_tag → delimiter, member fields)
                                         ├── AdminRules
                                         ├── ValidationRules
                                         └── LookupTables
```

**Key types:**

| Type | Role |
|------|------|
| `LoadedProfile` | mmap'd view of a `.art` binary artifact |
| `NormalizedDictionaryView` | Read-only accessor over profile sections — field/message/group lookups |
| `ProfileRegistry` | Holds all loaded profiles for the engine |
| `ProfileLoader` | Loads `.art` files with optional `madvise`/`mlock` page warming; also loads `.ffd` dictionaries directly into memory |

**Artifact sections** (`SectionKind` enum):

```
kStringTable, kFieldDefs, kMessageDefs, kGroupDefs,
kAdminRules, kValidationRules, kLookupTables,
kTemplateDescriptors, kMessageFieldRules, kGroupFieldRules
```

**Dictionary input format** (`.ffd`):

```
profile_id=1001
field|35|MsgType|string|0
message|D|NewOrderSingle|0|35:r,49:r,56:r,453:o
group|453|448|Parties|0|448:r,447:r,452:r
```

**Overlay format**: Multiple `.ffd` files can be merged. The first provides the baseline, additional files extend or override fields/messages/groups.

### 3. Message (`include/fastfix/message/`)

Two representations of a FIX message:

**`Message`** — Owned, heap-allocated, writable:

```cpp
auto msg = MessageBuilder{"D"}
    .set_string(11, "ORD-001")
    .set_int(38, 100)
    .add_group_entry(453)
        .set_string(448, "PARTY-A")
        .set_char(447, 'D')
        .set_int(452, 3);
auto message = std::move(builder).build();
```

**`MessageView`** — Zero-copy, read-only, references original buffer:

```cpp
auto field = view.get_string(11);       // → optional<string_view>
auto qty = view.get_int(38);            // → optional<int64_t>
auto group = view.group(453);           // → optional<GroupView>
auto entry = group->entry(0);           // → GroupEntryView
auto party_id = entry.get_string(448);  // → optional<string_view>
```

**`TypedMessageView`** — MessageView bound to a dictionary for validation:

```cpp
auto typed = TypedMessageView::Bind(dictionary, view);
typed.validate_required_fields(&missing_tag);  // Check required fields present
```

**`FixedLayoutWriter`** — Hot-path encoder with pre-computed O(1) slot mapping:

```cpp
// Build layout once at startup
auto layout = FixedLayout::Build(dictionary, "D").value();

// Per-message (hot path):
FixedLayoutWriter writer(layout);
writer.set_string(11, "ORD-001");       // O(1) slot write
writer.set_int(38, 100);
writer.add_group_entry(453)
    .set_string(448, "PARTY-A");
writer.encode_to_buffer(dictionary, options, &buffer);
```

`FixedLayout::Build()` pre-computes tag → slot index mappings. `FixedLayoutWriter` writes directly to pre-allocated slots at known offsets — no hash map lookups, no field-list scanning. The hybrid path (`set_extra_string()` etc.) appends extension fields outside the fixed layout for venue-specific custom tags.

**Internal storage**: Fields use a `FieldSlot` linked-list structure within a flat buffer. Groups track entry boundaries via a small frame stack matched against dictionary group definitions.

### 4. Codec (`include/fastfix/codec/`)

Parsing and encoding FIX byte streams.

**Parsing pipeline:**

```
Raw bytes (span<const byte>)
    │
    ├── PeekSessionHeaderView()     ~198 ns, zero alloc
    │   └── Extracts: MsgType, SenderCompID, TargetCompID,
    │       MsgSeqNum, BeginString without full decode
    │
    └── DecodeFixMessageView()      ~1314 ns
        ├── SIMD SOH scanning (simd_scan.h)
        ├── Field tokenization (tag=value\x01 splitting)
        ├── Header validation (8/9/35 ordering, BodyLength, Checksum)
        ├── Group nesting (dictionary-driven stack)
        └── Returns DecodedMessageView { MessageView, ValidationIssues }
```

**Encoding pipeline:**

```
Message + EncodeOptions
    │
    ├── EncodeFixMessage()          ~1471 ns, allocates output buffer
    │
    └── EncodeFixMessageToBuffer()  ~1490 ns, reuses EncodeBuffer (reduced alloc)
    │
    └── FixedLayoutWriter::encode_to_buffer()  ~684 ns, hot-path O(1) slot writes
        ├── Write header: 8=BeginString|9=<placeholder>|35=MsgType|...
        ├── Write body fields from Message
        ├── Backfill BodyLength
        └── Append 10=<checksum>
```

**FrameEncodeTemplate**: For high-frequency messages, pre-builds the fixed portion of the frame. Only variable fields + length + checksum are computed per message.

**SIMD scanning** (`simd_scan.h`):

```cpp
// SSE2 path: 16-byte parallel comparison
auto* p = FindByte(data, len, std::byte{0x01});  // SOH
auto* q = FindByte(data, len, std::byte{'='});    // Field separator

// Automatic fallback to memchr on non-SSE2 platforms
```

**Validation issues** reported during decode:

```
kUnknownField, kFieldNotAllowed, kDuplicateField,
kFieldOutOfOrder, kIncorrectNumInGroupCount
```

### 5. Session (`include/fastfix/session/`)

FIX session state machine and admin message protocol.

#### SessionCore — State Machine

```
                    ┌────────────────────────────────┐
                    ▼                                │
             kDisconnected                           │
                    │                                │
                    │ OnTransportConnected()          │
                    ▼                                │
              kConnected                             │
                    │                                │
          ┌────────┤                                 │
          │        │ BeginLogon()                     │
          │        ▼                                 │
          │  kPendingLogon ──timeout──► kDisconnected │
          │        │                                 │
          │        │ OnLogonAccepted()               │
          │        ▼                                 │
          │     kActive ◄──────────────────┐         │
          │        │                       │         │
          │   ┌────┤                       │         │
          │   │    │ gap detected          │         │
          │   │    ▼                       │         │
          │   │ kResendProcessing          │         │
          │   │    │                       │         │
          │   │    │ CompleteResend()       │         │
          │   │    └───────────────────────┘         │
          │   │                                      │
          │   │ BeginLogout()                         │
          │   ▼                                      │
          │ kAwaitingLogout ──timeout──► kDisconnected│
          │   │                                      │
          │   │ Logout received                      │
          │   └──────────────────────────────────────┘
          │
          │ OnTransportConnected() (acceptor: skip PendingLogon)
          └─► kActive (direct bind)
```

**Key operations:**

```cpp
session.AllocateOutboundSeq();           // → next_out_seq++ (atomic, no contention)
session.ObserveInboundSeq(seq_num);      // → validate, detect gap
session.BeginResend(begin, end);         // → enter kResendProcessing
session.CompleteResend();                // → return to kActive
session.Snapshot();                      // → {next_in_seq, next_out_seq, state}
```

#### AdminProtocol — FIX Admin Messages

Wraps `SessionCore` and handles the full admin message set:

| MsgType | Name | Direction | Handler |
|---------|------|-----------|---------|
| A | Logon | Both | Handshake initiation/acceptance |
| 5 | Logout | Both | Graceful disconnect |
| 0 | Heartbeat | Both | Keep-alive response |
| 1 | TestRequest | Both | Liveness probe |
| 2 | ResendRequest | Both | Gap recovery request |
| 4 | SequenceReset | Both | Sequence gap fill or hard reset |
| 3 | Reject | Inbound | Session-level rejection |

**`ProtocolEvent`** — returned from `OnInbound()`:

```cpp
struct ProtocolEvent {
    std::vector<ProtocolFrame> outbound_frames;    // Frames to send
    std::vector<MessageRef> application_messages;   // App messages to dispatch
    bool session_active;                            // Session became active?
    bool disconnect;                                // Should close transport?
    bool poss_resend;                               // PossResend(97) flag set?
};
```

#### Resend Recovery

When a sequence gap is detected:

```
Inbound seq 45, expected 42 → gap [42, 44]
    │
    ├── AdminProtocol sends ResendRequest(BeginSeqNo=42, EndSeqNo=44)
    │
    ├── Remote sends:
    │   ├── SequenceReset-GapFill (admin msgs in gap)
    │   └── Original app messages with PossDupFlag=Y
    │
    └── SessionCore tracks progress, CompleteResend() when gap filled
```

### 6. Store (`include/fastfix/store/`)

Persistence layer for message recovery and sequence state.

```
SessionStore (interface)
    ├── MemorySessionStore      RAM only, no persistence
    ├── MmapSessionStore        mmap append-only file
    └── DurableBatchStore       Batch-flush with rollover and archival
```

**Interface:**

```cpp
class SessionStore {
    virtual auto SaveOutbound(const MessageRecord& record) -> Status = 0;
    virtual auto SaveInbound(const MessageRecord& record) -> Status = 0;
    virtual auto LoadOutboundRange(uint64_t session_id, uint32_t begin, uint32_t end)
        -> Result<vector<MessageRecord>> = 0;
    virtual auto SaveRecoveryState(const SessionRecoveryState& state) -> Status = 0;
    virtual auto LoadRecoveryState(uint64_t session_id)
        -> Result<SessionRecoveryState> = 0;
    virtual auto Flush() -> Status;
    virtual auto Rollover() -> Status;
};
```

**DurableBatchStore internals:**

```
store_root/
    ├── active.log           Current segment (append-only)
    ├── active.out.idx       Outbound sequence index
    ├── recovery.log         Session recovery state
    └── archive/
        ├── segment-1.log    Archived segment
        ├── segment-1.out.idx
        ├── segment-2.log
        └── segment-2.out.idx
```

**Rollover modes:**

| Mode | Trigger |
|------|---------|
| `kUtcDay` | Automatic at UTC midnight |
| `kLocalTime` | Automatic at local midnight (configurable UTC offset) |
| `kExternal` | Manual `Rollover()` call |

### 7. Transport (`include/fastfix/transport/`)

TCP socket I/O with frame boundary detection.

```cpp
class TcpConnection {
    static auto Connect(host, port, timeout) -> Result<TcpConnection>;
    auto Send(bytes, timeout) -> Status;
    auto ReceiveFrameView(timeout) -> Result<span<const byte>>;
    auto Close() -> void;
};

class TcpAcceptor {
    static auto Listen(host, port, backlog) -> Result<TcpAcceptor>;
    auto TryAccept() -> Result<optional<TcpConnection>>;
    auto Accept(timeout) -> Result<TcpConnection>;
};
```

**Frame detection**: The receiver scans for `8=` (BeginString), reads BodyLength, consumes the body, and verifies the checksum before returning a complete frame.

### 8. Runtime (`include/fastfix/runtime/`)

Top-level orchestration: engine lifecycle, worker threading, I/O polling, metrics, and tracing.

#### Engine

Central coordinator:

```cpp
class Engine {
    auto LoadProfiles(config) -> Status;     // Load .art files
    auto Boot(config) -> Status;             // Initialize runtime, workers
    auto ResolveInboundSession(header) -> Result<ResolvedCounterparty>;
    void SetSessionFactory(SessionFactory);  // Dynamic session acceptance
};
```

#### LiveInitiator / LiveAcceptor

Event-driven endpoint drivers:

```
LiveAcceptor                          LiveInitiator
    │                                     │
    ├── OpenListeners("main")             ├── OpenSession(id, host, port)
    │   └── TcpAcceptor::Listen()         │   └── TcpConnection::Connect()
    │                                     │
    └── Run()                             └── Run()
        ├── poll() on listener FDs            ├── poll() on connection FDs
        ├── AcceptReadyListener()             ├── ProcessConnection()
        │   └── Accept → ConnectionState      │   ├── ReceiveFrameView()
        ├── ProcessConnection()               │   ├── DecodeFixMessageView()
        │   ├── Peek header                   │   ├── AdminProtocol::OnInbound()
        │   ├── BindConnectionFromLogon()     │   └── DispatchAppMessage()
        │   │   └── ResolveInboundSession()   │
        │   ├── AdminProtocol::OnInbound()    ├── ProcessPendingReconnects()
        │   └── DispatchAppMessage()          │   └── Exponential backoff + jitter
        │                                     │
        └── TimerWheel::PopExpired()          └── TimerWheel::PopExpired()
            └── Heartbeat/timeout                 └── Heartbeat/timeout
```

#### Worker Sharding

```
Engine
  ├── Worker[0] (std::jthread)
  │   ├── ShardPoller (poll/epoll wrapper)
  │   ├── TimerWheel
  │   ├── ConnectionState[] (owned sessions)
  │   ├── SpscQueue<PendingConnection> (inbox from front-door)
  │   └── SpscQueue<OutboundCommand> (inbox from app thread)
  │
  ├── Worker[1]
  │   └── ... (same structure, independent sessions)
  │
  └── Front-door thread (acceptor only)
      └── Accepts connections, routes to least-loaded worker
```

**Session-to-worker routing**: `FindSessionShard(session_id)` determines which worker owns a session. If a connection arrives on the wrong worker (common with front-door accept), `MigrateConnectionToRoutedWorker()` transfers it.

#### Application Dispatch

Two modes:

**Inline** (`kInline`): Callback runs directly on the session worker thread.

```
Worker thread: ReceiveFrame → Decode → AdminProtocol → OnAppMessage()
```

Lowest latency. Application must not block.

**Queue-Decoupled** (`kQueueDecoupled`): Event is enqueued to a SPSC queue. Application drains from a separate thread.

```
Worker thread: ReceiveFrame → Decode → AdminProtocol → SpscQueue.push(event)
App thread:    SpscQueue.pop() → OnAppMessage() → handle.Send()
```

Higher latency, but decouples application processing from I/O.

#### Observability

**MetricsRegistry**: Counters and gauges for connections, messages, errors, sequence gaps.

**TraceRecorder**: Ring-buffer of timestamped trace events (session state transitions, protocol events, timer firings). Enabled via `trace_mode = kRing`.

---

## Data Flow: Complete Inbound Path

```
                                    ┌──────────────┐
TCP socket readable ──► poll() ──►  │ Worker Thread │
                                    └──────┬───────┘
                                           │
                                    TcpConnection::ReceiveFrameView()
                                           │
                                    ┌──────▼───────┐
                                    │  SIMD Scan   │ Find SOH boundaries
                                    │  Tokenize    │ Split tag=value pairs
                                    │  Checksum    │ Verify tag 10
                                    └──────┬───────┘
                                           │
                                    PeekSessionHeaderView()
                                           │ (if new connection: BindConnectionFromLogon)
                                           │
                                    DecodeFixMessageView()
                                           │
                                    ┌──────▼───────┐
                                    │ AdminProtocol│
                                    │  OnInbound() │
                                    └──┬────┬──────┘
                                       │    │
                              Admin msg │    │ App msg
                                       │    │
                              ┌────────▼┐  ┌▼─────────────┐
                              │Heartbeat│  │DispatchApp    │
                              │Resend   │  │  Message()    │
                              │SeqReset │  │               │
                              │Logout   │  │ → Inline CB   │
                              └─────────┘  │ → SPSC Queue  │
                                           └───────────────┘
```

## Data Flow: Complete Outbound Path

```
Application code
    │
    ├── MessageBuilder::build()
    │
    ├── SessionHandle::Send(message)
    │   │
    │   ├── SessionCore::AllocateOutboundSeq()
    │   │
    │   ├── AdminProtocol::SendApplication()
    │   │   ├── EncodeFixMessage() or FrameEncodeTemplate
    │   │   ├── SessionStore::SaveOutbound()
    │   │   └── Returns ProtocolFrame (encoded bytes)
    │   │
    │   └── TcpConnection::Send(frame.bytes)
    │
    └── Done (frame on wire)
```

---

## Threading Model

FastFix does not create threads behind your back. Every thread is explicitly configured, explicitly started, and pinnable to a specific CPU core. This section describes what threads exist, who owns them, and how the user controls them.

### Thread Topology

#### Acceptor

```
┌─────────────────────────────────────────────────────────────────┐
│ Front-door thread (ff-acc-main)          CPU: front_door_cpu    │
│   accept() → read Logon → route to least-loaded worker          │
└──────────────────────────┬──────────────────────────────────────┘
                           │ inbox (mutex-protected handoff)
          ┌────────────────┼────────────────┐
          ▼                ▼                ▼
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ Worker 0     │  │ Worker 1     │  │ Worker 2     │
│ ff-acc-w0    │  │ ff-acc-w1    │  │ ff-acc-w2    │
│ CPU: affn[0] │  │ CPU: affn[1] │  │ CPU: affn[2] │
│              │  │              │  │              │
│ ShardPoller  │  │ ShardPoller  │  │ ShardPoller  │
│ TimerWheel   │  │ TimerWheel   │  │ TimerWheel   │
│ Sessions[]   │  │ Sessions[]   │  │ Sessions[]   │
└──────┬───────┘  └──────┬───────┘  └──────┬───────┘
       │ SPSC              │ SPSC            │ SPSC
       ▼                   ▼                 ▼
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ App Worker 0 │  │ App Worker 1 │  │ App Worker 2 │  (optional, kThreaded only)
│ ff-app-w0    │  │ ff-app-w1    │  │ ff-app-w2    │
│ CPU: app[0]  │  │ CPU: app[1]  │  │ CPU: app[2]  │
└──────────────┘  └──────────────┘  └──────────────┘
```

| Thread | Name | Count | Role | CPU Pin |
|--------|------|-------|------|---------|
| Front-door | `ff-acc-main` | 1 | Accept TCP connections, read Logon, route to worker | `front_door_cpu` |
| Session worker | `ff-acc-w{N}` | `worker_count` | Own sessions exclusively: decode, sequence, timer, encode, send | `worker_cpu_affinity[N]` |
| App worker | `ff-app-w{N}` | `worker_count` (if `kThreaded`) | Drain SPSC queues, invoke business callbacks | `app_cpu_affinity[N]` |

#### Initiator

Same structure, minus the front-door thread:

| Thread | Name | Count | Role | CPU Pin |
|--------|------|-------|------|---------|
| Session worker | `ff-ini-w{N}` | `worker_count` | Own sessions: connect, decode, sequence, timer, encode, send, reconnect | `worker_cpu_affinity[N]` |
| App worker | `ff-app-w{N}` | `worker_count` (if `kThreaded`) | Drain SPSC queues, invoke business callbacks | `app_cpu_affinity[N]` |

When `worker_count=1`, the single worker runs on the caller's thread (no `std::jthread` spawned).

### Thread Lifecycle

```
Engine::Boot()
    │
    ├── LoadProfiles()           (cold, caller's thread)
    │
LiveAcceptor::OpenListeners()    (cold, caller's thread)
    │
    ├── ResetWorkerShards(N)     Create N ShardPoller + TimerWheel + inbox (no threads yet)
    │
LiveAcceptor::Run()
    │
    ├── if N > 1: StartWorkerThreads()
    │   └── spawn N std::jthread, each runs WorkerLoop(worker_id)
    │       ├── SetCurrentThreadName("ff-acc-wN")
    │       └── ApplyCurrentThreadAffinity(worker_cpu_affinity[N])
    │
    └── Front-door loop on caller's thread
        ├── accept() + read Logon
        ├── SelectAcceptWorkerId() → least-loaded worker
        ├── EnqueuePendingConnection(worker_id, conn)  (lock inbox, push)
        └── SignalWorkerWakeup(worker_id)               (write 1 byte to pipe)
```

All threads are `std::jthread` — calling `Stop()` or destroying the runtime joins them cleanly.

### Session Ownership

**Each session belongs to exactly one worker thread. That worker alone handles all protocol state for the session: decode, sequence numbers, timers, store persistence, encode, and write. No locks are needed on the hot path.**

When the front-door accepts a connection, it routes the connection to a worker via an inbox (mutex-protected push, then pipe-based wakeup). Once the worker adopts the connection, the front-door never touches it again.

Cross-thread session access goes through `SessionHandle`, which enqueues commands into a per-worker SPSC queue and wakes the target worker via its pipe fd:

```cpp
// From any thread — thread-safe:
session_handle.Send(msg);                // SPSC push → wakeup
session_handle.Snapshot();               // Query-only
session_handle.Subscribe();              // Event subscription

// NOT thread-safe:
message_view.get_string(11);             // Only within callback scope
```

### Application Callback Modes

#### Inline (`kInline`, default)

```
Session Worker Thread
    decode → validate → sequence → store
                                    │
                         OnAppMessage(event)   ← your code runs HERE
```

- Zero-copy `MessageView` in callback — no materialization overhead
- Callback **must not block** (no disk I/O, no mutex waits, no sleeps)
- Lowest possible latency

#### Queue-Decoupled (`kQueueDecoupled`)

```
Session Worker Thread                     App Worker Thread
    decode → validate → sequence → store
                                    │
                         SPSC push ──────► SPSC pop → OnAppMessage(event)
```

Two sub-modes:

| Sub-mode | Where app callback runs | Thread |
|----------|------------------------|--------|
| `kCoScheduled` | On session worker thread, during explicit `PollManagedQueueWorkerOnce()` call | Same as session worker |
| `kThreaded` | On dedicated app worker thread (`ff-app-wN`) | Separate thread |

Queue overflow policies when SPSC fills up:

| Policy | Behavior |
|--------|----------|
| `kCloseSession` | Close the session |
| `kBackpressure` | Return `Busy`, session worker retries later |
| `kDropNewest` | Silently drop the event |

### Cross-Thread Communication

| Path | Mechanism | Hot path? |
|------|-----------|-----------|
| Front-door → Worker | Mutex-protected inbox + pipe wakeup | No (once per connection) |
| Worker → App (queue mode) | Per-worker SPSC queue | Yes |
| App → Worker (send) | Per-worker SPSC command queue + pipe wakeup | Yes |
| Worker → Worker | Not directly supported (go through application layer) | — |
| Any → Worker (wakeup) | `PollWakeup::Signal()` writes 1 byte to pipe fd | Yes |

`PollWakeup` uses a per-worker `pipe(2)` fd pair. The `poll()` call monitors both connection fds and the wakeup fd. A single-byte write from any thread makes `poll()` return immediately.

### Polling Modes

| Mode | `poll()` timeout | CPU usage | Latency |
|------|-----------------|-----------|---------|
| `kBlocking` (default) | Configurable (e.g. 50ms) | Low when idle | Higher tail latency on sparse traffic |
| `kBusy` | 0 (return immediately) | 100% per worker core | Minimal latency jitter |

In busy-poll mode, worker threads spin continuously without sleeping. This is the recommended mode for ultra-low-latency deployments where each worker is pinned to a dedicated CPU core.

### CPU Affinity

FastFix uses `pthread_setaffinity_np()` (Linux) and `pthread_setname_np()` for thread naming. Non-Linux platforms silently no-op.

```
engine.front_door_cpu = 0           # Pin front-door to core 0
engine.worker_cpu_affinity = 1,2,3  # Pin workers to cores 1-3
engine.app_cpu_affinity = 4,5,6     # Pin app workers to cores 4-6
engine.poll_mode = busy             # Spin-loop (requires dedicated cores)
```

Typical low-latency deployment:

```
Core 0:  Front-door (accept only)
Core 1:  Session worker 0  ← hot path, isolated
Core 2:  Session worker 1
Core 3:  Session worker 2
Core 4:  App worker 0      (if kThreaded)
Core 5:  App worker 1
Core 6:  App worker 2
```

### Concurrency Rules

1. Each `SessionCore` is owned by exactly one worker thread — never shared.
2. Worker threads never take locks on the hot path.
3. Cross-thread communication uses `SpscQueue` (lock-free, wait-free).
4. `std::atomic` for counters (metrics, connection counts) — no mutex.
5. `std::mutex` only for cold-path operations: profile registry, metrics snapshot, managed queue runner setup.
6. Front-door thread (acceptor) hands off connections to workers via inbox + pipe wakeup.
7. `MessageView` is thread-affine to the callback scope — if you need the data elsewhere, explicitly copy.
8. Hot-path atomics use `std::memory_order_relaxed` for counters and `acquire/release` for SpscQueue head/tail.

---

## Profile Artifact Format

Binary format loaded via mmap:

```
┌──────────────────────────┐
│ Magic (4 bytes)          │  "FFPF"
│ Format Version (4 bytes) │
│ Profile ID (8 bytes)     │
│ Schema Hash (8 bytes)    │
│ Build ID (8 bytes)       │
│ Section Count (4 bytes)  │
├──────────────────────────┤
│ Section Table            │  Array of {kind, offset, size}
├──────────────────────────┤
│ StringTable              │  Packed null-terminated strings
├──────────────────────────┤
│ FieldDefs                │  Tag → {type, name_offset, flags}
├──────────────────────────┤
│ MessageDefs              │  MsgType → {name_offset, flags, rule_offset, rule_count}
├──────────────────────────┤
│ GroupDefs                │  CountTag → {delimiter_tag, name, member_fields}
├──────────────────────────┤
│ MessageFieldRules        │  Per-message: tag → required/optional
├──────────────────────────┤
│ GroupFieldRules          │  Per-group: tag → required/optional
├──────────────────────────┤
│ LookupTables             │  Hash tables for fast field/message/group lookup
└──────────────────────────┘
```

All sections are offsets into to the same mmap'd buffer — zero-copy, zero-allocation access.
