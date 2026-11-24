#!/usr/bin/env python3
"""Run real Engine/MPSC loopback cases and preserve raw latency samples."""

import argparse
import csv
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import queue
import re
import selectors
import shutil
import socket
import statistics
import subprocess
import tempfile
import threading
import time

from run import output, source_digest


ROOT = Path(__file__).resolve().parents[1]
SOH = b"\x01"


def make_frame(seq, msgtype, fields=()):
    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H:%M:%S.%f")[:-3]
    body = (f"35={msgtype}\x0149=BUY\x0156=SELL\x0134={seq}\x0152={timestamp}\x01").encode()
    body += b"".join(str(tag).encode() + b"=" + str(value).encode() + SOH for tag, value in fields)
    prefix = b"8=FIX.4.4\x019=" + str(len(body)).encode() + SOH + body
    return prefix + f"10={sum(prefix) % 256:03d}".encode() + SOH


def extract_frames(buffer):
    frames = []
    while buffer:
        first = buffer.find(SOH)
        second = buffer.find(SOH, first + 1) if first >= 0 else -1
        if first < 0 or second < 0:
            break
        try:
            body_size = int(buffer[first + 3:second])
        except ValueError as error:
            raise RuntimeError("invalid FIX BodyLength") from error
        checksum_start = second + 1 + body_size
        frame_end = checksum_start + 7
        if len(buffer) < frame_end:
            break
        raw, buffer = buffer[:frame_end], buffer[frame_end:]
        if raw[checksum_start:checksum_start + 3] != b"10=":
            raise RuntimeError("invalid FIX CheckSum field")
        if sum(raw[:checksum_start]) % 256 != int(raw[checksum_start + 3:checksum_start + 6]):
            raise RuntimeError("invalid FIX checksum")
        fields = {}
        for part in raw[:-1].split(SOH):
            if b"=" not in part:
                continue
            tag, value = part.split(b"=", 1)
            fields[tag] = value
        frames.append((raw, fields))
    return frames, buffer


class Child:
    def __init__(self, command):
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1)
        self.events = queue.Queue()
        self.stderr_lines = []
        self.stdout_reader = threading.Thread(target=self._read_lines,
            args=(self.process.stdout, self.events), daemon=True)
        self.stderr_reader = threading.Thread(target=self._read_lines,
            args=(self.process.stderr, self.stderr_lines), daemon=True)
        self.stdout_reader.start()
        self.stderr_reader.start()

    @staticmethod
    def _read_lines(stream, destination):
        for line in stream:
            value = line.strip()
            if hasattr(destination, "put"):
                destination.put(value)
            else:
                destination.append(value)

    def command(self, value):
        self.process.stdin.write(value + "\n")
        self.process.stdin.flush()

    def wait_event(self, prefix, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = max(0.001, deadline - time.monotonic())
            try:
                line = self.events.get(timeout=remaining)
            except queue.Empty as error:
                exit_status = self.process.poll()
                details = " | ".join(self.stderr_lines[-10:])
                raise RuntimeError(f"timed out waiting for server event {prefix!r}; "
                    f"server_exit={exit_status}; stderr={details}") from error
            if line.startswith(prefix):
                return line
            if line.startswith("ERROR "):
                raise RuntimeError(line)
            raise RuntimeError(f"unexpected server event: {line}")
        raise RuntimeError(f"timed out waiting for server event {prefix!r}")

    def finish(self, timeout=30):
        if self.process.stdin and not self.process.stdin.closed:
            self.process.stdin.close()
        result = self.process.wait(timeout=timeout)
        self.stdout_reader.join(timeout=2)
        self.stderr_reader.join(timeout=2)
        if result:
            raise RuntimeError(f"benchmark child exit={result}: {' | '.join(self.stderr_lines[-10:])}")
        return self.stderr_lines

    def terminate(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)


def receive_until(peer, buffered, expected, server_sequence, timeout=30):
    peer.settimeout(timeout)
    while len(expected):
        frames, buffered = extract_frames(buffered)
        if not frames:
            chunk = peer.recv(65536)
            if not chunk:
                raise RuntimeError("peer closed before expected FIX reply")
            buffered += chunk
            continue
        for raw, fields in frames:
            msg_type = fields.get(b"35", b"").decode()
            if int(fields.get(b"34", b"0")) != server_sequence[0]:
                raise RuntimeError("out-of-sequence server message")
            server_sequence[0] += 1
            if msg_type in expected:
                expected.remove(msg_type)
    return buffered


def send_test_request_heartbeat(fields, pending, inbound_sequence, server_sequence):
    request_id = fields.get(b"112")
    if request_id is None:
        raise RuntimeError("server TestRequest omitted TestReqID")
    pending.extend(make_frame(inbound_sequence[0], "0", (
        (112, request_id.decode()), (369, server_sequence[0] - 1))))
    inbound_sequence[0] += 1


def callback_phase(child, peer, count, offset, payload, rate, window,
    inbound_sequence, server_sequence):
    child.command(f"RUN {count} {offset}")
    child.wait_event("RUN_READY")
    pending = bytearray()
    buffered = b""
    sent = received = 0
    origins = {}
    rows = []
    start_ns = time.monotonic_ns() + 10_000_000
    interval = max(1, 1_000_000_000 // rate)
    next_heartbeat_ns = time.monotonic_ns() + 5_000_000_000
    started_ns = None
    end_ns = None

    peer.setblocking(False)
    selector = selectors.DefaultSelector()
    try:
        while received < count:
            now = time.monotonic_ns()
            while sent < count and sent - received < window and now >= start_ns + sent * interval:
                scheduled = start_ns + sent * interval
                request_origin = time.monotonic_ns()
                identifier = offset + sent
                origins[identifier] = (scheduled, request_origin)
                pending.extend(make_frame(inbound_sequence[0], "D", (
                    (11, identifier), (55, "AAPL"), (58, payload),
                    (900001, request_origin), (900003, scheduled),
                    (369, server_sequence[0] - 1))))
                inbound_sequence[0] += 1
                sent += 1
                if started_ns is None:
                    started_ns = scheduled
            if now >= next_heartbeat_ns:
                pending.extend(make_frame(inbound_sequence[0], "0", (
                    (369, server_sequence[0] - 1),)))
                inbound_sequence[0] += 1
                next_heartbeat_ns = now + 5_000_000_000
            events = selectors.EVENT_READ | (selectors.EVENT_WRITE if pending else 0)
            selector.register(peer, events)
            next_due = next_heartbeat_ns
            if sent < count and sent - received < window:
                next_due = min(next_due, start_ns + sent * interval)
            timeout = min(0.01, max(0, (next_due - time.monotonic_ns()) / 1e9))
            ready = selector.select(timeout)
            selector.unregister(peer)
            for _, mask in ready:
                if mask & selectors.EVENT_WRITE and pending:
                    try:
                        amount = peer.send(pending)
                        del pending[:amount]
                    except BlockingIOError:
                        pass
                if mask & selectors.EVENT_READ:
                    try:
                        chunk = peer.recv(65536)
                    except BlockingIOError:
                        continue
                    if not chunk:
                        raise RuntimeError("server closed during callback phase; "
                            f"server_stderr={' | '.join(child.stderr_lines[-10:])}")
                    buffered += chunk
                    frames, buffered = extract_frames(buffered)
                    observed = time.monotonic_ns()
                    for _, fields in frames:
                        msg_type = fields.get(b"35", b"").decode()
                        if int(fields.get(b"34", b"0")) != server_sequence[0]:
                            raise RuntimeError("out-of-sequence server message")
                        server_sequence[0] += 1
                        if msg_type == "0":
                            continue
                        if msg_type == "1":
                            send_test_request_heartbeat(fields, pending, inbound_sequence, server_sequence)
                            continue
                        if msg_type != "8":
                            raise RuntimeError(f"unexpected message type in callback phase: {msg_type}; "
                                f"server_stderr={' | '.join(child.stderr_lines[-10:])}")
                        identifier = int(fields[b"11"])
                        scheduled_origin = origins.pop(identifier, None)
                        if scheduled_origin is None:
                            raise RuntimeError(f"unknown callback reply id {identifier}")
                        scheduled, origin = scheduled_origin
                        received += 1
                        end_ns = observed
                        if identifier >= offset and identifier < offset + count:
                            rows.append({"id": identifier, "scheduled_ns": scheduled,
                                "actual_origin_ns": origin,
                                "generation_lateness_ns": max(0, origin - scheduled),
                                "reply_observed_ns": observed,
                                "scheduled_round_trip_ns": observed - scheduled,
                                "actual_origin_round_trip_ns": observed - origin})
        if pending:
            while pending:
                try:
                    amount = peer.send(pending)
                    del pending[:amount]
                except BlockingIOError:
                    time.sleep(0.0001)
        child.wait_event("RUN_DONE ")
    finally:
        selector.close()
        peer.setblocking(True)
    elapsed_ns = (end_ns or time.monotonic_ns()) - (started_ns or start_ns)
    return rows, elapsed_ns


def mpsc_phase(child, peer, count, offset, payload, rate, inbound_sequence,
    server_sequence):
    child.command(f"RUN {count} {offset}")
    buffered = b""
    pending = bytearray()
    received = 0
    peer_rtts = []
    deadline = time.monotonic() + max(15, count / rate * 3 + 10)
    next_heartbeat = time.monotonic() + 5
    peer.setblocking(False)
    selector = selectors.DefaultSelector()
    try:
        while received < count or pending:
            if time.monotonic() >= deadline:
                raise RuntimeError(f"MPSC peer timeout: received={received}/{count}; "
                    f"server_exit={child.process.poll()}; stderr={' | '.join(child.stderr_lines[-10:])}")
            if time.monotonic() >= next_heartbeat:
                pending.extend(make_frame(inbound_sequence[0], "0", (
                    (369, server_sequence[0] - 1),)))
                inbound_sequence[0] += 1
                next_heartbeat = time.monotonic() + 5
            events = selectors.EVENT_READ | (selectors.EVENT_WRITE if pending else 0)
            selector.register(peer, events)
            ready = selector.select(0.05)
            selector.unregister(peer)
            for _, mask in ready:
                if mask & selectors.EVENT_WRITE and pending:
                    try:
                        amount = peer.send(pending)
                        del pending[:amount]
                    except BlockingIOError:
                        pass
                if mask & selectors.EVENT_READ:
                    try:
                        chunk = peer.recv(65536)
                    except BlockingIOError:
                        continue
                    if not chunk:
                        raise RuntimeError("server closed during MPSC phase; "
                            f"server_stderr={' | '.join(child.stderr_lines[-10:])}")
                    buffered += chunk
                    frames, buffered = extract_frames(buffered)
                    for _, fields in frames:
                        msg_type = fields.get(b"35", b"").decode()
                        if int(fields.get(b"34", b"0")) != server_sequence[0]:
                            raise RuntimeError("out-of-sequence sender-source server frame")
                        server_sequence[0] += 1
                        if msg_type == "0":
                            continue
                        if msg_type == "1":
                            send_test_request_heartbeat(fields, pending, inbound_sequence, server_sequence)
                            continue
                        if msg_type != "D":
                            raise RuntimeError(f"unexpected sender-source message type {msg_type}")
                        identifier = int(fields[b"11"])
                        origin = fields[b"900001"].decode()
                        response_origin = time.monotonic_ns()
                        pending.extend(make_frame(inbound_sequence[0], "8", (
                            (11, identifier), (17, "EXEC-1"),
                            (58, fields.get(b"58", b"").decode()),
                            (900001, origin), (900002, response_origin),
                            (369, server_sequence[0] - 1))))
                        inbound_sequence[0] += 1
                        peer_rtts.append((identifier, response_origin))
                        received += 1
        done = child.wait_event("RUN_DONE ")
        server_elapsed_ns = int(done.split()[2])
    finally:
        selector.close()
        peer.setblocking(True)
    return peer_rtts, server_elapsed_ns


def session_run(binary, output_dir, state_parent, mode, send_source, samples,
    warmup, producers, payload_bytes, rate, window, segment_bytes,
    max_segments, retain_outbound_messages, diagnostic_pass):
    run_name = "diagnostics" if diagnostic_pass else "timed"
    state_dir = Path(tempfile.mkdtemp(prefix=f"nimblefix-runtime-{mode}-{run_name}-",
        dir=state_parent))
    sample_path = state_dir / "samples.csv"
    child = None
    peer = None
    try:
        child = Child([str(binary), mode, send_source, str(samples), str(warmup),
            str(producers), str(payload_bytes), str(rate), run_name, str(state_dir),
            str(segment_bytes), str(max_segments), str(retain_outbound_messages)])
        event = child.wait_event("READY ", timeout=15)
        port = int(event.split()[1])
        peer = socket.create_connection(("127.0.0.1", port), timeout=10)
        peer.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        peer.settimeout(10)
        peer.sendall(make_frame(1, "A", ((98, 0), (108, 30), (369, 0))))
        buffered = b""
        server_sequence = [1]
        inbound_sequence = [2]
        buffered = receive_until(peer, buffered, {"A"}, server_sequence, timeout=10)
        initial_segment_indices = journal_segment_indices(state_dir)

        phase_rows = []
        measured_elapsed_ns = 0
        phase_specs = []
        if warmup:
            phase_specs.append((warmup, 0, True))
        phase_specs.append((samples, warmup, False))
        for count, offset, is_warmup in phase_specs:
            if send_source == "callback":
                rows, elapsed = callback_phase(child, peer, count, offset,
                    "X" * payload_bytes, rate, window, inbound_sequence, server_sequence)
                if not is_warmup:
                    phase_rows.extend(rows)
                    measured_elapsed_ns = elapsed
            else:
                peer_rtts, elapsed = mpsc_phase(child, peer, count, offset,
                    "X" * payload_bytes, rate, inbound_sequence, server_sequence)
                if not is_warmup:
                    phase_rows.extend({"id": identifier, "peer_response_sent_ns": timestamp}
                        for identifier, timestamp in peer_rtts)
                    measured_elapsed_ns = elapsed

        peer.setblocking(True)
        peer.settimeout(10)
        peer.sendall(make_frame(inbound_sequence[0], "5", ((369, server_sequence[0] - 1),)))
        buffered = receive_until(peer, buffered, {"5"}, server_sequence, timeout=10)
        child.command("STOP")
        peer.close()
        peer = None
        stderr = child.finish()
        child = None
        report_line = next((line for line in reversed(stderr)
            if line.startswith('{"measured_elapsed_ns"')), None)
        if report_line is None:
            raise RuntimeError(f"missing server summary: {' | '.join(stderr[-10:])}")
        server_report = json.loads(report_line)
        if sample_path.exists():
            with sample_path.open(newline="") as source:
                server_rows = {int(row["id"]): row for row in csv.DictReader(source)}
        else:
            server_rows = {}
        segment_indices = journal_segment_indices(state_dir)
        initial_max_segment = max(initial_segment_indices, default=-1)
        final_max_segment = max(segment_indices, default=-1)
        rotation_observed = final_max_segment >= max_segments if max_segments else \
            final_max_segment > initial_max_segment
        if diagnostic_pass:
            expected_kernel_type = "8" if send_source == "callback" else "D"
            measured_ids = list(range(warmup, warmup + samples))
            kernel_rows = [server_rows.get(identifier) for identifier in measured_ids]
            if any(row is None for row in kernel_rows):
                raise RuntimeError("kernel-send diagnostic omitted measured sample rows")
            if server_report["kernel_send_expected_type"] != expected_kernel_type:
                raise RuntimeError("kernel-send diagnostic watched the wrong FIX message type")
            if server_report["kernel_send_completed_samples"] != samples or any(
                not row.get("kernel_write_return_ns") or int(row["kernel_write_return_ns"]) == 0
                for row in kernel_rows
            ):
                completed = sum(bool(row.get("kernel_write_return_ns")) and
                    int(row["kernel_write_return_ns"]) != 0 for row in kernel_rows)
                raise RuntimeError(f"kernel-send diagnostic completed {completed}/{samples} sample frames")

            diagnostic_samples_dir = output_dir / "diagnostics"
            diagnostic_samples_dir.mkdir(parents=True, exist_ok=True)
            kernel_sample_csv = diagnostic_samples_dir / "kernel-send-samples.csv"
            kernel_sample_json = diagnostic_samples_dir / "kernel-send-samples.json"
            kernel_sample_rows = []
            accepted_to_kernel_raw = []
            accepted_to_kernel_lower = []
            api_to_kernel_upper = []
            preaccept_count = 0
            for identifier, source_row in zip(measured_ids, kernel_rows):
                scheduled_ns = int(source_row["scheduled_ns"])
                origin_ns = int(source_row["origin_ns"])
                api_start_ns = int(source_row["api_start_ns"])
                accepted_ns = int(source_row["accepted_ns"])
                kernel_ns = int(source_row["kernel_write_return_ns"])
                if not api_start_ns or not accepted_ns or not kernel_ns:
                    raise RuntimeError(f"missing diagnostic API/kernel timestamp for id={identifier}")
                if accepted_ns < api_start_ns or kernel_ns < api_start_ns:
                    raise RuntimeError(f"diagnostic timestamp order violated for id={identifier}")
                raw_accept_delta = kernel_ns - accepted_ns
                lower = max(0, raw_accept_delta)
                upper = kernel_ns - api_start_ns
                wrote_before_accept = kernel_ns < accepted_ns
                preaccept_count += int(wrote_before_accept)
                accepted_to_kernel_raw.append(raw_accept_delta)
                accepted_to_kernel_lower.append(lower)
                api_to_kernel_upper.append(upper)
                kernel_sample_rows.append({
                    "id": identifier,
                    "message_type": expected_kernel_type,
                    "scheduled_ns": scheduled_ns,
                    "actual_origin_ns": origin_ns,
                    "generation_lateness_ns": int(source_row["generation_lateness_ns"]),
                    "api_start_ns": api_start_ns,
                    "accepted_return_ns": accepted_ns,
                    "kernel_write_return_ns": kernel_ns,
                    "kernel_minus_accepted_return_signed_ns": raw_accept_delta,
                    "accepted_to_kernel_lower_bound_ns": lower,
                    "accepted_to_kernel_upper_bound_signed_ns": upper,
                    "kernel_write_before_accept_return": int(wrote_before_accept),
                })
            with kernel_sample_csv.open("w", newline="") as destination:
                columns = list(kernel_sample_rows[0])
                writer = csv.DictWriter(destination, fieldnames=columns)
                writer.writeheader()
                writer.writerows(kernel_sample_rows)
            kernel_sample_json.write_text(json.dumps(kernel_sample_rows, indent=2) + "\n",
                encoding="utf-8")

            def distribution(values):
                return {
                    "p50_ns": percentile(values, .50),
                    "p99_ns": percentile(values, .99),
                    "p999_ns": percentile(values, .999),
                    "min_ns": min(values),
                    "max_ns": max(values),
                    "negative_count": sum(value < 0 for value in values),
                }

            kernel_diagnostic = {
                "samples": samples,
                "message_type": expected_kernel_type,
                "completed_full_frame_writes": len(kernel_sample_rows),
                "expected_full_frame_writes": samples,
                "kernel_write_before_accept_return_samples": preaccept_count,
                "kernel_minus_accepted_return_signed_ns": distribution(accepted_to_kernel_raw),
                "accepted_to_kernel_lower_bound_ns": distribution(accepted_to_kernel_lower),
                "accepted_to_kernel_upper_bound_signed_ns": distribution(api_to_kernel_upper),
                "bounds_note": "accepted_return_ns is sampled immediately after the API call; scheduling can intervene before that sample. API acceptance occurs inside the call, bracketed by api_start_ns and accepted_return_ns. kernel_write_return_ns is sampled after send returned all bytes of the frame to the kernel, not peer receipt. Lower bound is max(0, write_return - accepted_return); upper bound is write_return - api_start. Raw signed deltas are preserved; a negative raw delta counts a write before the post-call observation, and does not prove it preceded the actual API return. Impossible negative upper bounds fail validation.",
                "samples_csv": str(kernel_sample_csv),
                "samples_json": str(kernel_sample_json),
                "scope": "separate diagnostic pass with send wrapper enabled; includes only complete tracked FIX D/8 frames from the measured phase",
            }
            (diagnostic_samples_dir / "kernel-send-diagnostic.json").write_text(
                json.dumps(kernel_diagnostic, indent=2) + "\n", encoding="utf-8")
            return {"diagnostics": server_report["counters"],
                "minor_page_faults": server_report["minor_page_faults"],
                "major_page_faults": server_report["major_page_faults"],
                "peak_rss_kib": server_report["peak_rss_kib"],
                "perf_counters": server_report["perf_counters"],
                "kernel_send_diagnostic": kernel_diagnostic,
                "journal_live_segments": len(segment_indices),
                "journal_initial_max_segment_index": initial_max_segment,
                "journal_max_segment_index": final_max_segment,
                "journal_rotation_observed": rotation_observed}
        client_rows = {int(row["id"]): row for row in phase_rows}
        combined = []
        for identifier in range(warmup, warmup + samples):
            server = server_rows.get(identifier, {})
            client = client_rows.get(identifier, {})
            if send_source == "callback":
                scheduled_ns = int(client.get("scheduled_ns", 0))
                origin_ns = int(client.get("actual_origin_ns", 0))
                completion_ns = int(client.get("reply_observed_ns", 0))
                scheduled_rtt_ns = int(client.get("scheduled_round_trip_ns", 0))
                actual_origin_rtt_ns = int(client.get("actual_origin_round_trip_ns", 0))
            else:
                scheduled_ns = int(server.get("scheduled_ns", 0))
                origin_ns = int(server.get("origin_ns", 0))
                completion_ns = int(server.get("callback_start_ns", 0))
                scheduled_rtt_ns = int(server.get("server_round_trip_ns", 0))
                actual_origin_rtt_ns = int(server.get("actual_origin_round_trip_ns", 0))
            if not scheduled_ns or not origin_ns or not completion_ns:
                raise RuntimeError(f"missing scheduled/origin/completion timestamp for id={identifier}: "
                    f"server={server}; peer={client}")
            if origin_ns < scheduled_ns:
                raise RuntimeError(f"sample generated before its scheduled time for id={identifier}")
            if not scheduled_rtt_ns:
                raise RuntimeError(f"missing scheduled-boundary RTT for id={identifier}")
            lateness_ns = origin_ns - scheduled_ns
            combined.append({
                "id": identifier,
                "scheduled_ns": scheduled_ns,
                "actual_origin_ns": origin_ns,
                "generation_lateness_ns": lateness_ns,
                "scheduled_to_completion_ns": scheduled_rtt_ns,
                "actual_origin_round_trip_ns": actual_origin_rtt_ns,
                "completion_observed_ns": completion_ns,
                "server_callback_start_ns": server.get("callback_start_ns", ""),
                "server_callback_end_ns": server.get("callback_end_ns", ""),
                "accepted_ns": server.get("accepted_ns", ""),
                "callback_ingress_origin_ns": server.get("callback_ingress_origin_ns", ""),
                "callback_ingress_delay_ns": server.get("callback_ingress_delay_ns", ""),
                "callback_duration_ns": server.get("callback_duration_ns", ""),
                "server_scheduled_to_callback_ns": server.get("server_round_trip_ns", ""),
                "peer_response_sent_ns": client.get("peer_response_sent_ns", ""),
                "reply_observed_ns": client.get("reply_observed_ns", ""),
                "receipt_id": server.get("receipt_id", ""),
                "receipt_state": server.get("receipt_state", "missing"),
                "delivery_observed_on_peer": bool(client.get("reply_observed_ns")) or
                    send_source in {"mpsc", "lane"},
            })
        expected_receipt_states = {"accepted", "appended", "written"} if send_source == "callback" else {"written"}
        if any(row["receipt_state"] not in expected_receipt_states for row in combined):
            bad = next(row for row in combined if row["receipt_state"] not in expected_receipt_states)
            raise RuntimeError(f"unexpected receipt state {bad['receipt_state']!r} for id={bad['id']}; "
                f"expected one of {sorted(expected_receipt_states)}")
        receipt_counts = {}
        for row in combined:
            state = row["receipt_state"]
            receipt_counts[state] = receipt_counts.get(state, 0) + 1
        expected_receipt_state = "accepted_or_appended_or_written" if send_source == "callback" else "written"
        csv_path = output_dir / "samples.csv"
        with csv_path.open("w", newline="") as destination:
            columns = list(combined[0]) if combined else ["id"]
            writer = csv.DictWriter(destination, fieldnames=columns)
            writer.writeheader()
            writer.writerows(combined)
        rtts = [int(row["scheduled_to_completion_ns"]) for row in combined]
        actual_origin_rtts = [int(row["actual_origin_round_trip_ns"]) for row in combined]
        lateness_values = [int(row["generation_lateness_ns"]) for row in combined]
        missing_ingress = [row["id"] for row in combined
            if not row["callback_ingress_delay_ns"]]
        if missing_ingress:
            first = missing_ingress[0]
            raise RuntimeError(f"server callback sample missing ingress data for id={first}: "
                f"{server_rows.get(first)}")
        ingress_delays = [int(row["callback_ingress_delay_ns"]) for row in combined]
        if send_source == "mpsc":
            receipt_semantics = "ordinary MPSC Sender receipt queried until ReceiptState::written"
        elif send_source == "lane":
            receipt_semantics = "SPSC ProducerLane receipt queried until ReceiptState::written"
        else:
            receipt_semantics = "SessionContext::send returns the Driver receipt state available at API return (accepted/appended/written depending on callback policy); peer receive is separately observed and establishes delivery"
        return {"samples_csv": str(csv_path), "samples": samples,
            "warmup": warmup, "send_source": send_source,
            "measured_elapsed_ns": measured_elapsed_ns or server_report["measured_elapsed_ns"],
            "throughput_per_second": samples * 1e9 / max(1,
                measured_elapsed_ns or server_report["measured_elapsed_ns"]),
            "completion_boundary": (
                "scheduled FIX D generation time to final FIX 8 reply observation by the Python peer"
                if send_source == "callback" else
                "scheduled sender time to inbound FIX 8 callback start in the Engine"),
            "round_trip_ns": {"p50": percentile(rtts, .50), "p99": percentile(rtts, .99),
                "p999": percentile(rtts, .999), "max": max(rtts),
                "scope": "scheduled time to the case completion boundary"},
            "actual_origin_round_trip_ns": {
                "p50": percentile(actual_origin_rtts, .50),
                "p99": percentile(actual_origin_rtts, .99),
                "p999": percentile(actual_origin_rtts, .999),
                "max": max(actual_origin_rtts),
                "scope": "actual generation/API-origin time to the same completion boundary"},
            "generation_lateness_ns": {
                "late_count": sum(value > 0 for value in lateness_values),
                "samples": len(lateness_values),
                "p50": percentile(lateness_values, .50),
                "p99": percentile(lateness_values, .99),
                "max": max(lateness_values),
                "scope": "actual FIX generation/origin timestamp minus scheduled timestamp; all planned samples retained"},
            "callback_ingress_delay_ns": {"p50": percentile(ingress_delays, .50),
                "p99": percentile(ingress_delays, .99),
                "p999": percentile(ingress_delays, .999), "max": max(ingress_delays)},
            "callback_ingress_delay_note": (
                "Python peer FIX D send start to Engine callback start; includes socket, parse, owner and queued dispatch time, not an isolated worker-queue wait; do not compare directly with sender-source cases"
                if send_source == "callback" else
                "Python peer FIX 8 send start for MPSC/ProducerLane cases to Engine callback start; includes socket, parse, owner and queued dispatch time, not an isolated worker-queue wait; do not compare directly with callback-source cases"),
            "minor_page_faults": server_report["minor_page_faults"],
            "major_page_faults": server_report["major_page_faults"],
            "journal_live_segments": len(segment_indices),
            "journal_initial_max_segment_index": initial_max_segment,
            "journal_max_segment_index": final_max_segment,
            "journal_rotation_observed": rotation_observed,
            "journal_rotation_config": {"segment_bytes": segment_bytes,
                "max_segments": max_segments,
                "retain_outbound_messages": retain_outbound_messages},
            "producer_threads": producers,
            "request_rate_per_second": rate,
            "in_flight_window": window,
            "server_boundary": ("scheduled ordinary MPSC Sender or SPSC ProducerLane time to inbound FIX 8 callback start"
                if send_source in {"mpsc", "lane"} else
                "scheduled Python FIX D generation time to callback-generated FIX 8 receive"),
            "receipt_semantics": receipt_semantics,
            "receipt_counts": receipt_counts,
            "receipt_state_expectation": expected_receipt_state}
    except Exception as error:
        if child is not None:
            child.terminate()
            child.stderr_reader.join(timeout=1)
            details = " | ".join(child.stderr_lines[-10:])
            if details:
                error.add_note(f"server stderr after shutdown: {details}")
        if peer is not None:
            peer.close()
        raise
    finally:
        shutil.rmtree(state_dir, ignore_errors=True)


def percentile(values, quantile):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * quantile) - 1)]


def mount_details(path):
    filesystem = subprocess.check_output(["stat", "-f", "-c", "%T", str(path)], text=True).strip()
    try:
        mount = subprocess.run(["findmnt", "-J", "-T", str(path)], capture_output=True, text=True)
        details = json.loads(mount.stdout) if mount.returncode == 0 else {}
    except (OSError, json.JSONDecodeError):
        details = {}
    return filesystem, details


def journal_segment_indices(state_dir):
    indices = []
    for segment in state_dir.rglob("*.seg*"):
        marker = segment.name.rfind(".seg")
        try:
            indices.append(int(segment.name[marker + 4:]))
        except (ValueError, IndexError):
            continue
    return indices


def writeback_snapshot():
    keys = {"nr_dirty", "nr_writeback", "nr_dirtied", "nr_written", "pgfault", "pgmajfault"}
    vmstat = {}
    for line in Path("/proc/vmstat").read_text().splitlines():
        name, _, value = line.partition(" ")
        if name in keys:
            vmstat[name] = int(value)
    tunables = {}
    for path in sorted(Path("/proc/sys/vm").glob("dirty_*")):
        try:
            tunables[path.name] = path.read_text().strip()
        except OSError:
            tunables[path.name] = "unavailable"
    return {"vmstat": vmstat, "dirty_tunables": tunables}


def parse_cpu_list(value):
    cpus = set()
    for part in value.split(","):
        if "-" in part:
            begin, end = (int(item) for item in part.split("-", 1))
            cpus.update(range(begin, end + 1))
        else:
            cpus.add(int(part))
    if not cpus:
        raise ValueError("empty CPU set")
    return cpus


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--state-parent", type=Path, default=ROOT / "build/bench-state")
    parser.add_argument("--mode", choices=("inline", "queued-journal", "queued-ring", "all"), default="all")
    parser.add_argument("--send-source", choices=("callback", "mpsc", "lane", "both"), default="both")
    parser.add_argument("--samples", type=int, default=2000)
    parser.add_argument("--warmup", type=int, default=100)
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--producers", type=int, default=4)
    parser.add_argument("--payload-bytes", type=int, default=32)
    parser.add_argument("--rate", type=int, default=2000)
    parser.add_argument("--window", type=int, default=64)
    parser.add_argument("--journal-segment-bytes", type=int, default=1024 * 1024)
    parser.add_argument("--max-journal-segments", type=int, default=16)
    parser.add_argument("--retain-outbound-messages", type=int, default=8192)
    parser.add_argument("--require-filesystem", help="optional explicit filesystem gate for dedicated measurements, e.g. btrfs")
    parser.add_argument("--cpu-set", help="optional Linux CPU list, for example 2 or 2-3")
    parser.add_argument("--compiler", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--build-flags", default="C++20 -O3; static nimblefix; linker --wrap instrumentation")
    args = parser.parse_args()
    if not 1 <= args.samples <= 100000 or not 0 <= args.warmup <= 10000 or \
        not 1 <= args.runs <= 20 or not 1 <= args.producers <= 128 or \
        not 0 <= args.payload_bytes <= 65000 or not 1 <= args.rate <= 100000 or \
        not 1 <= args.window <= 128 or not 8192 <= args.journal_segment_bytes <= 256 * 1024 * 1024 or \
        args.journal_segment_bytes % 8 != 0 or not 0 <= args.max_journal_segments <= 10000 or \
        not 0 <= args.retain_outbound_messages <= 1_000_000:
        parser.error("samples/warmup/producers/payload/rate/window are outside supported bounds")
    if args.cpu_set:
        os.sched_setaffinity(0, parse_cpu_list(args.cpu_set))

    binary = args.binary.resolve()
    output_dir = args.output.resolve()
    state_parent = args.state_parent.resolve()
    state_parent.mkdir(parents=True, exist_ok=True)
    filesystem, mount = mount_details(state_parent)
    if args.require_filesystem and filesystem != args.require_filesystem:
        parser.error(f"requested filesystem {args.require_filesystem!r}, found {filesystem!r}")
    output_dir.mkdir(parents=True, exist_ok=True)

    modes = ["inline", "queued-journal", "queued-ring"] if args.mode == "all" else [args.mode]
    sources = ["callback", "mpsc", "lane"] if args.send_source == "both" else [args.send_source]
    report = {
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "project_version": re.search(r"project\(NimbleFIX VERSION ([^)\s]+)",
            (ROOT / "CMakeLists.txt").read_text()).group(1),
        "revision": output("git", "rev-parse", "HEAD"),
        "worktree_dirty": bool(output("git", "status", "--porcelain")),
        "source_sha256": source_digest(),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "binary_bytes": binary.stat().st_size,
        "compiler": output(args.compiler, "--version").splitlines()[0],
        "build_flags": args.build_flags,
        "kernel": platform.platform(),
        "cpu": output("lscpu"),
        "cpu_affinity": sorted(os.sched_getaffinity(0)),
        "cpu_pinned": len(os.sched_getaffinity(0)) == 1,
        "journal_filesystem": filesystem,
        "memory_filesystem": filesystem in ("tmpfs", "ramfs"),
        "journal_mount": mount,
        "journal_location": str(state_parent),
        "schema": "FlatSchema with FIX.4.4 raw loopback peer; D/8 and tags 11, 17, 35, 55, 58, 900001, 900002",
        "payload_bytes": args.payload_bytes,
        "journal_segment_bytes": args.journal_segment_bytes,
        "max_journal_segments": args.max_journal_segments,
        "retain_outbound_messages": args.retain_outbound_messages,
        "samples": args.samples,
        "warmup": args.warmup,
        "producer_threads": args.producers,
        "request_rate_per_second": args.rate,
        "in_flight_window": args.window,
        "runs": [],
        "diagnostic_scope": "separate child process/pass; atomic wrappers for executable/static objects; calls originating wholly inside shared libraries may bypass hooks; counters cover the measured phase including harness coordination, producer wake/poll and receipt polling. The executable is linked with a send wrapper in both passes; tracking is disabled in the timed pass, which still pays wrapper dispatch and atomic flag-read overhead, while only the diagnostic pass parses frame bytes",
        "allocation_counter_semantics": "successful malloc/calloc/realloc and overridden C++ new requests; bytes sum requested sizes, realloc counts the full new size rather than its delta",
        "timing_scope": "real Engine, mmap Journal, FIX codec and localhost TCP; no fsync/fdatasync/msync durability guarantee",
        "counter_normalization": "diagnostic totals divided by measured sample count are phase-wide per-sample normalized costs, not steady-state per-message hot-path costs",
        "writeback_observation_scope": "read-only host-wide counters and dirty_* tunables; deltas include concurrent machine activity and cannot be attributed to this process",
    }

    writeback_before = writeback_snapshot()

    for run_index in range(args.runs):
        cases = {}
        for mode in modes:
            for source in sources:
                case_dir = output_dir / mode / source / f"run-{run_index + 1}"
                case_dir.mkdir(parents=True, exist_ok=True)
                timed = session_run(binary, case_dir, state_parent, mode, source,
                    args.samples, args.warmup, args.producers, args.payload_bytes,
                    args.rate, args.window, args.journal_segment_bytes,
                    args.max_journal_segments, args.retain_outbound_messages, False)
                diagnostic = session_run(binary, case_dir, state_parent, mode, source,
                    args.samples, args.warmup, args.producers, args.payload_bytes,
                    args.rate, args.window, args.journal_segment_bytes,
                    args.max_journal_segments, args.retain_outbound_messages, True)
                diagnostic_counts = dict(diagnostic["diagnostics"])
                diagnostic_counts["per_sample_phase_normalized"] = {
                    key: value / args.samples for key, value in diagnostic["diagnostics"].items()
                    if key.endswith("_calls") or key in {"allocation_bytes"}}
                timed["diagnostics"] = diagnostic_counts
                timed["diagnostic_minor_page_faults"] = diagnostic["minor_page_faults"]
                timed["diagnostic_major_page_faults"] = diagnostic["major_page_faults"]
                timed["diagnostic_peak_rss_kib"] = diagnostic["peak_rss_kib"]
                timed["diagnostic_perf_counters"] = diagnostic["perf_counters"]
                timed["kernel_send_diagnostic"] = diagnostic["kernel_send_diagnostic"]
                timed["diagnostic_journal_live_segments"] = diagnostic["journal_live_segments"]
                timed["diagnostic_journal_initial_max_segment_index"] = diagnostic["journal_initial_max_segment_index"]
                timed["diagnostic_journal_max_segment_index"] = diagnostic["journal_max_segment_index"]
                timed["diagnostic_journal_rotation_observed"] = diagnostic["journal_rotation_observed"]
                cases[f"{mode}/{source}"] = timed
        report["runs"].append({"run": run_index + 1, "cases": cases})

    writeback_after = writeback_snapshot()
    report["host_writeback_observation"] = {
        "scope": "host-wide; deltas are concurrent machine activity and cannot be attributed to this process",
        "before": writeback_before,
        "after": writeback_after,
        "vmstat_delta": {key: writeback_after["vmstat"].get(key, value) - value
            for key, value in writeback_before["vmstat"].items()},
    }

    report_path = output_dir / "report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    for case, result in report["runs"][0]["cases"].items():
        print(f"{case}: RTT p50={result['round_trip_ns']['p50']}ns "
            f"p99={result['round_trip_ns']['p99']}ns; "
            f"ingress-delay p50={result['callback_ingress_delay_ns']['p50']}ns; "
            f"CSV={result['samples_csv']}")


if __name__ == "__main__":
    main()
