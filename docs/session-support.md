# Stage 3 support matrix

This describes the implemented FIX 4.4 demonstration boundary. The complete
release target remains the [rewrite design](rewrite-design.md); stages 4–7
extend protocol coverage, execution modes, deployment and performance work.

| Area | Implemented at stage 3 | Boundary |
| --- | --- | --- |
| Codec | FIX.4.4 BodyLength/CheckSum, bounded frames, partial/coalesced inspection, ordered flat fields, exact decimals | Flat mode does not validate application business requirements or group structure. |
| Builders | Static tags, fixed shapes, generated sample names/tags with one encoder | Two-message XML sample; six scalar types. Full FIX44 bindings, groups, typed receive getters and conditional validation follow. |
| Engine | `Engine<>`, inline callbacks, one `poll` owner, movable/noncopyable ownership | `Engine<OtherSchema>` fails compilation. No queued executor, ordinary Sender or MPSC ingress yet. |
| Logon | Initiator and acceptor, persistent sequence numbers, CompID matching, EncryptMethod=0, positive HeartBtInt | No credentials, TLS, ResetSeqNumFlag reset policy, NextExpectedMsgSeqNum negotiation or session schedules. |
| Heartbeat/TestRequest | Timer-driven Heartbeat, matching TestReqID response, bounded liveness timeout | TestRequest timeout starts when written; TCP stalls have their own deadline. No arbitrary callback preemption. |
| Sequence gaps | ResendRequest on high sequence, bounded buffering of a high Logon, activation after the missing range closes | Not a complete FIX session conformance suite. Sequence reset mode is unsupported. |
| Replay | Stored application frames replay with original sequence, PossDupFlag=Y and OrigSendingTime; skipped admin ranges use GapFill | Replay never interrupts a partially written frame; bounded replay work can pause on EAGAIN. |
| GapFill | SequenceReset with GapFillFlag=Y, increasing NewSeqNo committed atomically with the input | GapFillFlag absent/N is rejected. No administrative reset workflow. |
| Duplicates | Low sequence requires valid PossDup/OrigSendingTime and is not delivered again | No application order deduplication or business reconciliation. |
| Logout | Local request waits for peer acknowledgement or timeout; peer request closes after its reply is written | Expiry reports `logout_timeout`; bounded Engine shutdown returns the failure. It cannot guarantee peer acknowledgement over a stalled stream. |
| Rejects | Header/identity/protocol failures produce status/error events and close the affected session; business callback failure is isolated | Complete Reject/BusinessMessageReject generation and validation policies are stage 4. Received admin Reject is not a business callback. |
| Journal | Preallocated file-backed MAP_SHARED segment, explicit format/version/identity, CRC32C and atomic commit, exclusive writer | Fixed capacity and epoch; no rotation, retention, compaction, reader leases or automatic file migration. Full store errors fail the session. |
| Crash recovery | Sequence scan, inbound redelivery, dependent reply reuse, delivery markers, original-sequence replay | Same-host process death with OS/filesystem operating. No fsync/msync durability or power-loss guarantee. |
| Delivery | Append before callback, completion after successful callback and dependent replies; `is_recovery` and stable record id | At-least-once callback invocation. External side effects require application idempotency. Recovery replies must repeat the same send order/content. |
| Send/receipt | Driver-level appended/written/failed receipts with bounded expiry, rejection without sequence advance, low-water callback | Written means transport accepted bytes; peer receipt and business acceptance are separate. Engine-level ordinary sender/receipt subscription is future work. |
| Backpressure | Message/byte/RX bounds, admin reserve, short-write offset, EAGAIN, age and write-stall limits | Admin traffic obeys the same ordered TCP stream. A reserved Logout cannot bypass blocked earlier bytes. |
| TCP acceptor | Separate listeners and allowlisted session identities, duplicate connection rejection, pending Logon bounds | Numeric IPv4/IPv6 hosts. No DNS lookup, dynamic authentication or dynamic session creation. |
| Identity | Version plus oriented local/remote CompIDs, length-delimited and independent of display name or TCP port | Qualifiers and other FIX versions are not exposed yet. Opposite directions keep distinct identities. |
| Scheduling | Round-robin owner service with a 64 KiB read-turn budget and 16 frames per buffer-processing pass; complete buffered frames requeued | A read turn can run more than one buffer pass. Inline callback work must be bounded; one blocked C++ callback can block its owner. |
| Stop/configuration | Offline numeric-address/resource validation; frozen registrations; atomic/eventfd stop wakeup | Run once per Engine; recreate with the same directory for restart. `SessionContext::logout()` is restricted to the active callback; use `request_stop()` for whole-engine stop. |
| Installation | Relocatable CMake package, generated sample/helper, public headers and four standalone examples | xmake is the primary development build; CMake is also tested for packaging and external applications. |
| Independent checks | QuickFIX 1.16.0 dictionary/wire oracle, live SocketInitiator/FileStore round trip and restart; two independent Python reconnect peers | No QuickFIX performance comparison or full cross-vendor certification is claimed. |

Error observers also execute inline on the owner and must be bounded; the
examples forward events to a bounded sink and print elsewhere. A built-in
asynchronous diagnostic channel is later work. Reaching the pending-connection
limit closes the excess unauthenticated socket; stage 3 does not publish a
separate capacity event for that rejection.

Engine defaults: heartbeat 30 s, connect/logon 10 s, reconnect delay 250 ms,
write stall and oldest pending message 30 s, Logout 5 s; 64 sessions,
32 pending connections, 256 pending messages / 4 MiB TX and 128 pending
inbound messages / 4 MiB RX. Maximum frame is 64 KiB, journal 16 MiB /
65,536 records per session. Startup rejects incompatible budgets; eight TX
slots and two maximum-frame byte capacities are reserved for administration.
Set `max_pending_age_ms=0` explicitly to disable age expiry. The lower-level
SessionDriver has its own documented configuration defaults in the public
header, including a 1,024-entry receipt ring.

The journal reserves file space at startup. The implementation returns
`storage_full`, `store_locked`, `identity_mismatch`, `unsupported_version`
or `corrupt_store` for their respective conditions. It does not recreate a
failed store with empty sequence state. An incomplete tail can be discarded;
committed corruption or a hole before later committed records is an error.
