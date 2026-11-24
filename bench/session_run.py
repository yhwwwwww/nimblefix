#!/usr/bin/env python3
"""Preserve the two-driver mmap session microbenchmark and its sync/allocation checks."""
import argparse
import csv
import gzip
import hashlib
import io
import json
import math
import os
from pathlib import Path
import platform
import subprocess
from run import output, source_digest


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--binary",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--state-parent",type=Path,default=Path("build/bench-state"))
    parser.add_argument("--samples",type=int,default=2000)
    parser.add_argument("--runs",type=int,default=3)
    args=parser.parse_args()
    if not 100<=args.samples<=10000 or not 1<=args.runs<=10: parser.error("samples 100..10000; runs 1..10")
    args.output.mkdir(parents=True,exist_ok=True);args.state_parent.mkdir(parents=True,exist_ok=True)
    binary=args.binary.resolve()
    report={"boundary":"L2 two real SessionDrivers + appended mmap journals + inline callbacks + fake transport round trip",
        "revision":output("git","rev-parse","HEAD"),
        "binary_sha256":hashlib.sha256(binary.read_bytes()).hexdigest(),
        "source_sha256":source_digest(),"compiler":output(os.environ.get("CXX","g++"),"--version").splitlines()[0],
        "build_flags":"xmake mode.release (-O3), C++20; wrapped malloc, fsync, fdatasync, msync",
        "cpu":output("lscpu"),
        "kernel":platform.platform(),"cpu_affinity":sorted(os.sched_getaffinity(0)),
        "cpu_pinned":len(os.sched_getaffinity(0))==1,
        "filesystem":subprocess.check_output(["stat","-f","-c","%T",str(args.state_parent)],text=True).strip(),
        "confirmation":"appended; no fsync/fdatasync/msync; process-crash recovery only",
        "workload":"fixed one-field message, two callbacks, six journal records per round trip; 100 warmup operations",
        "limitations":"hot development-host microbenchmark; two user-space clock calls; not network latency; no power-loss guarantee",
        "runs":[]}
    for index in range(args.runs):
        result=subprocess.run([str(binary),str(args.samples),str(args.state_parent.resolve())],text=True,capture_output=True,check=True)
        rows=list(csv.DictReader(io.StringIO(result.stdout)))
        assert len(rows)==args.samples
        values=sorted(int(row["round_trip_ns"]) for row in rows)
        assert all(int(row["allocation_calls"])==0 and int(row["sync_calls"])==0 for row in rows)
        filename=f"run-{index+1}.csv.gz"
        with (args.output/filename).open("wb") as raw:
            with gzip.GzipFile(filename="",mode="wb",fileobj=raw,mtime=0) as stream:stream.write(result.stdout.encode())
        measured=json.loads(result.stderr)
        measured.update({"raw":filename,"samples":len(rows),"p50_ns":values[math.ceil(len(values)*.5)-1],
            "p99_ns":values[math.ceil(len(values)*.99)-1],"p999_ns":values[math.ceil(len(values)*.999)-1],"max_ns":max(values)})
        report["runs"].append(measured)
    (args.output/"report.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps(report))


if __name__=="__main__":main()
