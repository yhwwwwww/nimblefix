# NimbleFIX 0.2 support matrix

The source API is frozen for 0.2.x. This is the implemented boundary of
[design §16](rewrite-design.md); [release evidence](release-0.2.md) records
functional, deployment and measurement checks.

| Area | Supported behavior | Explicit boundary |
| --- | --- | --- |
| Transport versions | FIX.4.2, FIX.4.3, FIX.4.4, FIXT.1.1; FIXT Logon checks configured DefaultApplVerID | Numeric IPv4/IPv6. Application dialect/schema must match the configured session. FIXT is session transport; FIX50SP2 supplies its application dictionary. |
| Standard schemas | Full FIX42, FIX43, FIX44, FIXT11 and FIX50SP2 dictionary generation; scalar types, enums, components, nested groups, binary DATA length pairs | Metadata validates dictionary rules; business conditions expressed only in prose still belong to the application. Generated standard headers are optional. |
| Builders/views | Static tags, fixed shapes, generated named/tag setters/getters; bounded groups; explicit DynamicMessage | Views and SessionContext are borrowed for one callback. Keep owned values for later use. Managed header tags cannot be set by applications. |
| Validation | Structure, required, strict; dictionary enums, scalar representations and group boundaries | Raw flat mode uses explicit application validation; selecting typed Engine or runtime XML adds dictionary checks. Unknown/unhandled typed messages are rejected before business code. |
| Logon/authentication | Initiator/acceptor, configured CompIDs/version, positive HeartBtInt, EncryptMethod=0, credentials/custom fields, callback authentication | Acceptor authenticates before opening a journal. Credentials require TLS by default; explicit require_tls=false is available. Non-recovery Logon content is removed from persisted payloads. |
| Heartbeat/TestRequest | Timer Heartbeat, matching TestReqID and finite liveness/write deadlines | Inline callbacks must finish promptly. User C++ callbacks cannot be forcibly preempted. |
| SendingTime | UTC calendar/precision checks; default Engine skew tolerance 120 seconds; Reject then Logout on excessive skew | Low-level Driver with absent SessionSettings retains legacy deterministic test behavior. Clock synchronization is an operator responsibility. |
| Gaps/replay | ResendRequest, GapFill, forward SequenceReset reset mode, original-sequence application replay with PossDup/OrigSendingTime | Replay cannot bypass a partially written frame. Required history that has been pruned causes history_unavailable, never a fabricated business GapFill. |
| Sequence epochs | Explicit allowed Logon reset, new audited epoch, sequence one only by policy | Default is no automatic reset. Setting logon.request_sequence_reset=true requests a reset on each new connection. Pending input/read leases prevent unsafe reset. |
| FIX 369 | Emit highest contiguous business-complete input; validate peer progress; combine peer acknowledgement and retention window | Accepted or merely appended input is not acknowledged as processed while its callback/replies remain unfinished. No acknowledgement means conservative retention. |
| Logout/rejects | Local/peer/simultaneous Logout, write-before-close, finite shutdown; structured Session Reject for schema/type failures; peer Reject observation | Full counterparty certification is outside the test suite. BusinessMessageReject/business-specific rejection rules remain application policy. |
| Inline/Queued | One protocol owner for either policy; Queued uses a bounded worker pool and one in-flight callback per session | Queued completion follows dependent reply append, not just worker return. Slow-session queues pause reads with bounded age/timeout. Inline callback work affects its owner. |
| Sender/lane | Ordinary Sender accepts multiple producer threads; advanced lane explicitly uses a bounded SPSC channel | Same-producer order only; no cross-session or cross-producer total order. Cross-thread lane misuse reports wrong_thread. |
| Receipts | accepted, appended, written, failed, expired with bounded retention | accepted owns a command; appended commits the frame; written means transport accepted bytes. Peer receipt and business acceptance require separate evidence. |
| Journal | Versioned segmented mmap queue, CRC/commit boundaries, index/checkpoint rebuild, rotation, independent readers and leases | v1 is retained explicitly with segment_bytes=0. No implicit migration or memory store. Segment indexes/RecordIds are never reused in a store. |
| Retention | Peer replay floor, configured outbound window, contiguous completed input, reader cursor/lag and mapping/TX leases | May retain conservative sequence-anchor segments. Full storage fails closed; it cannot overwrite required history. There are finite segment-index and epoch-audit limits. |
| Recovery | Append before TX/callback, local unfinished redelivery, dependent reply reuse by record/ordinal/content | At-least-once business invocation. Applications must make external side effects idempotent and reproduce replies in the same order/content. |
| Confirmation | appended under same-host process death while OS/filesystem remain operating | No per-message fsync/msync or power-loss guarantee. Index/audit/rotation are included in storage budgets. |
| TLS | Optional OpenSSL 3, TLS 1.2/1.3, SAN verification, optional mTLS, nonblocking WANT_READ/WANT_WRITE, finite handshake/record retry memory | Encrypted private keys requiring an interactive password are not supported. Verification failure closes the stream; no plaintext fallback. |
| Identity | Transport version, oriented CompIDs and a length-delimited local qualifier | Coexisting qualified acceptors with the same wire identity use distinct listeners; ambiguous routing on one listener is rejected offline. |
| Scheduling | Explicit UTC weekday/time windows including overnight windows; bounded Logout at close and reconnect at opening | No timezone/DST interpretation or automatic epoch reset. |
| Configuration/CLI | Fixed TOML v1 parser, aggregate typed diagnostics, relative paths, explicit secret environment references; init/validate/run/schema/inspect/doctor/optional bench | Offline validation does not bind or acquire a store writer. CLI run observes business input; applications/examples supply business replies. |
| Diagnostics | Bounded off-owner error channel, dropped-event counter and thread-safe session snapshots | Snapshot counters are independently sampled; a snapshot is not a transactional view across sessions. Observer exceptions stop the Engine. |
| Manual lifecycle | start/poll_once on one caller-owned thread, same driver/transport/recovery as run; thread-safe request_stop | One run per Engine. Continue polling through bounded shutdown; recreate with the same storage directory for a new run. Queued/diagnostic threads remain explicit components. |
| Installation | Relocatable CMake targets, CLI, headers, dictionaries/licenses, CMake and xmake XML helpers; optional capabilities declared as package components | Rebuild dependent binaries when upgrading; no C++ ABI guarantee. Core-only configure/build needs no XML/OpenSSL/Python. |
| Independent checks | QuickFIX 1.16.0 in both roles, real file stores/restarts; independent raw peers for every transport in both roles and policies | These validate the stated matrix, not universal vendor certification. Performance comparisons need identical boundaries/settings. |

Engine defaults are 30 s heartbeat, 10 s connect/Logon/TLS handshake, 250 ms
reconnect, 30 s write stall/pending age/read pause/delivery age, 5 s Logout;
64 sessions, 32 pending sockets, 256 TX messages/4 MiB, 128 pending input
messages/4 MiB, 256 ordinary Sender commands/4 MiB, 1,024 receipts,
2 Queued workers and 128 callbacks/4 MiB. Maximum frame is 64 KiB. Eight TX
slots and two maximum-frame byte capacities reserve administration capacity.
Age expiry can explicitly be disabled for outbound messages with
max_pending_age_ms=0. Administrative traffic follows TCP order and cannot
jump over earlier blocked bytes.

A normal Engine defaults to 16 MiB total journal budget and 1 MiB segments,
65,536 indexed records and 8,192 retained outbound messages per session.
The v2 epoch audit reserves 1 MiB when the budget supports it. Both byte and
record limits apply. Readers default to 16 slots and 65,536-record maximum lag.
A v2 store supports at most 65,535 segment indexes and 1,024 audited resets;
exhaustion is a visible bounded failure. Provision capacity for unacknowledged
history and slow readers, and rotate operational identities only by an
explicit bilateral session procedure.
