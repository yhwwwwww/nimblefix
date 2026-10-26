#!/usr/bin/env python3
"""Independent open-loop FIX load generator; save scheduling-to-reply latency."""
import argparse
import csv
import datetime
import json
import math
from pathlib import Path
import selectors
import socket
import subprocess
import tempfile
import time
import platform
import os
import hashlib
from run import output, source_digest


def frame(seq, msgtype, fields=()):
    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H:%M:%S.%f")[:-3]
    body = f"35={msgtype}\x0149=BUY\x0156=SELL\x0134={seq}\x0152={timestamp}\x01".encode()
    body += b"".join(f"{tag}={value}\x01".encode() for tag, value in fields)
    prefix = b"8=FIX.4.4\x019=" + str(len(body)).encode() + b"\x01" + body
    return prefix + f"10={sum(prefix)%256:03d}\x01".encode()


def extract(buffer):
    messages = []
    while buffer:
        first = buffer.find(b"\x01")
        second = buffer.find(b"\x01", first + 1)
        if first < 0 or second < 0: break
        length = int(buffer[first + 3:second])
        end = second + 1 + length + 7
        if len(buffer) < end: break
        raw, buffer = buffer[:end], buffer[end:]
        if sum(raw[:-7]) % 256 != int(raw[-4:-1]): raise RuntimeError("invalid peer checksum")
        messages.append(dict(field.split(b"=", 1) for field in raw[:-1].split(b"\x01")))
    return messages, buffer


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=2000)
    parser.add_argument("--rate", type=int, default=2000)
    parser.add_argument("--echo", action="store_true", help="measure the raw byte-echo baseline")
    parser.add_argument("--state-parent", type=Path, default=Path("build/bench-state"))
    args = parser.parse_args()
    if not 1 <= args.samples <= 20000 or not 1 <= args.rate <= 100000:
        parser.error("samples 1..20000, rate 1..100000 required")
    args.output.mkdir(parents=True, exist_ok=True)
    args.state_parent.mkdir(parents=True, exist_ok=True)
    filesystem = subprocess.check_output(["stat", "-f", "-c", "%T", str(args.state_parent)], text=True).strip()
    with tempfile.TemporaryDirectory(prefix="nimblefix-tcp-bench-", dir=args.state_parent.resolve()) as state:
        child = subprocess.Popen([str(args.binary.resolve()), state], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            startup = selectors.DefaultSelector()
            startup.register(child.stdout, selectors.EVENT_READ)
            if not startup.select(10): raise RuntimeError("server startup timed out")
            port = int(child.stdout.readline())
            startup.close()
            with socket.create_connection(("127.0.0.1", port), timeout=5) as peer:
                peer.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                peer.sendall(frame(1, "A", ((98, 0), (108, 30))))
                buffer = b""
                while True:
                    chunk = peer.recv(65536)
                    if not chunk: raise RuntimeError("peer closed before Logon")
                    messages, buffer = extract(buffer + chunk)
                    if any(message[b"35"] == b"A" for message in messages): break
                peer.setblocking(False)
                poller = selectors.DefaultSelector()
                poller.register(peer, selectors.EVENT_READ | selectors.EVENT_WRITE)
                start = time.monotonic_ns() + 10000000
                step = 1000000000 // args.rate
                pending = bytearray()
                samples, sent, received, seq = [], 0, 0, 2
                next_peer_seq = 2
                deadline = time.monotonic() + max(10, args.samples / args.rate + 5)
                while received < args.samples and time.monotonic() < deadline:
                    now = time.monotonic_ns()
                    while sent < args.samples and now >= start + sent * step and len(pending) < 1048576:
                        pending += frame(seq, "D", ((11, sent), (55, "AAPL"), (54, 1), (38, 100), (40, 1)))
                        sent += 1; seq += 1
                    poller.modify(peer, selectors.EVENT_READ | (selectors.EVENT_WRITE if pending else 0))
                    timeout = min(.01, max(0, (start + sent * step - now) / 1e9)) if sent < args.samples else .01
                    for key, events in poller.select(timeout):
                        if events & selectors.EVENT_WRITE and pending:
                            try:
                                written = peer.send(pending)
                                del pending[:written]
                            except BlockingIOError: pass
                        if events & selectors.EVENT_READ:
                            try: chunk = peer.recv(65536)
                            except BlockingIOError: continue
                            if not chunk: raise RuntimeError("session closed during load")
                            messages, buffer = extract(buffer + chunk)
                            observed = time.monotonic_ns()
                            for message in messages:
                                if int(message[b"34"]) != next_peer_seq: raise RuntimeError("out-of-order FIX response")
                                next_peer_seq += 1
                                if message[b"35"] != (b"D" if args.echo else b"8"): continue
                                identifier = int(message[b"11"])
                                samples.append((identifier, observed - (start + identifier * step)))
                                received += 1
                if received != args.samples or len({row[0] for row in samples}) != args.samples:
                    raise RuntimeError(f"incomplete/duplicate replies: {received}/{args.samples}")
                elapsed_ns = time.monotonic_ns() - start
                peer.setblocking(True); peer.settimeout(2)
                peer.sendall(frame(seq, "5"))
                while True:
                    chunk = peer.recv(65536)
                    if not chunk: break
                    messages, buffer = extract(buffer + chunk)
                    if any(message[b"35"] == b"5" for message in messages): break
            with (args.output / "samples.csv").open("w") as destination:
                writer = csv.writer(destination); writer.writerow(("id", "scheduled_to_reply_ns")); writer.writerows(samples)
            latencies = sorted(row[1] for row in samples)
            report = {"boundary":("L0 minimal nonblocking raw TCP byte echo; no codec/session/store" if args.echo else "L3 loopback, independent Python open-loop schedule to parsed reply, appended mmap journal, inline"),
                "samples":args.samples,"offered_rate":args.rate,"received":received,
                "observed_rate":received*1e9/elapsed_ns,"warmup_business_messages":0,
                "p50_ns":latencies[math.ceil(len(latencies)*.5)-1],"p99_ns":latencies[math.ceil(len(latencies)*.99)-1],
                "max_ns":max(latencies),"limitations":"development host; Python framing and scheduling included; no wire timestamps"}
            report.update({"state_filesystem":filesystem,"kernel":platform.platform(),
                "cpu_affinity":sorted(os.sched_getaffinity(0)),"cpu_pinned":len(os.sched_getaffinity(0))==1,
                "code_revision":subprocess.check_output(["git","rev-parse","HEAD"],text=True).strip(),
                "binary_sha256":hashlib.sha256(args.binary.read_bytes()).hexdigest(),
                "source_sha256":source_digest(),"cpu":output("lscpu"),
                "compiler":output("g++","--version").splitlines()[0],
                "build_flags":"xmake mode.release (-O3), C++20",
                "tcp_nodelay":True,"clock":"Python monotonic_ns; scheduled arrival to complete parsed reply"})
        finally:
            child.terminate()
            try: stdout, stderr = child.communicate(timeout=10)
            except subprocess.TimeoutExpired: child.kill(); stdout, stderr = child.communicate()
            if child.returncode: raise RuntimeError(f"server exit={child.returncode}: {stderr}")
        if not args.echo:
            counters = json.loads(stdout.strip().splitlines()[-1])
            if counters["errors"] or counters["delivered"] != args.samples:
                raise RuntimeError(f"server callback/error mismatch: {counters}")
            report["server_counters"] = counters
        (args.output / "report.json").write_text(json.dumps(report,indent=2)+"\n")
        print(json.dumps(report))


if __name__ == "__main__": main()
