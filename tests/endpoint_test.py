#!/usr/bin/env python3
"""Exercise the public endpoints as independent, interactive FIX processes."""

import argparse
from collections import deque
from datetime import datetime, timezone
from decimal import Decimal
from pathlib import Path
import queue
import re
import select
import signal
import socket
import subprocess
import tempfile
import threading
import time


TIMEOUT = 30
SOH = b"\x01"


class LineProcess:
    """Capture line oriented process output while keeping stdin interactive."""

    def __init__(self, command, cwd):
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            bufsize=1, cwd=cwd)
        self.stdout_queue = queue.Queue()
        self.stdout_lines = []
        self.stderr_lines = []
        self.stdout_thread = threading.Thread(target=self._read_stdout, daemon=True)
        self.stderr_thread = threading.Thread(target=self._read_stderr, daemon=True)
        self.stdout_thread.start()
        self.stderr_thread.start()

    def _read_stdout(self):
        for line in self.process.stdout:
            self.stdout_queue.put(line.rstrip("\r\n"))
        self.stdout_queue.put(None)

    def _read_stderr(self):
        for line in self.process.stderr:
            self.stderr_lines.append(line.rstrip("\r\n"))

    def mark(self):
        return len(self.stdout_lines)

    def write(self, line):
        if self.process.stdin is None or self.process.stdin.closed:
            raise RuntimeError("process stdin is closed")
        try:
            self.process.stdin.write(line + "\n")
            self.process.stdin.flush()
        except (BrokenPipeError, OSError) as error:
            raise RuntimeError(f"could not send command {line!r}: {self._details()}") from error

    def close_stdin(self):
        if self.process.stdin is not None and not self.process.stdin.closed:
            self.process.stdin.close()

    def send_signal(self, value):
        self.process.send_signal(value)

    def _details(self):
        return " | ".join(self.stderr_lines[-12:])

    def wait_for(self, predicate, description, timeout=TIMEOUT, after=0):
        deadline = time.monotonic() + timeout
        cursor = max(0, after)
        while True:
            while cursor < len(self.stdout_lines):
                line = self.stdout_lines[cursor]
                cursor += 1
                if predicate(line):
                    return line
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError(f"timed out waiting for {description}; "
                    f"exit={self.process.poll()}; stdout={self.stdout_lines[-12:]}; "
                    f"stderr={self._details()}")
            try:
                line = self.stdout_queue.get(timeout=remaining)
            except queue.Empty as error:
                raise RuntimeError(f"timed out waiting for {description}; "
                    f"exit={self.process.poll()}; stdout={self.stdout_lines[-12:]}; "
                    f"stderr={self._details()}") from error
            if line is None:
                raise RuntimeError(f"process exited before {description}; "
                    f"exit={self.process.poll()}; stdout={self.stdout_lines[-12:]}; "
                    f"stderr={self._details()}")
            self.stdout_lines.append(line)

    def wait_text(self, text, description=None, timeout=TIMEOUT, after=0):
        return self.wait_for(lambda line: text in line,
            description or f"output containing {text!r}", timeout, after)

    def wait_regex(self, pattern, description=None, timeout=TIMEOUT, after=0):
        return self.wait_for(lambda line: pattern.search(line) is not None,
            description or f"output matching {pattern.pattern!r}", timeout, after)

    def wait_absent(self, predicate, description, duration, after=0):
        deadline = time.monotonic() + duration
        for line in self.stdout_lines[max(0, after):]:
            if predicate(line):
                raise RuntimeError(f"unexpected {description}: {line!r}")
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            try:
                line = self.stdout_queue.get(timeout=remaining)
            except queue.Empty:
                return
            if line is None:
                raise RuntimeError(f"process exited while checking absence of {description}; "
                    f"exit={self.process.poll()}; stderr={self._details()}")
            self.stdout_lines.append(line)
            if predicate(line):
                raise RuntimeError(f"unexpected {description}: {line!r}")

    def wait_exit(self, timeout=TIMEOUT):
        try:
            result = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(f"process did not exit within {timeout}s; "
                f"stdout={self.stdout_lines[-12:]}; stderr={self._details()}") from error
        self.stdout_thread.join(timeout=2)
        self.stderr_thread.join(timeout=2)
        while True:
            try:
                line = self.stdout_queue.get_nowait()
            except queue.Empty:
                break
            if line is not None:
                self.stdout_lines.append(line)
        return result

    def terminate(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        self.close_stdin()
        self.stdout_thread.join(timeout=1)
        self.stderr_thread.join(timeout=1)


def validate_cli(acceptor, initiator, directory):
    for binary in (acceptor, initiator):
        result = subprocess.run([str(binary), "--help"], capture_output=True,
            timeout=3, cwd=directory)
        if result.returncode:
            raise RuntimeError(f"{binary.name} --help failed: {result.stderr!r}")
        for arguments in (("--port", "1x"), ("--port", "65536"), ("--port", "-1"),
                ("--timeout-ms", "0"), ("--host", "localhost"), ("--unknown", "x")):
            result = subprocess.run([str(binary), "--state", str(directory / "invalid"),
                "--port", "12345", *arguments], capture_output=True, timeout=3, cwd=directory)
            if result.returncode != 2:
                raise RuntimeError(f"invalid CLI accepted by {binary.name}: {arguments}; "
                    f"stdout={result.stdout!r}; stderr={result.stderr!r}")
    removed_option = subprocess.run([str(initiator), "--state", str(directory / "old-cli"),
        "--port", "12345", "--cl-ord-id", "NO-LONGER-SUPPORTED"],
        capture_output=True, timeout=3, cwd=directory)
    if removed_option.returncode != 2:
        raise RuntimeError("initiator still accepts the removed --cl-ord-id option")

    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        unavailable_port = reservation.getsockname()[1]
        # A bound, non-listening socket makes refusal deterministic.
        failed = subprocess.run([str(initiator), "--state", str(directory / "unavailable"),
            "--port", str(unavailable_port), "--timeout-ms", "250"],
            input="", text=True, capture_output=True, timeout=8, cwd=directory)
        if failed.returncode != 1:
            raise RuntimeError("unavailable endpoint did not fail within its deadline: "
                f"exit={failed.returncode}; stdout={failed.stdout!r}; stderr={failed.stderr!r}")


def start_acceptor(binary, state, cwd):
    process = LineProcess([str(binary), "--state", str(state), "--port", "0",
        "--timeout-ms", "1000"], cwd)
    line = process.wait_regex(re.compile(r"LISTENING 127\.0\.0\.1:(\d+)"),
        "acceptor endpoint publication", timeout=10)
    match = re.search(r"LISTENING 127\.0\.0\.1:(\d+)", line)
    if match is None:
        raise RuntimeError(f"invalid acceptor endpoint: {line!r}")
    return process, int(match.group(1))


def start_initiator(binary, state, port, cwd, timeout_ms=8000):
    return LineProcess([str(binary), "--state", str(state), "--host", "127.0.0.1",
        "--port", str(port), "--timeout-ms", str(timeout_ms)], cwd)


def order_events(client, identifier, symbol, side, after, expect_running=True):
    escaped_id = re.escape(identifier)
    accepted = client.wait_regex(re.compile(
        rf"\bACCEPTED\s+ClOrdID={escaped_id}\s+receipt=\d+\b"),
        f"accepted event for {identifier}", after=after)
    receipt = client.wait_regex(re.compile(
        rf"\bRECEIPT\s+ClOrdID={escaped_id}\s+state=written\s+sequence=(\d+)\b"),
        f"written receipt for {identifier}", after=after)
    report = client.wait_regex(re.compile(
        rf"\bEXECUTION_REPORT\s+ClOrdID={escaped_id}\s+Symbol={re.escape(symbol)}\s+Side={side}\b"),
        f"matching ExecutionReport for {identifier}", after=after)
    sequence_match = re.search(r"\bsequence=(\d+)\b", receipt)
    if sequence_match is None:
        raise RuntimeError(f"written receipt omitted sequence number: {receipt!r}")
    if expect_running and client.process.poll() is not None:
        raise RuntimeError(f"initiator exited after ExecutionReport for {identifier}: "
            f"{accepted!r}; {receipt!r}; {report!r}; stderr={client._details()}")
    return int(sequence_match.group(1))


def decimal_equal(actual, expected):
    try:
        return Decimal(actual) == Decimal(expected)
    except Exception:
        return False


def verify_fresh_timestamp(value, command_time):
    formats = ("%Y%m%d-%H:%M:%S.%f", "%Y%m%d-%H:%M:%S")
    parsed = None
    for format_string in formats:
        try:
            parsed = datetime.strptime(value, format_string).replace(tzinfo=timezone.utc)
            break
        except ValueError:
            pass
    if parsed is None or abs((parsed - command_time).total_seconds()) > 5:
        raise RuntimeError(f"TransactTime is missing, malformed, or stale: {value!r}")
    return parsed


def make_frame(sequence, message_type, fields=(), sender="SELL", target="BUY"):
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H:%M:%S.%f")[:-3]
    body = (f"35={message_type}\x0149={sender}\x0156={target}\x0134={sequence}"
        f"\x0152={stamp}\x01").encode()
    body += b"".join(str(tag).encode() + b"=" + str(value).encode() + SOH
        for tag, value in fields)
    prefix = b"8=FIX.4.4\x019=" + str(len(body)).encode() + SOH + body
    return prefix + f"10={sum(prefix) % 256:03d}".encode() + SOH


def extract_frames(buffer):
    frames = []
    while True:
        first = buffer.find(SOH)
        second = buffer.find(SOH, first + 1) if first >= 0 else -1
        if first < 0 or second < 0:
            break
        if not buffer.startswith(b"8=FIX.4.4"):
            raise RuntimeError("raw peer received an invalid BeginString")
        try:
            body_length = int(buffer[first + 3:second])
        except ValueError as error:
            raise RuntimeError("raw peer received an invalid BodyLength") from error
        if body_length < 0 or body_length > 1_000_000:
            raise RuntimeError("raw peer received an out-of-bounds BodyLength")
        checksum_start = second + 1 + body_length
        frame_end = checksum_start + 7
        if len(buffer) < frame_end:
            break
        raw, buffer = buffer[:frame_end], buffer[frame_end:]
        if raw[checksum_start:checksum_start + 3] != b"10=" or raw[-1:] != SOH:
            raise RuntimeError("raw peer received an invalid CheckSum field")
        if sum(raw[:checksum_start]) % 256 != int(raw[checksum_start + 3:checksum_start + 6]):
            raise RuntimeError("raw peer received an invalid FIX checksum")
        fields = {}
        for part in raw[:-1].split(SOH):
            if b"=" in part:
                tag, value = part.split(b"=", 1)
                fields[tag] = value
        frames.append((raw, fields))
    return frames, buffer


class FixPeer:
    def __init__(self, connection):
        self.connection = connection
        self.connection.settimeout(10)
        self.buffer = b""
        self.frames = deque()

    def send(self, sequence, message_type, fields=()):
        self.connection.sendall(make_frame(sequence, message_type, fields))

    def _read_frame(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            if self.frames:
                return self.frames.popleft()
            frames, self.buffer = extract_frames(self.buffer)
            if frames:
                self.frames.extend(frames[1:])
                return frames[0]
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            ready, _, _ = select.select([self.connection], [], [], remaining)
            if not ready:
                return None
            chunk = self.connection.recv(65536)
            if not chunk:
                raise RuntimeError("initiator closed the raw FIX connection")
            self.buffer += chunk

    def receive(self, timeout=10):
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("timed out waiting for a FIX message")
            frame = self._read_frame(remaining)
            if frame is not None:
                return frame[1]

    def expect_type(self, message_type, sequence=None, timeout=10):
        message = self.receive(timeout)
        if message.get(b"35") != message_type.encode():
            raise RuntimeError(f"expected FIX {message_type}, received {message!r}")
        if sequence is not None and int(message[b"34"]) != sequence:
            raise RuntimeError(f"expected FIX sequence {sequence}, got {message!r}")
        if message.get(b"49") != b"BUY" or message.get(b"56") != b"SELL":
            raise RuntimeError(f"initiator FIX identity mismatch: {message!r}")
        return message

    def assert_no_type(self, message_type, duration):
        deadline = time.monotonic() + duration
        while True:
            # Check any frames already decoded before waiting on the socket.
            if self.frames:
                message = self.frames.popleft()[1]
            else:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return
                frame = self._read_frame(remaining)
                if frame is None:
                    return
                message = frame[1]
            if message.get(b"35") == message_type.encode():
                raise RuntimeError(f"unexpected FIX {message_type} without an explicit order command")
            if time.monotonic() >= deadline:
                return


def verify_new_order(message, identifier, symbol, side, quantity, order_type, price=None):
    checks = {b"35": b"D", b"11": identifier.encode(), b"55": symbol.encode(),
        b"54": b"1" if side == "buy" else b"2", b"40": order_type.encode()}
    for tag, expected in checks.items():
        if message.get(tag) != expected:
            raise RuntimeError(f"FIX NewOrderSingle {tag.decode()} mismatch: {message!r}")
    if b"38" not in message or not decimal_equal(message[b"38"].decode(), quantity):
        raise RuntimeError(f"FIX NewOrderSingle quantity mismatch: {message!r}")
    if price is None:
        if b"44" in message:
            raise RuntimeError(f"market NewOrderSingle unexpectedly has Price: {message!r}")
    elif b"44" not in message or not decimal_equal(message[b"44"].decode(), price):
        raise RuntimeError(f"limit NewOrderSingle price mismatch: {message!r}")
    if b"60" not in message:
        raise RuntimeError(f"FIX NewOrderSingle omitted TransactTime: {message!r}")
    return verify_fresh_timestamp(message[b"60"].decode(), datetime.now(timezone.utc))


def send_execution_report(peer, sequence, identifier, symbol, side, quantity):
    side_value = "1" if side == "buy" else "2"
    peer.send(sequence, "8", ((37, f"ORD-{identifier}"), (17, f"EXEC-{identifier}"),
        (11, identifier), (150, "0"), (39, "0"), (55, symbol), (54, side_value),
        (151, quantity), (14, "0"), (6, "0"),
        (60, datetime.now(timezone.utc).strftime("%Y%m%d-%H:%M:%S.%f")[:-3])))


def exercise_cli_and_processes(acceptor, initiator, directory):
    validate_cli(acceptor, initiator, directory)
    server, port = start_acceptor(acceptor, directory / "sell-state", directory)
    clients = []
    try:
        conflict = subprocess.run([str(acceptor), "--state", str(directory / "conflict"),
            "--port", str(port), "--timeout-ms", "200"], capture_output=True,
            text=True, timeout=5, cwd=directory)
        if conflict.returncode != 1:
            raise RuntimeError(f"listener bind conflict did not fail promptly: "
                f"exit={conflict.returncode}; {conflict.stderr}")

        client_state = directory / "buy-state"
        client = start_initiator(initiator, client_state, port, directory)
        clients.append(client)
        active_line = client.wait_text("SESSION_ACTIVE", "initiator session activation", timeout=15)
        del active_line
        client.wait_absent(lambda line: "ACCEPTED ClOrdID=" in line,
            "an automatic order before stdin input", 0.2)
        if client.process.poll() is not None:
            raise RuntimeError("initiator exited after session activation")

        first_cursor = client.mark()
        client.write("order MULTI-1 AAPL buy 100 150.25")
        first_sequence = order_events(client, "MULTI-1", "AAPL", "buy", first_cursor)
        if client.process.poll() is not None:
            raise RuntimeError("initiator exited after its first ExecutionReport")

        status_cursor = client.mark()
        client.write("status")
        client.wait_regex(re.compile(r"\bSTATUS\b"), "status response", after=status_cursor)
        second_cursor = client.mark()
        client.write("order MULTI-2 MSFT sell 2 market")
        second_sequence = order_events(client, "MULTI-2", "MSFT", "sell", second_cursor)
        if second_sequence <= first_sequence:
            raise RuntimeError("outbound sequence did not advance between orders")
        client.write("quit")
        if client.wait_exit(timeout=10) != 0:
            raise RuntimeError(f"initiator quit failed: stdout={client.stdout_lines}; "
                f"stderr={client._details()}")

        # Restart with the same client identity. EOF immediately after an order
        # must still wait for its report and written receipt before Logout.
        restarted = start_initiator(initiator, client_state, port, directory)
        clients.append(restarted)
        restarted.wait_text("SESSION_ACTIVE", "restarted initiator activation", timeout=15)
        restart_cursor = restarted.mark()
        restarted.write("order PERSIST-1 NVDA buy 0.5 900.00")
        restarted.close_stdin()
        restart_sequence = order_events(restarted, "PERSIST-1", "NVDA", "buy", restart_cursor,
            expect_running=False)
        if restart_sequence <= second_sequence:
            raise RuntimeError("outbound sequence reset after persistent client restart")
        if restarted.wait_exit(timeout=10) != 0:
            raise RuntimeError(f"initiator EOF shutdown failed: stdout={restarted.stdout_lines}; "
                f"stderr={restarted._details()}")
        journals = list(client_state.rglob("*.nfxj"))
        if len(journals) != 1:
            raise RuntimeError(f"restarted initiator identity created {len(journals)} journals")

        idle = start_initiator(initiator, client_state, port, directory)
        clients.append(idle)
        idle.wait_text("SESSION_ACTIVE", "idle initiator activation", timeout=15)
        idle.send_signal(signal.SIGTERM)
        idle_status = idle.wait_exit(timeout=10)
        if idle_status not in (0, 128 + signal.SIGTERM):
            raise RuntimeError(f"idle SIGTERM did not shut down cleanly: exit={idle_status}; "
                f"stdout={idle.stdout_lines}; stderr={idle._details()}")
    finally:
        for client in clients:
            if client.process.poll() is None:
                client.terminate()
        if server.process.poll() is None:
            server.send_signal(signal.SIGTERM)
            result = server.wait_exit(timeout=10)
            if result != 0:
                raise RuntimeError(f"acceptor shutdown failed: exit={result}; "
                    f"stdout={server.stdout_lines}; stderr={server._details()}")
        else:
            server.wait_exit(timeout=2)


def exercise_raw_peer(initiator, directory):
    state = directory / "raw-peer-state"
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(4)
        listener.settimeout(10)
        client = start_initiator(initiator, state, listener.getsockname()[1], directory,
            timeout_ms=12_000)
        next_inbound_sequence = 1
        next_outbound_sequence = 1
        connection = None
        peer = None
        try:
            connection, _ = listener.accept()
            peer = FixPeer(connection)
            logon = peer.expect_type("A", sequence=next_outbound_sequence)
            if logon.get(b"98") != b"0" or b"108" not in logon:
                raise RuntimeError(f"invalid initiator Logon: {logon!r}")
            peer.send(next_inbound_sequence, "A", ((98, 0), (108, 30)))
            next_inbound_sequence += 1
            client.wait_text("SESSION_ACTIVE", "raw-peer initiator activation", timeout=15)
            peer.assert_no_type("D", 0.2)
            if client.process.poll() is not None:
                raise RuntimeError("raw-peer initiator exited while stdin was idle")

            client.write("order BAD-1 AAPL hold 1 12.50")
            peer.assert_no_type("D", 0.2)
            if client.process.poll() is not None:
                raise RuntimeError("invalid business command terminated the initiator")

            first_time = datetime.now(timezone.utc)
            first_cursor = client.mark()
            client.write("order RAW-1 AAPL buy 5 12.50")
            first_order = peer.expect_type("D", timeout=10)
            verify_new_order(first_order, "RAW-1", "AAPL", "buy", "5", "2", "12.50")
            first_out_sequence = int(first_order[b"34"])
            send_execution_report(peer, next_inbound_sequence, "RAW-1", "AAPL", "buy", "5")
            next_inbound_sequence += 1
            order_events(client, "RAW-1", "AAPL", "buy", first_cursor)
            if client.process.poll() is not None:
                raise RuntimeError("initiator exited after the first raw-peer report")

            # One order can receive multiple reports. A trade update is not
            # another submit command and must not be rejected as a duplicate.
            for status, leaves, cumulative in (("1", "3", "2"), ("2", "0", "5")):
                after = client.mark()
                peer.send(next_inbound_sequence, "8", ((37, "ORD-RAW-1"),
                    (17, f"TRADE-{status}"), (11, "RAW-1"), (150, "F"), (39, status),
                    (55, "AAPL"), (54, "1"), (38, "5"), (151, leaves),
                    (14, cumulative), (6, "12.50"), (32, "2" if status == "1" else "3"),
                    (31, "12.50")))
                next_inbound_sequence += 1
                client.wait_regex(re.compile(rf"EXECUTION_REPORT ClOrdID=RAW-1 .*OrdStatus={status}"),
                    "later execution report", after=after)
                client.wait_text("ORDER_RESULT ClOrdID=RAW-1 state=response",
                    "trade update correlation", after=after)

            # Force a transport reconnect after a fully reported order. The
            # reconnect itself must not create another application order.
            connection.shutdown(socket.SHUT_RDWR)
            connection.close()
            connection, _ = listener.accept()
            peer = FixPeer(connection)
            reconnect_logon = peer.expect_type("A", timeout=12)
            reconnect_sequence = int(reconnect_logon[b"34"])
            if reconnect_sequence != first_out_sequence + 1:
                raise RuntimeError(f"unexpected client sequence on reconnect: "
                    f"first order={first_out_sequence}, Logon={reconnect_sequence}")
            peer.send(next_inbound_sequence, "A", ((98, 0), (108, 30)))
            next_inbound_sequence += 1
            peer.assert_no_type("D", 0.2)
            if client.process.poll() is not None:
                raise RuntimeError("initiator exited during a no-command reconnect")

            second_cursor = client.mark()
            client.write("order RAW-2 MSFT sell 0.5 market")
            second_order = peer.expect_type("D", timeout=10)
            verify_new_order(second_order, "RAW-2", "MSFT", "sell", "0.5", "1")
            second_out_sequence = int(second_order[b"34"])
            if second_out_sequence != reconnect_sequence + 1:
                raise RuntimeError(f"client order sequence was not monotonic: {second_order!r}")
            first_report_time = verify_fresh_timestamp(first_order[b"60"].decode(), first_time)
            second_time = verify_fresh_timestamp(second_order[b"60"].decode(),
                datetime.now(timezone.utc))
            if second_time <= first_report_time:
                raise RuntimeError("TransactTime did not advance for the later order")

            client.write("quit")
            receipt_pattern = re.compile(r"\bRECEIPT\s+ClOrdID=RAW-2\s+state=written\s+sequence=\d+\b")
            client.wait_regex(receipt_pattern, "second raw order written receipt", after=second_cursor)
            peer.assert_no_type("5", 0.2)
            if client.process.poll() is not None:
                raise RuntimeError("quit sent Logout before the outstanding report was received")

            send_execution_report(peer, next_inbound_sequence, "RAW-2", "MSFT", "sell", "0.5")
            next_inbound_sequence += 1
            order_events(client, "RAW-2", "MSFT", "sell", second_cursor, expect_running=False)
            logout = peer.expect_type("5", sequence=second_out_sequence + 1, timeout=10)
            peer.send(next_inbound_sequence, "5", ((58, "shutdown"),))
            next_inbound_sequence += 1
            if client.wait_exit(timeout=10) != 0:
                raise RuntimeError(f"raw-peer initiator quit failed: stdout={client.stdout_lines}; "
                    f"stderr={client._details()}; Logout={logout!r}")
        finally:
            if connection is not None:
                try:
                    connection.close()
                except OSError:
                    pass
            if client.process.poll() is None:
                client.terminate()


def exercise_idle_sigterm(initiator, directory):
    state = directory / "signal-peer-state"
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(10)
        client = start_initiator(initiator, state, listener.getsockname()[1], directory,
            timeout_ms=12_000)
        connection = None
        try:
            connection, _ = listener.accept()
            peer = FixPeer(connection)
            peer.expect_type("A", sequence=1)
            peer.send(1, "A", ((98, 0), (108, 30)))
            client.wait_text("SESSION_ACTIVE", "idle raw-peer session activation", timeout=15)
            peer.assert_no_type("D", 0.2)
            client.send_signal(signal.SIGTERM)
            peer.expect_type("5", sequence=2, timeout=10)
            peer.send(2, "5", ((58, "shutdown"),))
            if client.wait_exit(timeout=10) not in (0, 128 + signal.SIGTERM):
                raise RuntimeError(f"idle SIGTERM initiator exited unsuccessfully: "
                    f"stdout={client.stdout_lines}; stderr={client._details()}")
        finally:
            if connection is not None:
                try:
                    connection.close()
                except OSError:
                    pass
            if client.process.poll() is None:
                client.terminate()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--acceptor", type=Path, required=True)
    parser.add_argument("--initiator", type=Path, required=True)
    args = parser.parse_args()
    acceptor = args.acceptor.resolve()
    initiator = args.initiator.resolve()
    with tempfile.TemporaryDirectory(prefix="nimblefix-interactive-endpoints-") as temporary:
        directory = Path(temporary)
        exercise_cli_and_processes(acceptor, initiator, directory)
        exercise_raw_peer(initiator, directory)
        exercise_idle_sigterm(initiator, directory)
    print("interactive orders, persistence, reconnect, independent raw peer, and graceful shutdown passed")


if __name__ == "__main__":
    main()
