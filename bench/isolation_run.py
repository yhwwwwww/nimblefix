#!/usr/bin/env python3
"""Exercise slow-reader isolation through two real FIX acceptor sessions."""

import argparse
import csv
import datetime
import hashlib
import json
import math
from pathlib import Path
import platform
import selectors
import signal
import socket
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
SOH = b"\x01"
SLOW_MESSAGES = 256
SLOW_PAYLOAD_BYTES = 50 * 1024
SLOW_STALL_MS = 2500
SLOW_LOGOUT_TIMEOUT_MS = 750


def make_frame(seq, msg_type, sender, target, fields=()):
    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y%m%d-%H:%M:%S.%f")[:-3]
    body = (f"35={msg_type}\x0149={sender}\x0156={target}\x0134={seq}\x0152={timestamp}\x01").encode()
    body += b"".join(str(tag).encode() + b"=" + str(value).encode() + SOH
        for tag, value in fields)
    prefix = b"8=FIX.4.4\x019=" + str(len(body)).encode() + SOH + body
    return prefix + f"10={sum(prefix) % 256:03d}".encode() + SOH


def extract_frames(buffer):
    frames = []
    while buffer:
        first = buffer.find(SOH)
        if first < 0:
            break
        second = buffer.find(SOH, first + 1)
        if second < 0:
            break
        if buffer[:first] != b"8=FIX.4.4" or buffer[first + 1:first + 3] != b"9=":
            raise RuntimeError("invalid FIX BeginString or BodyLength header")
        try:
            body_length = int(buffer[first + 3:second])
        except ValueError as error:
            raise RuntimeError("invalid FIX BodyLength") from error
        checksum_start = second + 1 + body_length
        frame_end = checksum_start + 7
        if len(buffer) < frame_end:
            break
        raw, buffer = buffer[:frame_end], buffer[frame_end:]
        if raw[checksum_start:checksum_start + 3] != b"10=":
            raise RuntimeError("invalid FIX CheckSum field")
        try:
            expected_checksum = int(raw[checksum_start + 3:checksum_start + 6])
        except ValueError as error:
            raise RuntimeError("invalid FIX CheckSum value") from error
        if sum(raw[:checksum_start]) % 256 != expected_checksum:
            raise RuntimeError("invalid FIX checksum")
        fields = {}
        for part in raw[:-1].split(SOH):
            if b"=" not in part:
                raise RuntimeError("malformed FIX field")
            tag, value = part.split(b"=", 1)
            fields[tag] = value
        frames.append((raw, fields))
    return frames, buffer


def recv_frames(sock, buffer, timeout):
    sock.settimeout(timeout)
    while True:
        frames, buffer = extract_frames(buffer)
        if frames:
            return frames, buffer
        chunk = sock.recv(65536)
        if not chunk:
            raise RuntimeError("FIX peer closed the connection")
        buffer += chunk


class Peer:
    def __init__(self, port, sender):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sender = sender
        self.target = "SELL"
        self.buffer = b""
        self.next_client_seq = 1
        self.next_server_seq = 1

    def send(self, msg_type, fields=()):
        data = make_frame(self.next_client_seq, msg_type, self.sender, self.target, fields)
        self.next_client_seq += 1
        self.sock.sendall(data)

    def logon(self):
        self.send("A", ((98, 0), (108, 30)))
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            frames, self.buffer = recv_frames(self.sock, self.buffer,
                max(0.01, deadline - time.monotonic()))
            for _, fields in frames:
                seq = int(fields[b"34"])
                if seq != self.next_server_seq:
                    raise RuntimeError(f"{self.sender} Logon response sequence {seq}, "
                        f"expected {self.next_server_seq}")
                self.next_server_seq += 1
                if fields.get(b"35") == b"A":
                    return
                if fields.get(b"35") not in (b"0", b"1"):
                    raise RuntimeError(f"unexpected {self.sender} Logon response type "
                        f"{fields.get(b'35')!r}")
        raise RuntimeError(f"{self.sender} Logon timed out")

    def logout(self):
        self.send("5")
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            frames, self.buffer = recv_frames(self.sock, self.buffer,
                max(0.01, deadline - time.monotonic()))
            for _, fields in frames:
                seq = int(fields[b"34"])
                if seq != self.next_server_seq:
                    raise RuntimeError(f"{self.sender} Logout response sequence {seq}, "
                        f"expected {self.next_server_seq}")
                self.next_server_seq += 1
                msg_type = fields.get(b"35")
                if msg_type == b"5":
                    return
                if msg_type == b"1":
                    test_req_id = fields.get(b"112", b"").decode()
                    self.send("0", ((112, test_req_id),) if test_req_id else ())
                elif msg_type != b"0":
                    raise RuntimeError(f"unexpected message during Logout: {msg_type!r}")
        raise RuntimeError(f"{self.sender} Logout timed out")


def read_process_memory_kib(pid):
    current = high_water = None
    try:
        with open(f"/proc/{pid}/status", encoding="ascii") as status:
            for line in status:
                if line.startswith("VmRSS:"):
                    current = int(line.split()[1])
                elif line.startswith("VmHWM:"):
                    high_water = int(line.split()[1])
    except (OSError, ValueError):
        pass
    return current, high_water


def update_memory(memory, pid):
    current, high_water = read_process_memory_kib(pid)
    if current is not None:
        memory["rss_kib_max_observed"] = max(memory["rss_kib_max_observed"], current)
    if high_water is not None:
        memory["vmhwm_kib_max_observed"] = max(memory["vmhwm_kib_max_observed"], high_water)


def percentile(values, quantile):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * quantile) - 1)]


def healthy_phase(peer, samples, rate, child_pid, memory):
    peer.sock.setblocking(False)
    selector = selectors.DefaultSelector()
    pending = bytearray()
    sent = received = 0
    seen = set()
    timings = {}
    rows = []
    start_ns = time.monotonic_ns() + 10_000_000
    interval_ns = max(1, 1_000_000_000 // rate)
    deadline = time.monotonic() + max(30, samples / rate * 4 + 10)
    try:
        while received < samples:
            now_ns = time.monotonic_ns()
            while sent < samples and now_ns >= start_ns + sent * interval_ns:
                identifier = f"healthy-{sent}"
                scheduled_ns = start_ns + sent * interval_ns
                queued_ns = now_ns
                timings[identifier] = (scheduled_ns, queued_ns)
                pending.extend(make_frame(peer.next_client_seq, "D", peer.sender,
                    peer.target, ((11, identifier),)))
                peer.next_client_seq += 1
                sent += 1
            if time.monotonic() >= deadline:
                raise RuntimeError(f"healthy timed out after {received}/{samples} replies")
            events = selectors.EVENT_READ | (selectors.EVENT_WRITE if pending else 0)
            selector.register(peer.sock, events)
            next_due = start_ns + sent * interval_ns if sent < samples else now_ns + 1_000_000
            timeout = min(0.01, max(0, (next_due - time.monotonic_ns()) / 1e9))
            ready = selector.select(timeout)
            selector.unregister(peer.sock)
            for _, mask in ready:
                if mask & selectors.EVENT_WRITE and pending:
                    try:
                        amount = peer.sock.send(pending)
                        if amount:
                            del pending[:amount]
                    except BlockingIOError:
                        pass
                if mask & selectors.EVENT_READ:
                    try:
                        chunk = peer.sock.recv(65536)
                    except BlockingIOError:
                        continue
                    if not chunk:
                        raise RuntimeError("healthy session closed during traffic")
                    peer.buffer += chunk
                    frames, peer.buffer = extract_frames(peer.buffer)
                    observed_ns = time.monotonic_ns()
                    for _, fields in frames:
                        seq = int(fields[b"34"])
                        if seq != peer.next_server_seq:
                            raise RuntimeError(f"healthy response sequence {seq}, "
                                f"expected {peer.next_server_seq}")
                        peer.next_server_seq += 1
                        msg_type = fields.get(b"35")
                        if msg_type == b"8":
                            identifier = fields[b"11"].decode()
                            if identifier not in timings or identifier in seen:
                                raise RuntimeError(f"unexpected/duplicate healthy reply {identifier}")
                            scheduled_ns, queued_ns = timings[identifier]
                            seen.add(identifier)
                            rows.append({
                                "id": identifier,
                                "scheduled_ns": scheduled_ns,
                                "client_send_queued_ns": queued_ns,
                                "reply_observed_ns": observed_ns,
                                "scheduled_to_reply_ns": observed_ns - scheduled_ns,
                                "queue_to_reply_ns": observed_ns - queued_ns,
                            })
                            received += 1
                        elif msg_type == b"1":
                            test_req_id = fields.get(b"112", b"").decode()
                            hb_fields = ((112, test_req_id),) if test_req_id else ()
                            pending.extend(make_frame(peer.next_client_seq, "0", peer.sender,
                                peer.target, hb_fields))
                            peer.next_client_seq += 1
                        elif msg_type != b"0":
                            raise RuntimeError(f"unexpected healthy message type {msg_type!r}")
            update_memory(memory, child_pid)
        if len(seen) != samples or sent != samples:
            raise RuntimeError(f"healthy sample mismatch: sent={sent}, seen={len(seen)}")
        if pending:
            peer.sock.setblocking(True)
            peer.sock.sendall(pending)
            pending.clear()
        elapsed_ns = max(1, max(row["reply_observed_ns"] for row in rows) - start_ns)
        return rows, elapsed_ns
    finally:
        selector.close()
        peer.sock.setblocking(True)


def wait_startup(child, timeout=15):
    selector = selectors.DefaultSelector()
    selector.register(child.stdout, selectors.EVENT_READ)
    try:
        if not selector.select(timeout):
            raise RuntimeError("isolation server startup timed out")
        line = child.stdout.readline().strip()
        try:
            port = int(line)
        except ValueError as error:
            raise RuntimeError(f"invalid startup port line {line!r}") from error
        if not 1 <= port <= 65535:
            raise RuntimeError(f"invalid startup port {port}")
        return port
    finally:
        selector.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--state-parent", type=Path, default=ROOT / "build/bench-state")
    parser.add_argument("--samples", type=int, default=1000)
    parser.add_argument("--rate", type=int, default=1000)
    args = parser.parse_args()
    if not 1 <= args.samples <= 20000 or not 1 <= args.rate <= 100000:
        parser.error("samples must be 1..20000 and rate 1..100000")

    args.output.mkdir(parents=True, exist_ok=True)
    args.state_parent.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    memory = {"rss_kib_max_observed": 0, "vmhwm_kib_max_observed": 0}
    child = None
    slow = healthy = None
    server_report = None
    try:
        with tempfile.TemporaryDirectory(prefix="nimblefix-isolation-",
                dir=args.state_parent.resolve()) as state_directory:
            child = subprocess.Popen([str(binary), state_directory], stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
            port = wait_startup(child)

            slow = Peer(port, "LAG")
            slow.logon()
            healthy = Peer(port, "BUY")
            healthy.logon()

            # The slow peer deliberately reads only its Logon. It then submits
            # enough small orders to make the server queue large 50 KiB replies.
            slow_batch = bytearray()
            for index in range(SLOW_MESSAGES):
                slow_batch.extend(make_frame(slow.next_client_seq, "D", slow.sender,
                    slow.target, ((11, f"slow-{index}"),)))
                slow.next_client_seq += 1
            slow.sock.sendall(slow_batch)
            settle_until = time.monotonic() + 0.2
            while time.monotonic() < settle_until:
                update_memory(memory, child.pid)
                time.sleep(0.005)

            rows, elapsed_ns = healthy_phase(healthy, args.samples, args.rate,
                child.pid, memory)
            healthy.logout()

            stall_until = time.monotonic() + (SLOW_STALL_MS + SLOW_LOGOUT_TIMEOUT_MS + 500) / 1000
            while time.monotonic() < stall_until:
                update_memory(memory, child.pid)
                time.sleep(0.01)

            # Closing this one unread socket after measurement keeps server
            # shutdown independent of its blocked output; its queue-full and
            # pending high-water evidence was already captured above.
            slow.sock.close()
            slow = None
            healthy.sock.close()
            healthy = None
            child.send_signal(signal.SIGTERM)
            try:
                stdout, stderr = child.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                child.kill()
                stdout, stderr = child.communicate()
                raise RuntimeError("isolation server failed to stop gracefully")
            if child.returncode:
                raise RuntimeError(f"isolation server exit={child.returncode}: {stderr[-4000:]}")
            lines = stdout.strip().splitlines()
            if not lines:
                raise RuntimeError(f"missing server summary; stdout={stdout!r}; stderr={stderr[-4000:]}")
            server_report = json.loads(lines[-1])
            if server_report.get("listen_port") != port:
                raise RuntimeError("server startup port and summary stream disagree")

            if len(rows) != args.samples or server_report["healthy_replies"] != args.samples:
                raise RuntimeError(f"healthy delivery mismatch: client={len(rows)}, "
                    f"server={server_report['healthy_replies']}, expected={args.samples}")
            if server_report["healthy_error_events"] or server_report["healthy_send_failures"]:
                raise RuntimeError(f"healthy session was affected: {server_report}")
            if not server_report["healthy_active_observed"] or \
                server_report["healthy_during_slow_backpressure"] == 0:
                raise RuntimeError(f"healthy traffic did not overlap slow-reader backpressure: {server_report}")
            if server_report["slow_inbound"] == 0 or not server_report["slow_pressure_confirmed"]:
                raise RuntimeError(f"slow-reader pressure was not demonstrated: {server_report}")

            csv_path = args.output / "healthy_samples.csv"
            with csv_path.open("w", newline="") as destination:
                columns = list(rows[0]) if rows else ["id"]
                writer = csv.DictWriter(destination, fieldnames=columns)
                writer.writeheader()
                writer.writerows(rows)

            latencies = [row["scheduled_to_reply_ns"] for row in rows]
            report = {
                "boundary": "one Inline Engine and one owner thread; separate healthy BUY and non-reading LAG acceptor sessions",
                "samples": args.samples,
                "received": len(rows),
                "offered_rate_per_second": args.rate,
                "measured_elapsed_ns": elapsed_ns,
                "observed_rate_per_second": args.samples * 1e9 / elapsed_ns,
                "latency_ns": {
                    "p50": percentile(latencies, 0.50),
                    "p99": percentile(latencies, 0.99),
                    "p999": percentile(latencies, 0.999),
                    "max": max(latencies),
                },
                "healthy_samples_csv": str(csv_path),
                "slow_client": {
                    "read_after_logon": False,
                    "orders_sent": SLOW_MESSAGES,
                    "reply_payload_bytes": SLOW_PAYLOAD_BYTES,
                    "flow_control_pending_messages": 12,
                    "write_stall_timeout_ms": SLOW_STALL_MS,
                    "logout_timeout_ms": SLOW_LOGOUT_TIMEOUT_MS,
                    "client_closed_after_measurement": True,
                    "last_error_note": "the runner closes LAG only after the measured interval; queue_full and pending high-water are the backpressure proof",
                    **server_report,
                },
                "server_memory": {
                    **memory,
                    "measurement": "/proc/<pid>/status VmRSS and VmHWM sampled by runner",
                },
                "server_binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "python": platform.python_version(),
                "kernel": platform.platform(),
                "tcp_nodelay": True,
                "limitations": "loopback development-host measurement; Python scheduling and framing included; slow LAG intentionally loses replies when its bounded queue fills",
            }
            (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
            print(json.dumps(report))
    finally:
        if slow is not None:
            slow.sock.close()
        if healthy is not None:
            healthy.sock.close()
        if child is not None and child.poll() is None:
            child.send_signal(signal.SIGTERM)
            try:
                child.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                child.kill()
                child.communicate()


if __name__ == "__main__":
    main()
