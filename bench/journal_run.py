#!/usr/bin/env python3
"""Measure v2 Journal append, independent reader, and checkpoint reopen costs."""

import argparse
import csv
import datetime
import hashlib
import io
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile

from run import output, source_digest


ROOT = Path(__file__).resolve().parents[1]
CSV_COLUMNS = (
    "operation", "iteration", "kind", "elapsed_ns", "record_id", "segment_index",
    "payload_bytes", "reader_lag_records", "committed_records", "minor_faults", "major_faults",
)


def percentile(values, quantile):
    return values[max(0, math.ceil(len(values) * quantile) - 1)]


def summarize(values):
    ordered = sorted(values)
    return {
        "samples": len(ordered),
        "p50_ns": percentile(ordered, 0.50),
        "p99_ns": percentile(ordered, 0.99),
        "p999_ns": percentile(ordered, 0.999),
        "max_ns": max(ordered),
        "mean_ns": statistics.fmean(ordered),
    }


def filesystem_type(path):
    result = subprocess.run(["stat", "-f", "-c", "%T", str(path)], text=True,
        capture_output=True, check=True)
    return result.stdout.strip()


def parse_rows(stdout, expected_samples, expected_reopens):
    rows = list(csv.DictReader(io.StringIO(stdout)))
    if not rows or tuple(rows[0].keys()) != CSV_COLUMNS:
        raise RuntimeError("benchmark emitted an unexpected or empty CSV schema")
    grouped = {}
    for row in rows:
        grouped.setdefault((row["operation"], row["kind"]), []).append(row)
    for kind in ("inbound", "outbound", "delivery"):
        if len(grouped.get(("append", kind), ())) != expected_samples:
            raise RuntimeError(f"expected {expected_samples} append rows for {kind}")
        if len(grouped.get(("reader_next", kind), ())) != expected_samples:
            raise RuntimeError(f"expected {expected_samples} reader rows for {kind}")
    if len(grouped.get(("reopen", "normal_checkpoint_index_recovery"), ())) != expected_reopens:
        raise RuntimeError("normal reopen sample count mismatch")
    if len(grouped.get(("fresh_open", "new_store"), ())) != 1:
        raise RuntimeError("fresh open measurement missing")
    if len(grouped.get(("checkpoint", "checkpoint_and_outbound_index"), ())) != 1:
        raise RuntimeError("checkpoint measurement missing")
    return rows, grouped


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=2000,
        help="inbound/outbound/delivery triplets per run (10..100000)")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--reopens", type=int, default=3,
        help="normal checkpoint/index reopen samples per run (1..50)")
    parser.add_argument("--segment-bytes", type=int, default=65536)
    parser.add_argument("--max-segments", type=int, default=128)
    parser.add_argument("--payload-bytes", type=int, default=128)
    parser.add_argument("--state-parent", type=Path, default=ROOT / "build/bench-state")
    parser.add_argument("--require-filesystem",
        help="optional filesystem gate, for example btrfs")
    parser.add_argument("--compiler", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--flags", default="g++ -std=c++20 -O3 -DNDEBUG -Iinclude -pthread; v2 appended mmap Journal")
    parser.add_argument("--timeout", type=float, default=180.0)
    args = parser.parse_args()

    if not 10 <= args.samples <= 100000:
        parser.error("--samples must be 10..100000")
    if not 1 <= args.runs <= 20:
        parser.error("--runs must be 1..20")
    if not 1 <= args.reopens <= 50:
        parser.error("--reopens must be 1..50")
    if not 8192 <= args.segment_bytes <= (1 << 24) or args.segment_bytes % 8:
        parser.error("--segment-bytes must be 8-byte aligned and in 8192..16777216")
    if not 2 <= args.max_segments <= 4096:
        parser.error("--max-segments must be 2..4096")
    if not 16 <= args.payload_bytes <= 2048:
        parser.error("--payload-bytes must be 16..2048")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"benchmark binary does not exist: {binary}")
    output_dir = args.output.resolve()
    state_parent = args.state_parent.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    state_parent.mkdir(parents=True, exist_ok=True)
    filesystem = filesystem_type(state_parent)
    if args.require_filesystem and filesystem != args.require_filesystem:
        parser.error(f"requested filesystem {args.require_filesystem!r}, found {filesystem!r}")

    affinity = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    metadata = {
        "benchmark": "journal-v2",
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
        "cpu_affinity": affinity,
        "cpu_pinned": len(affinity) == 1,
        "boundary": "real v2 Journal API: append commit publication, independent JournalReader::next, normal close/open recovery",
        "timing": "steady_clock duration around one API operation; append rows exclude CSV serialization and do not request per-record sync",
        "resource_scope": "phase page-fault deltas and process peak RSS include benchmark bookkeeping and preallocated sample buffers; reader_next timing excludes the subsequent borrowed-payload copy",
        "page_faults": "process rusage deltas for fresh open, append phase, reader phase, checkpoint publish, and each normal reopen; CSV per-row fault columns are blank where syscall sampling was intentionally excluded from the measured loop",
        "storage": {
            "filesystem": filesystem,
            "state_parent": str(state_parent),
            "temporary_state_under_state_parent": True,
            "segment_bytes": args.segment_bytes,
            "max_segments": args.max_segments,
            "explicit_per_record_sync": False,
            "os_page_cache_cleared": False,
            "power_loss_simulated": False,
        },
        "workload": {
            "samples_per_run": args.samples,
            "runs": args.runs,
            "normal_reopens_per_run": args.reopens,
            "payload_bytes": args.payload_bytes,
            "records_per_sample": ["inbound", "outbound", "delivery"],
            "reader": "one independent reader created before append; borrowed payload copied before the next call",
            "checkpoint": "publish final delivery cursor and replay cursor, then close and reopen the same journal repeatedly",
            "cold_index_rebuild": "not measured",
        },
        "runs": [],
    }

    for run_index in range(args.runs):
        with tempfile.TemporaryDirectory(prefix="nimblefix-journal-", dir=state_parent) as state_dir:
            command = [str(binary), state_dir, str(args.samples), str(args.reopens),
                str(args.segment_bytes), str(args.max_segments), str(args.payload_bytes)]
            result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True,
                timeout=args.timeout)
            if result.returncode:
                raise RuntimeError(f"journal benchmark exited {result.returncode}: {result.stderr.strip()}")
            rows, grouped = parse_rows(result.stdout, args.samples, args.reopens)
            try:
                measured = json.loads(result.stderr)
            except json.JSONDecodeError as error:
                raise RuntimeError(f"benchmark stderr is not its JSON summary: {result.stderr!r}") from error
            expected_records = args.samples * 3
            if measured.get("format_version") != 2 or measured.get("records_committed") != expected_records:
                raise RuntimeError("run did not use v2 Journal with all records committed")
            if measured.get("checkpoint_stale") or measured.get("checkpoint_index_recovered") is not True:
                raise RuntimeError("checkpoint/index normal recovery verification failed")
            if measured.get("reader_peak_lag_records") != expected_records or measured.get("reader_final_lag_records") != 0:
                raise RuntimeError("reader lag workload did not drain as expected")
            if measured.get("segment_index_transitions", 0) < 1 or len(measured.get("segment_indices", [])) < 2:
                raise RuntimeError("workload did not rotate through multiple segment indices")
            if measured.get("per_message_syncs_requested") != 0:
                raise RuntimeError("benchmark unexpectedly requested per-message sync")

            filename = f"run-{run_index + 1}.csv"
            (output_dir / filename).write_text(result.stdout)
            summaries = {}
            for (operation, kind), samples in sorted(grouped.items()):
                summaries[f"{operation}/{kind}"] = summarize([int(row["elapsed_ns"]) for row in samples])
            measured["raw_samples"] = filename
            measured["timings"] = summaries
            measured["csv_rows"] = len(rows)
            metadata["runs"].append(measured)

    report_path = output_dir / "report.json"
    report_path.write_text(json.dumps(metadata, indent=2) + "\n")
    first_run = metadata["runs"][0]
    print(f"journal-v2: filesystem={filesystem} committed={first_run['records_committed']} "
        f"segments={first_run['segment_index_count']} rotations={first_run['segment_index_transitions']} "
        f"peak_lag={first_run['reader_peak_lag_records']} "
        f"reopens={args.reopens} report={report_path}")


if __name__ == "__main__":
    main()
