# Codec / slot implementation slice

Historical record for the named stage commits. Current release behavior is in
[the 0.2 support matrix](session-support.md) and [release record](release-0.2.md).

This records the first implementation slice in §16 of the rewrite design.
The public surface is experimental C++20: message slots, exact decimal values,
bounded FIX 4.4 encoding, frame inspection, flat parsing, and a minimal XML
generator. This slice does not start a FIX session or open a store or socket.
The subsequent [session/TCP record](session-tcp-slice.md) closes stages 2–3;
its support matrix describes the current runtime rather than this historical
codec-only boundary.

## Public API and invariants

| Surface | Contract |
| --- | --- |
| `message<MsgType>().set<Tag>()` | A move-only, bounded owning expression. Ordinary strings are copied before a setter returns. Flat declaration order determines wire order. Default limits: 64 fields, 4096 value bytes. |
| `FixedMessage<MsgType, Fields<...>, ByteCapacity>` | Declared shape only; 16-byte value slots, compact presence bits, fixed arena. `reset()` clears used bytes, presence and sticky errors. Text replacement compacts the arena and adjusts offsets. No heap fallback. |
| `encode_fix44(message, SessionHeader, span<char>)` | Validates and sizes before writing. Failure leaves output unchanged. Output must not overlap message/header input storage. Header values are caller-supplied in this stateless slice. |
| `Decimal`, `"..."_dec` | Signed 64-bit coefficient and scale 0–18. Exact text conversion and trailing scale preservation, without binary floating point. Invalid runtime input returns `Status`. |
| `inspect_frame` | Reports incomplete input separately from invalid input, validates one frame, and leaves subsequent coalesced frames unconsumed. Default maximum frame size: 65,536 bytes. |
| `parse_flat` | Revalidates framing and checksum, uses caller scratch, preserves ordered duplicate tags. `find_unique` distinguishes absent and repeated tags. |
| `WireHints` | Adds up to 64 nonconflicting custom Length/Data pairs. The standard FIX44 pairs are built in. Recognized Data reads an explicit positive byte length, including embedded SOH/NUL. |
| XML generated builders | Named setters delegate to static tag setters. The same schema/message/shape shares canonical `plan_type` and plan address across setter orders. A compact static slot map resolves physical payload order without moving prefixes. |

Tags are restricted to 1–999999999. Plain text values are nonempty and reject
C0/DEL; `=` and Latin-1 high bytes are preserved. Unknown binary fields cannot
be inferred without wire hints. A flat view cannot determine repeating-group
boundaries or validate business requirements.

View lifetimes are explicit: a flat view borrows both the frame and its scratch
index; a builder view borrows the builder's current storage. A mutation, move,
reset or destruction can invalidate borrowed views. Builders support exclusive
ownership and transfer; concurrent mutation/use of the same object is not
supported. There is no thread-local message lifetime dependency.
The replay encoder similarly requires distinct output storage from its
borrowed original frame and header strings.

The XML sample is a two-message subset, not a complete FIX44 dictionary. The
generator supports `STRING`, `CHAR`, `INT`, `PRICE`, `QTY`, `UTCTIMESTAMP`,
required flags and enum membership. Quantity/price accept exact decimals or
in-range integers; numeric strings and binary floating point fail compilation.
UTC timestamps accept whole seconds or three fractional digits, with calendar
validation. Business conditional requirements, nested groups, components,
typed receive projections, runtime XML loading, and static Data sending are
outside this slice and are not advertised as implemented.

## Verification

The authoritative repeatable commands are:

```sh
xmake f -m release --toolchain=gcc --sanitize=n --fuzzers=n --benchmarks=y -o build
xmake check
python3 tests/compile_fail_test.py --cxx clang++
python3 tests/install_test.py
python3 tests/install_test.py --core-only
python3 tests/install_test.py --cxx clang++
python3 tests/schema_size_test.py
```

`xmake check` now runs the codec contracts together with journal/session/TCP
tests, 20 XML generator tests, 15 positive-control-backed compile-fail cases,
and executable examples. GCC 16.2.1 and Clang 23 are verified on Linux.
CMake runs the corresponding core contracts and installed-consumer tests
remain separate commands.

The public contract fixture fixes BodyLength=122 and CheckSum=212 independently
of the encoder. Every truncated prefix, coalesced frames, exact capacity
boundaries, decimal extremes, temporary strings and moved builders are checked.
Generated named/tagged paths have both type-level plan identity and byte-level
wire assertions. A 2,000-step independent string model exercises replacement,
capacity reclamation, and borrowed substrings spanning compacted regions.
Parser tests include malformed boundaries, integer overflow, checksum
corruption, duplicate fields, binary payloads, and a deterministic malformed
buffer sweep.

Clang ASan/UBSan passes with `ASAN_OPTIONS=detect_leaks=0` in this ptrace-based
environment. A seeded libFuzzer run completed 100,000 executions without a
sanitizer failure. This is a smoke test, not an exhaustive fuzzing claim:

```sh
xmake f -m debug --toolchain=clang --sanitize=y --fuzzers=y --benchmarks=n -o build-sanitize
ASAN_OPTIONS=detect_leaks=0 xmake check
ASAN_OPTIONS=detect_leaks=0 xmake run parser-fuzz -runs=100000 -max_len=1024
```

The install test builds from a read-only source copy, installs to a temporary
prefix, relocates that prefix, hides both original source and build paths,
and builds standalone consumers. It also modifies XML and verifies automatic
regeneration using the installed helper, and rejects unknown required CMake
components. Core-only and generated-schema consumers are separate checks.
The GitHub Actions workflow runs xmake contracts on GCC/Clang, installed
consumers, benchmark smoke, and a sanitizer/fuzzer job. Remote CI has not been
run locally.

## Review outcome

The integration review found and corrected these issues before the code commit:

- Decimal integer digits were reversed for scaled numbers. Hard-coded decimal
  and wire expectations now cover that formatter independently.
- Framing did not require the final body SOH, and plain parsing admitted empty
  and control-containing values. Both now have targeted regressions.
- XML required/enum metadata initially had no enforcing path; numeric and
  timestamp XML types also needed enforcement in `set<Tag>()`. Names and tags
  now share those checks.
- Different setter orders originally produced different plan objects. Canonical
  plan identity is now shared, with independent physical slot mapping.
- Arena growth and compaction needed to preserve self-referential strings,
  including substrings crossing a moved boundary. Focused and reference-model
  tests cover these ownership cases.
- Installed sample generation was absent from the default install build, and
  the generator path did not relocate. External-consumer tests exposed and
  verified both fixes.
- Generated C++ identifiers and non-BMP description escaping could produce
  invalid source. Reserved symbols and UTF-8 octal escaping are tested.

No blocking correctness finding remains for this documented codec subset.
This does not constitute FIX session or production trading readiness.

## Measurement and remaining gates

The codec benchmark keeps per-operation CSV samples, environment metadata,
allocation counts and per-case page-fault deltas. It separates clock overhead,
byte-copy baseline, fixed encode, fixed reset/build/encode, fluent build/encode,
generated build/encode, and inspect/flat parse. It includes no socket, journal,
callback dispatch, syscall trace, shared-lock counter or hardware cache counter.
The schema-size probe fixes a two-field shape while adding 1,000 unused optional
fields and widening tag spacing; it records slot/builder size, generated header
size, compile time and probe binary size.

The [measured baseline and raw files](../bench/results/codec-initial/README.md)
identify implementation commit `059c3f3`, with three runs and a fixed-shape
schema-size probe. Results from this development machine are exploratory; no
latency SLO is set here.
Fluent construction still allocates a bounded backing block and grows it;
fixed-shape construction/encode and flat parse are the allocation-free paths.
Pooling/reusable fluent storage remains work before claiming an allocation-free
default engine path.

Stage 3 adds independent QuickFIX dictionary/wire and TCP interoperability,
an initial controlled `S/K/B` matrix, an 8,192-message rotating working set,
independent minimal parse/encode and network echo baselines. These extend the
initial record rather than changing its old measurements. Full workload
coverage, instruction/cache/syscall/lock tracing and dedicated-machine
repeatability remain later gates. No relative speedup claim is made.

The real appended journal, deterministic session core, fake time/transport,
receipt/delivery tracking and crash/short-write/EAGAIN tests are documented
in the [stage 2–3 acceptance record](session-tcp-slice.md). The design-baseline
commit remains the rollback point before this stateless implementation.

Protocol expectations were checked against the FIX Trading Community's
[TagValue standard](https://fixtrading.org/packages/fix-tagvalue-encoding/),
[FIX 4.4 field definitions](https://fiximate.fixtrading.org/legacy/en/FIX.4.4/),
and [Orchestra Length/Data definition](https://github.com/FIXTradingCommunity/fix-orchestra-spec/blob/master/v1-0-STANDARD/orchestra_spec.md).
