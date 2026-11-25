#!/usr/bin/env python3
"""Validate deployed CLI contracts, real TCP authentication and secret storage."""
import argparse
import datetime
import json
import os
from pathlib import Path
import selectors
import signal
import socket
import subprocess
import tempfile
import time


def frame(sequence, kind, fields=()):
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H:%M:%S.%f")[:-3]
    body = f"35={kind}\x0149=BUY\x0156=SELL\x0134={sequence}\x0152={stamp}\x01".encode()
    body += b"".join(f"{tag}={value}\x01".encode() for tag, value in fields)
    raw = b"8=FIX.4.4\x019=" + str(len(body)).encode() + b"\x01" + body
    return raw + f"10={sum(raw) % 256:03d}\x01".encode()


class Reader:
    def __init__(self, stream): self.stream, self.buffer = stream, b""
    def read(self):
        while True:
            first = self.buffer.find(b"\x01")
            second = self.buffer.find(b"\x01", first + 1)
            if first >= 0 and second >= 0:
                length = int(self.buffer[first + 3:second])
                size = second + 1 + length + 7
                if len(self.buffer) >= size:
                    raw, self.buffer = self.buffer[:size], self.buffer[size:]
                    assert sum(raw[:-7]) % 256 == int(raw[-4:-1])
                    return dict(field.split(b"=", 1) for field in raw[:-1].split(b"\x01"))
            chunk = self.stream.recv(65536)
            if not chunk: raise RuntimeError("unexpected peer EOF")
            self.buffer += chunk


class Events:
    def __init__(self, pipe):
        self.pipe, self.pending = pipe, b""
        self.selector = selectors.DefaultSelector()
        self.selector.register(pipe, selectors.EVENT_READ)
    def read(self, timeout=10):
        deadline = time.monotonic() + timeout
        while True:
            if b"\n" in self.pending:
                line, self.pending = self.pending.split(b"\n", 1)
                return json.loads(line)
            if not self.selector.select(max(0, deadline - time.monotonic())):
                raise RuntimeError("CLI event timed out")
            chunk = os.read(self.pipe.fileno(), 4096)
            if not chunk: raise RuntimeError("CLI exited before the expected event")
            self.pending += chunk
            if len(self.pending) > 65536: raise RuntimeError("unbounded CLI event")


def execute(binary, root, *arguments, expected=0, env=None):
    result = subprocess.run([str(binary), *map(str, arguments)], cwd=root, env=env,
        text=True, capture_output=True, timeout=20)
    if result.returncode != expected:
        raise RuntimeError(f"CLI {arguments}: exit {result.returncode}, expected {expected}\n{result.stdout}\n{result.stderr}")
    return result


def test(binary, root):
    execute(binary, root, "--help")
    assert "0.3.0" in execute(binary, root, "--version").stdout
    doctor = json.loads(execute(binary, root, "doctor", "--json").stdout)
    assert doctor["confirmation"] == "appended" and "queued" in doctor["dispatch"]
    config_dir = root / "配置 with spaces"
    created = execute(binary, root, "init", "--directory", config_dir, "--json")
    assert Path(json.loads(created.stdout)["acceptor"]).is_file()
    original = (config_dir / "acceptor.toml").read_bytes()
    execute(binary, root, "init", "--directory", config_dir, expected=2)
    assert (config_dir / "acceptor.toml").read_bytes() == original
    assert json.loads(execute(binary, root, "validate", "--config", config_dir / "acceptor.toml", "--json").stdout)["valid"]
    assert not (config_dir / "state").exists(), "offline validate opened the store"

    failover_toml = config_dir / "failover_initiator.toml"
    failover_toml.write_text('''version = 1
[engine]
dispatch = "inline"
io_backend = "epoll"
idle_strategy = "hybrid_backoff"
idle_spin_iterations = 5000
idle_yield_iterations = 500
shards = 4
cpu_cores = [0, 1, 2, 3]
[engine.storage]
directory = "state-failover"
[[sessions]]
name = "venue"
role = "initiator"
sender_comp_id = "BUY"
target_comp_id = "SELL"
endpoints = ["127.0.0.1:19876", "127.0.0.1:19877"]
[sessions.connection_strategy]
policy = "ordered"
initial_backoff_ms = 100
max_backoff_ms = 5000
backoff_multiplier = 2
max_retries = 10
reset_to_primary_on_success = true
immediate_failover = true
''')
    assert json.loads(execute(binary, root, "validate", "--config", failover_toml, "--json").stdout)["valid"]
    if doctor.get("fix44"):
        typed = config_dir / "typed.toml"
        typed_content = original.decode().replace('schema = "none"', 'schema = "fix44"')
        typed.write_text(typed_content)
        execute(binary, root, "validate", "--config", typed, "--json")
        typed.write_text(typed_content.replace('transport = "FIX.4.4"', 'transport = "FIX.4.2"'))
        mismatch = execute(binary, root, "validate", "--config", typed, "--json", expected=2)
        assert "dictionary version" in mismatch.stderr
        if doctor.get("xml"):
            dictionaries = Path(__file__).resolve().parents[1] / "schemas/standard"
            def dictionary_config(name):
                return typed_content.replace('[engine.storage]', 'dictionary = ' + json.dumps(str(dictionaries / name)) + '\n[engine.storage]')
            typed.write_text(dictionary_config("FIX44.xml"))
            execute(binary, root, "validate", "--config", typed, "--json")
            typed.write_text(dictionary_config("FIX42.xml"))
            mismatch = execute(binary, root, "validate", "--config", typed, "--json", expected=2)
            assert "fingerprint" in mismatch.stderr

    execute(binary, root, "run", "--config", config_dir / "acceptor.toml", "--duration-ms", "1x", expected=2)
    execute(binary, root, "unknown-command", expected=2)

    bad = config_dir / "bad.toml"
    bad.write_text('''version = 2
[engine]
dispatch = "mistyped"
unknown_key = true
[engine.storage]
directory = "state"
confirmation = "synced"
[[sessions]]
name = "bad"
role = "initiator"
sender_comp_id = "BUY"
target_comp_id = "SELL"
endpoint = "127.0.0.1:65537"
[sessions.logon]
password = "SHOULD-NEVER-APPEAR"
''')
    failed = execute(binary, root, "validate", "--config", bad, "--json", expected=2)
    diagnostics = json.loads(failed.stderr)
    assert len(diagnostics) >= 4 and all("field" in item for item in diagnostics)
    assert "SHOULD-NEVER-APPEAR" not in failed.stdout + failed.stderr

    secret = "CLI-SECRET-DO-NOT-JOURNAL"
    raw_path = root / "logon.fix"
    raw_path.write_bytes(frame(1, "A", ((98, 0), (108, 30), (553, "demo"), (554, secret))))
    inspected = execute(binary, root, "inspect", "frame", "--path", raw_path, "--json")
    assert secret not in inspected.stdout + inspected.stderr
    assert any(field["tag"] == 554 and field["value"] == "<redacted>" for field in json.loads(inspected.stdout)[0]["fields"])

    config = config_dir / "acceptor.toml"
    content = original.decode().replace("127.0.0.1:9876", "127.0.0.1:0")
    content += '''
timestamp_precision = "us"
verify_checksum = true
[sessions.authentication]
method = "credentials"
username = "demo"
password_env = "NIMBLEFIX_CLI_TEST_PASSWORD"
require_tls = false
'''
    config.write_text(content)
    environment = os.environ.copy()
    environment["NIMBLEFIX_CLI_TEST_PASSWORD"] = secret
    execute(binary, root, "validate", "--config", config, "--json", env=environment)
    server = subprocess.Popen([str(binary), "run", "--config", str(config), "--json"], cwd=root,
        env=environment, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        events = Events(server.stdout)
        event = events.read()
        while event.get("event") != "listening": event = events.read()
        assert event["event"] == "listening" and event["port"] > 0
        with socket.create_connection(("127.0.0.1", event["port"]), timeout=3) as rejected:
            rejected.sendall(frame(1, "A", ((98, 0), (108, 30), (553, "demo"), (554, "wrong"))))
            assert rejected.recv(1024) == b"", "bad credentials admitted"
        assert not list((config_dir / "state").rglob("*.nfxj")), "unauthenticated connection opened a journal"
        with socket.create_connection(("127.0.0.1", event["port"]), timeout=3) as peer:
            peer.sendall(raw_path.read_bytes())
            reader = Reader(peer)
            logon_reply = reader.read()
            assert logon_reply[b"35"] == b"A"
            assert len(logon_reply[b"52"]) == 24, f"expected microsecond 52=, got {logon_reply[b'52']!r}"
            peer.sendall(frame(2, "D", ((11, "CLI-ORDER"), (55, "AAPL"), (54, 1), (38, 100), (40, 1))))
            observed = events.read()
            while observed.get("event") != "message": observed = events.read()
            assert observed["business_messages"] == 1
            server.send_signal(signal.SIGTERM)
            assert reader.read()[b"35"] == b"5"
            peer.sendall(frame(3, "5"))
        stdout, stderr = server.communicate(timeout=10)
        assert server.returncode == 128 + signal.SIGTERM, (server.returncode, stdout, stderr)
        assert secret not in stdout + stderr
        stopped = next(json.loads(line) for line in stdout.splitlines() if json.loads(line).get("event") == "stopped")
        assert stopped["code"] == 0 and stopped["business_messages"] == 1
        files = [path for path in (config_dir / "state").rglob("*") if path.is_file()]
        assert files
        for path in files: assert secret.encode() not in path.read_bytes(), f"credential leaked to {path.name}"
    finally:
        if server.poll() is None: server.kill(); server.communicate()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nimblefix-cli-") as temporary:
        test(args.binary.resolve(), Path(temporary))
    print("CLI offline validation, diagnostics, authentication, shutdown, paths and credential redaction passed")


if __name__ == "__main__": main()
