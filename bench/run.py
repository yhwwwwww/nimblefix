#!/usr/bin/env python3
"""Save raw codec samples and reproducible environment/summary information."""
import argparse
import csv
import datetime
import gzip
import hashlib
import io
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def output(*command):
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    return result.stdout.strip() if result.returncode == 0 else "unavailable"


def source_digest():
    digest = hashlib.sha256()
    paths = [ROOT / "xmake.lua", ROOT / "CMakeLists.txt"]
    for directory in ("include", "src", "schemas", "tools/schema", "bench"):
        paths.extend(path for path in (ROOT / directory).rglob("*")
            if path.suffix in (".hpp", ".cpp", ".xml", ".py") and "results" not in path.parts)
    for path in sorted(paths):
        digest.update(str(path.relative_to(ROOT)).encode() + b"\0" + path.read_bytes())
    return digest.hexdigest()


def percentile(values, quantile):
    return values[max(0, math.ceil(len(values) * quantile) - 1)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=10000)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--flags", default="xmake mode.release (-O3); C++20; --wrap=malloc")
    args = parser.parse_args()
    if not 100 <= args.samples <= 1000000 or not 1 <= args.runs <= 20:
        parser.error("samples must be 100..1000000 and runs must be 1..20")
    args.output.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    metadata = {
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "revision": output("git", "rev-parse", "HEAD"),
        "worktree_dirty": bool(output("git", "status", "--porcelain")),
        "source_sha256": source_digest(),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "binary_bytes": binary.stat().st_size,
        "compiler": output(args.compiler, "--version").splitlines()[0],
        "build_flags": args.flags,
        "kernel": platform.platform(),
        "cpu": output("lscpu"),
        "cpu_affinity": sorted(os.sched_getaffinity(0)),
        "cpu_pinned": len(os.sched_getaffinity(0)) == 1,
        "timing": "steady_clock user-space operation duration; includes clock/hook overhead",
        "allocation_scope": "linker-wrapped direct malloc and overridden C++ new; excludes shared-library internal allocations and aligned new",
        "boundary": "L1 codec only; no session, store, network, syscall tracing, or locks measured",
        "dispatch": "none", "schema": "flat and two-message FIX44 XML subset",
        "workload": "hot seven-field order plus controlled S/K/B matrix and 8192-message rotating working set; 1000 warmup per case; timestamp fixed",
        "storage": "none (stateless codec)",
        "runs": [],
    }
    for run in range(args.runs):
        result = subprocess.run([str(binary), str(args.samples)], text=True, capture_output=True, check=True)
        rows = list(csv.DictReader(io.StringIO(result.stdout)))
        filename = f"run-{run + 1}.csv.gz"
        with (args.output / filename).open("wb") as raw:
            with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
                compressed.write(result.stdout.encode())
        operations = {row["case"]: row for row in map(json.loads, result.stderr.splitlines())}
        grouped = {}
        for row in rows:
            grouped.setdefault(row["case"], []).append(row)
        summaries = {}
        for case, samples in grouped.items():
            latencies = sorted(int(row["elapsed_ns"]) for row in samples)
            info = dict(operations[case])
            info.update({"p50_ns": percentile(latencies, .5), "p99_ns": percentile(latencies, .99),
                "p999_ns": percentile(latencies, .999), "max_ns": max(latencies),
                "loop_ops_per_second": len(samples) * 1e9 / info["wall_ns"],
                "allocation_calls_per_operation": statistics.mean(int(row["allocation_calls"]) for row in samples),
                "allocation_bytes_per_operation": statistics.mean(int(row["allocation_bytes"]) for row in samples)})
            if (case in {"fixed_encode", "fixed_build_encode", "frame_flat_parse", "rotating_8192_fixed_encode"} or case.startswith("shape_")) and info["allocation_calls_per_operation"] != 0:
                raise RuntimeError(f"{case} unexpectedly allocated")
            if len(samples) >= 100000:
                info["p9999_ns"] = percentile(latencies, .9999)
            summaries[case] = info
        metadata["runs"].append({"raw_samples": filename, "cases": summaries})
    (args.output / "report.json").write_text(json.dumps(metadata, indent=2) + "\n")
    for case in metadata["runs"][0]["cases"]:
        data = [run["cases"][case] for run in metadata["runs"]]
        print(f"{case}: median-run p50={statistics.median(row['p50_ns'] for row in data):g}ns "
              f"p99={statistics.median(row['p99_ns'] for row in data):g}ns "
              f"allocs/op={statistics.median(row['allocation_calls_per_operation'] for row in data):g}")


if __name__ == "__main__":
    main()
