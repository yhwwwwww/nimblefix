#!/usr/bin/env python3
"""Run the full FIX44 schema benchmark and save raw samples plus metadata.

The three slot references deliberately stop at reset/fill/presence enumeration.
The direct writer is a fixed-layout wire reference with a narrower validation
boundary than the generated encoder; both boundaries are recorded in the report.
"""
import argparse
import csv
from collections import Counter
import datetime
import gzip
import hashlib
import io
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
from run import source_digest as project_source_digest


def command_output(*command):
    try:
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    except OSError:
        return "unavailable"
    return result.stdout.strip() if result.returncode == 0 else "unavailable"


def source_digest(generated_header):
    digest = hashlib.sha256()
    digest.update(project_source_digest().encode("ascii"))
    paths = [ROOT / "bench/schema_bench.cpp", ROOT / "bench/schema_run.py",
        ROOT / "bench/perf_counters.hpp", generated_header, ROOT / "tools/schema/generate.py",
        ROOT / "schemas/standard/FIX44.xml"]
    for path in paths:
        digest.update(path.name.encode("utf-8") + b"\0" + path.read_bytes())
    return digest.hexdigest()


def schema_counts(generated_header):
    text = generated_header.read_text(encoding="utf-8")
    result = {}
    for name in ("field_count", "message_count", "group_count"):
        match = re.search(rf"static constexpr std::size_t {name} = ([0-9]+);", text)
        if match is None:
            raise RuntimeError(f"generated FIX44 header is missing Schema::{name}")
        result[name.removesuffix("_count") + "s"] = int(match.group(1))
    return result


def percentile(values, quantile):
    return values[max(0, math.ceil(len(values) * quantile) - 1)]


def summarize(result, output_dir, run_number):
    rows = list(csv.DictReader(io.StringIO(result.stdout)))
    if not rows:
        raise RuntimeError("schema benchmark returned no per-sample rows")
    expected_columns = {
        "case", "iteration", "elapsed_ns", "allocation_calls", "allocation_bytes",
        "wire_bytes", "instance_bytes", "initialization_bytes", "visited_slots",
        "active_indices", "sort_comparisons",
    }
    if set(rows[0]) != expected_columns:
        raise RuntimeError(f"unexpected schema benchmark CSV columns: {sorted(rows[0])}")
    grouped = {}
    for row in rows:
        grouped.setdefault(row["case"], []).append(row)

    summaries = {}
    for case, samples in grouped.items():
        latencies = sorted(int(row["elapsed_ns"]) for row in samples)
        first = samples[0]
        summary = {
            "samples": len(samples),
            "p50_ns": percentile(latencies, 0.50),
            "p99_ns": percentile(latencies, 0.99),
            "p999_ns": percentile(latencies, 0.999),
            "max_ns": max(latencies),
            "loop_ops_per_second": len(samples) * 1e9 / int(result.operations[case]["wall_ns"]),
            "allocation_calls_per_operation": statistics.mean(
                int(row["allocation_calls"]) for row in samples),
            "allocation_bytes_per_operation": statistics.mean(
                int(row["allocation_bytes"]) for row in samples),
            "wire_bytes": int(first["wire_bytes"]),
            "instance_bytes": int(first["instance_bytes"]),
            "initialization_bytes": int(first["initialization_bytes"]),
            "visited_slots": int(first["visited_slots"]),
            "active_indices": int(first["active_indices"]),
            "sort_comparisons": int(first["sort_comparisons"]),
            "minor_faults": int(result.operations[case]["minor_faults"]),
            "major_faults": int(result.operations[case]["major_faults"]),
            "wall_ns": int(result.operations[case]["wall_ns"]),
        }
        if len(samples) >= 100_000:
            summary["p9999_ns"] = percentile(latencies, 0.9999)
        summaries[case] = summary

    filename = f"run-{run_number}.csv.gz"
    with (output_dir / filename).open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
            compressed.write(result.stdout.encode("utf-8"))
    return filename, summaries


def run_benchmark(binary, samples, workset):
    result = subprocess.run([str(binary), str(samples), str(workset)], cwd=ROOT,
        text=True, capture_output=True, check=True)
    operations = {}
    schema = None
    for line in result.stderr.splitlines():
        try:
            item = json.loads(line)
        except json.JSONDecodeError as error:
            raise RuntimeError(f"unparseable benchmark diagnostics: {line}") from error
        if "case" in item:
            operations[item["case"]] = item
        elif "schema" in item:
            schema = item
    if not operations or schema is None:
        raise RuntimeError("schema benchmark diagnostics are incomplete")
    return type("BenchmarkResult", (), {
        "stdout": result.stdout,
        "operations": operations,
        "schema": schema,
    })()


def run_size_probe(compiler, output_dir):
    report = output_dir / "schema-size.json"
    command = [os.environ.get("PYTHON", "python3"), str(ROOT / "tests/schema_size_test.py"),
        "--cxx", compiler, "--output", str(report)]
    subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=True)
    result = json.loads(report.read_text(encoding="utf-8"))
    rows = result.get("measurements", [])
    if len(rows) != 3 or {row.get("unused_optional_fields") for row in rows} != {0, 1000}:
        raise RuntimeError("schema size probe did not return the N=0/1000 and sparse-tag cases")
    return result


def run_standard_matrix(compiler, timeout_seconds):
    generator = ROOT / "tools/schema/generate.py"
    inputs = sorted((ROOT / "schemas/standard").glob("*.xml"))
    expected = {"FIX42.xml", "FIX43.xml", "FIX44.xml", "FIX50SP2.xml", "FIXT11.xml"}
    if {path.name for path in inputs} != expected:
        raise RuntimeError(f"standard schema set differs from expected: {[p.name for p in inputs]}")
    measurements = []
    with tempfile.TemporaryDirectory(prefix="nimblefix-standard-schema-bench-") as temporary:
        base = Path(temporary)
        for xml in inputs:
            short_name = xml.stem.lower()
            namespace = f"nimblefix::bench_{short_name}"
            header = base / f"{short_name}.hpp"
            start = time.perf_counter()
            subprocess.run([os.environ.get("PYTHON", "python3"), str(generator),
                "--namespace", namespace, "--output", str(header), str(xml)],
                cwd=ROOT, text=True, capture_output=True, check=True, timeout=timeout_seconds)
            generation_seconds = time.perf_counter() - start

            source = base / f"{short_name}.cpp"
            if xml.stem == "FIXT11":
                usage = (f'std::array<char, 256> buf{{}}; '
                         f'auto r = nimblefix::encode_body_stream<{namespace}::Logon>(buf, [](auto& w) {{ '
                         f'w.encrypt_method(0).heart_bt_int(30).default_appl_ver_id("9"); }}); (void)r;')
            else:
                usage = f"""std::array<char, 256> buf{{}};
                    auto r = nimblefix::encode_body_stream<{namespace}::NewOrderSingle>(buf, [](auto& w) {{
                        w.cl_ord_id("STANDARD").handl_inst('1').symbol("AAPL").side('1')
                         .transact_time("20261031-12:00:00").order_qty(100).ord_type('1');
                    }}); (void)r;
                    {namespace}::NewOrderSingleView view{{nimblefix::FlatMessageView{{}}}};
                    (void)view.get<11>(); (void)view.get<38>();"""
            source.write_text(
                f'#include <array>\n#include "{header}"\n'
                f'int main() {{ {usage} return {namespace}::Schema::field_count == 0 ? 1 : 0; }}\n',
                encoding="utf-8")
            start = time.perf_counter()
            subprocess.run([compiler, "-std=c++20", "-I", str(ROOT / "include"),
                "-fsyntax-only", str(source)], cwd=ROOT, text=True, capture_output=True,
                check=True, timeout=timeout_seconds)
            compile_seconds = time.perf_counter() - start
            measurements.append({
                "xml": xml.name,
                "normalized_fingerprint": read_fingerprint(header),
                "generation_seconds": generation_seconds,
                "generated_header_bytes": header.stat().st_size,
                "syntax_compile_seconds": compile_seconds,
                "compile_order": len(measurements) + 1,
                "parallelism": 1,
            })
    return measurements


def read_fingerprint(header):
    for line in header.open(encoding="utf-8"):
        match = re.search(r"normalized fingerprint ([0-9a-f]{64})", line)
        if match is not None:
            return match.group(1)
    raise RuntimeError(f"generated header lacks a normalized fingerprint: {header.name}")


def collect_internal_pmu(binary, samples, workset):
    result = subprocess.run([str(binary), str(samples), str(workset), "--pmu"], cwd=ROOT,
        text=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if result.returncode != 0:
        return {"status": "unavailable", "source": "in-process perf_event_open",
            "reason": f"diagnostic process exited {result.returncode}: {result.stderr[-1500:]}"}
    counters = None
    for line in result.stderr.splitlines():
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            continue
        if "pmu" in item:
            counters = item["pmu"]
    if counters is None:
        return {"status": "unavailable", "source": "in-process perf_event_open",
            "reason": "diagnostic process returned no PMU JSON"}
    events = counters.get("events", {})
    available = [name for name, event in events.items() if event.get("available")]
    if not available:
        return {"status": "unavailable", "source": "in-process perf_event_open",
            "reason": "all hardware counters failed to open", "counters": counters}
    return {
        "status": "collected",
        "source": "in-process perf_event_open",
        "scope": "whole-benchmark aggregate from a separate diagnostic run; includes validation, setup, warmups, measured cases, and CSV/diagnostic harness; not per-case attribution",
        "available_events": available,
        "counters": counters,
    }


def collect_perf(binary, samples, workset, output_dir):
    perf = shutil.which("perf")
    external_failure = None
    if perf is not None:
        report = output_dir / "perf-stat.txt"
        result = subprocess.run([perf, "stat", "-x,", "-e", "cycles,instructions,cache-misses",
            "-o", str(report), "--", str(binary), str(samples), str(workset)],
            cwd=ROOT, text=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if result.returncode == 0:
            return {
                "status": "collected",
                "source": "perf stat",
                "scope": "whole-benchmark aggregate from a separate diagnostic run; includes validation, setup, warmups, measured cases, and CSV/diagnostic harness; not per-case attribution",
                "events": report.read_text(encoding="utf-8", errors="replace"),
            }
        external_failure = (result.stderr.strip() or
            report.read_text(encoding="utf-8", errors="replace").strip())[:1000]

    internal = collect_internal_pmu(binary, samples, workset)
    if external_failure is not None:
        internal["external_perf_failure"] = external_failure
    elif perf is None:
        internal["external_perf"] = "not installed"
    return internal


def run_clang_template_proxy(clang, generated_header):
    compiler = clang or shutil.which("clang++")
    if compiler is None:
        return {"status": "skipped", "reason": "clang++ not found"}
    with tempfile.TemporaryDirectory(prefix="nimblefix-clang-template-trace-") as temporary:
        base = Path(temporary)
        source = base / "fix44_consumer.cpp"
        output = base / "fix44_consumer.o"
        source.write_text(
            "#include <array>\n"
            "#include <nimblefix/codec.hpp>\n"
            f'#include "{generated_header}"\n'
            "int consume_generated_fix44() {\n"
            "  constexpr nimblefix::SessionHeader header{\"BUY\", \"SELL\", 1, \"20261031-12:00:00.000\"};\n"
            "  std::array<char, 512> bytes{};\n"
            "  return nimblefix::encode_fix44_stream<nimblefix::fix44::NewOrderSingle>(header, bytes, [](auto& w) {\n"
            "      w.cl_ord_id(\"TRACE-1\").handl_inst('1').symbol(\"AAPL\").side('1')\n"
            "       .transact_time(\"20261031-12:00:00.000\").order_qty(10).ord_type('1');\n"
            "  }).ok() ? 0 : 1;\n"
            "}\n", encoding="utf-8")
        command = [compiler, "-std=c++20", "-O2", "-ftime-trace",
            "-ftime-trace-granularity=0", "-I", str(ROOT / "include"),
            "-c", str(source), "-o", str(output)]
        start = time.perf_counter()
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, timeout=900)
        compile_seconds = time.perf_counter() - start
        if result.returncode != 0:
            return {
                "status": "failed",
                "compiler": command_output(compiler, "--version").splitlines()[0],
                "compile_seconds": compile_seconds,
                "stderr": result.stderr[-4000:],
            }
        candidates = [output.with_suffix(".json"), source.with_suffix(".json"), *base.glob("*.json")]
        trace = next((path for path in candidates if path.is_file()), None)
        if trace is None:
            return {
                "status": "unavailable",
                "reason": "Clang completed but emitted no -ftime-trace JSON",
                "compile_seconds": compile_seconds,
                "object_bytes": output.stat().st_size,
            }
        trace_data = json.loads(trace.read_text(encoding="utf-8"))
        names = [event.get("name", "") for event in trace_data.get("traceEvents", [])]
        exact_counts = Counter(name for name in names if name in {
            "InstantiateClass", "InstantiateFunction", "InstantiateFunctionDefinition"})
        return {
            "status": "collected",
            "compiler": command_output(compiler, "--version").splitlines()[0],
            "flags": "-std=c++20 -O2 -ftime-trace -ftime-trace-granularity=0 -c",
            "boundary": "one small FIX44 consumer translation unit; compiler trace events are an instantiation proxy, not a compiler-independent template count",
            "compile_seconds": compile_seconds,
            "object_bytes": output.stat().st_size,
            "trace_bytes": trace.stat().st_size,
            "instantiation_events": {
                "InstantiateClass": exact_counts["InstantiateClass"],
                "InstantiateFunction": exact_counts["InstantiateFunction"],
                "InstantiateFunctionDefinition": exact_counts["InstantiateFunctionDefinition"],
                "InstantiateFunction_total": sum(count for name, count in exact_counts.items()
                    if name.startswith("InstantiateFunction")),
            },
        }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--generated-header", type=Path,
        default=ROOT / "build/generated/nimblefix/fix44.hpp")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=5000)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--workset", type=int, default=1024)
    parser.add_argument("--compiler", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--clang", help="Clang executable for the optional template instantiation proxy")
    parser.add_argument("--flags", default="C++20; optimized release build; --wrap=malloc")
    parser.add_argument("--skip-size-probe", action="store_true",
        help="skip the small synthetic N=0/1000 plus sparse-tag codegen/size probe")
    parser.add_argument("--standard-schemas", action="store_true",
        help="also generate and syntax-compile each of the five shipped standard XML schemas serially")
    parser.add_argument("--compile-timeout", type=int, default=1800,
        help="per-schema timeout for the optional standard schema generation/compile matrix")
    parser.add_argument("--skip-perf", action="store_true",
        help="do not attempt perf_event counters")
    args = parser.parse_args()
    if not 100 <= args.samples <= 100_000 or not 1 <= args.runs <= 20:
        parser.error("samples must be 100..100000 and runs must be 1..20")
    if not 1 <= args.workset <= 8192 or args.compile_timeout < 1:
        parser.error("workset must be 1..8192 and compile timeout must be positive")

    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"benchmark executable not found: {binary}")
    generated_header = args.generated_header.resolve()
    if not generated_header.is_file():
        parser.error(f"generated FIX44 header not found: {generated_header}; build fix44-schema first")
    args.output.mkdir(parents=True, exist_ok=True)

    metadata = {
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "revision": command_output("git", "rev-parse", "HEAD"),
        "worktree_dirty": bool(command_output("git", "status", "--porcelain")),
        "source_sha256": source_digest(generated_header),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "binary_bytes": binary.stat().st_size,
        "compiler": command_output(args.compiler, "--version").splitlines()[0],
        "build_flags": args.flags,
        "kernel": platform.platform(),
        "cpu": command_output("lscpu"),
        "cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else [],
        "cpu_pinned": hasattr(os, "sched_getaffinity") and len(os.sched_getaffinity(0)) == 1,
        "schema": "full generated FIX 4.4 XML dictionary",
        "schema_counts": {
            **schema_counts(generated_header),
            "generated_header_bytes": generated_header.stat().st_size,
            "normalized_fingerprint": read_fingerprint(generated_header),
        },
        "timing": "steady_clock per-operation user-space duration; includes clock, assertions, and allocation-hook overhead",
        "allocation_scope": "linker-wrapped direct malloc and overridden C++ new; excludes shared-library internal allocations and aligned new",
        "pmu_scope": "separate diagnostic run; whole-process aggregate counters include validation, setup, warmups, measured cases, and output/harness work; not attributed per case",
        "reference_boundaries": {
            "shape_local_fill_enumerate": "test-only shape-sized RefSlot array; reset, fill, and scan S slots for K present values. No formatting, frame encoding, schema validation, or public API.",
            "full_schema_dense_fill_enumerate": "test-only array with N equal to full FIX44 field count (912); reset/fill/scan all N slots for the same synthetic payload. It is a dense cost reference, not a competing implementation.",
            "active_index_sparse_fill_enumerate": "test-only K-slot payload plus sorted active indexes; includes descending insertion/sort work and scans K active entries. No formatting or validation.",
            "direct_bounded_ordered_writer": "complete wire writer for one fixed seven-field NewOrderSingle layout; checks output bounds and is byte-compared to generated encoder, but intentionally omits runtime schema/type/enum/required-field validation. Not an equivalent-speedup claim.",
            "same_payload_comparison": "the fixed NewOrderSingle payload with tags 11,21,55,54,60,38,40 is also fed to shape-local, N=912 dense, and active-index references. Those three rows measure fill/enumeration only; the direct writer measures complete framing and checksum.",
            "generated_build_encode": "generated FIX44 NewOrderSingle typed builder plus production encoder, including ordinary validation and complete frame formatting.",
            "dynamic_runtime_tags_build_encode": "explicit DynamicMessage construction with seven runtime tags, owned values, and production frame encoding; byte-compared with generated NewOrderSingle, but no dictionary enum/required-field validation. Reports its own heap allocation cost.",
            "dynamic_prebuilt_encode_only": "production encoding of a previously built DynamicMessage; excludes initial runtime plan/value allocation and does not include schema validation.",
            "typed_parse_strict": "inspect/flat parse, strict full-schema validation, and generated typed getters.",
            "group_roundtrip": "generated ExecutionReport and owned PartyIDs group build, production encode, parse, strict validation, and typed group projection.",
        },
        "validation_before_timing": [
            "generated direct writer output byte-for-byte equals production encoder for the fixed NewOrderSingle payload",
            "direct writer and production encoder both reject too-small output spans",
            "production encoder rejects missing required fields and an invalid Side enum",
            "production frame parser rejects a changed checksum",
            "each synthetic reference field sequence matches the fields parsed from production encoder output",
            "group roundtrip verifies three PartyIDs and strict FIX44 schema validation",
        ],
        "workload": {
            "samples_per_case": args.samples,
            "runs": args.runs,
            "warmup_per_case": 1000,
            "working_set": args.workset,
            "hot_case": "one seven-field generated NewOrderSingle payload",
            "rotating_case": "generated NewOrderSingle IDs and quantities rotated over the configured input working set",
            "shape_matrix": ["S=8,K=2,B=8", "S=8,K=8,B=8", "S=32,K=2,B=8",
                "S=32,K=8,B=64", "S=32,K=32,B=128"],
            "dense_N": "full FIX44 field_count (912) in each synthetic dense-reference case",
        },
        "storage": "none; stateless codec/schema microbenchmark",
        "limitations": [
            "slot references are intentionally simple test-only candidates and do not reproduce production ownership or validation",
            "benchmark numbers are local user-space costs, not end-to-end session/network latency",
            "shape matrix uses synthetic tags 1000+index and generic string values; its encoder wire is only an equivalence oracle for ordering and field preservation",
        ],
        "runs_data": [],
    }

    for run_number in range(1, args.runs + 1):
        result = run_benchmark(binary, args.samples, args.workset)
        result.wall_ns = sum(item["wall_ns"] for item in result.operations.values())
        filename, summaries = summarize(result, args.output, run_number)
        metadata["runs_data"].append({
            "raw_samples": filename,
            "schema_counts_reported_by_binary": result.schema,
            "cases": summaries,
        })

    if not args.skip_size_probe:
        metadata["schema_size_probe"] = run_size_probe(args.compiler, args.output)
    if args.standard_schemas:
        metadata["standard_xml_generation_and_compile"] = run_standard_matrix(args.compiler, args.compile_timeout)
    metadata["clang_template_instantiation_proxy"] = run_clang_template_proxy(
        args.clang, generated_header)
    metadata["perf_event"] = {"status": "skipped", "reason": "--skip-perf"} if args.skip_perf else \
        collect_perf(binary, min(args.samples, 5000), args.workset, args.output)
    report = args.output / "report.json"
    report.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")

    first_run = metadata["runs_data"][0]["cases"]
    for case in first_run:
        data = [run["cases"][case] for run in metadata["runs_data"]]
        print(f"{case}: median-run p50={statistics.median(row['p50_ns'] for row in data):g}ns "
              f"p99={statistics.median(row['p99_ns'] for row in data):g}ns "
              f"allocs/op={statistics.median(row['allocation_calls_per_operation'] for row in data):g}")
    print(f"report: {report}")


if __name__ == "__main__":
    main()
