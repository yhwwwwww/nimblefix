#!/usr/bin/env python3
"""Exercise the public examples as separate, independently deployed processes."""
import argparse
from pathlib import Path
import re
import selectors
import socket
import subprocess
import tempfile


def validate_cli(acceptor, initiator, directory):
    for binary in (acceptor, initiator):
        help_result = subprocess.run([str(binary), "--help"], capture_output=True, timeout=3)
        if help_result.returncode: raise RuntimeError("example --help failed")
        for arguments in (("--port", "1x"), ("--port", "65536"), ("--port", "-1"),
                ("--timeout-ms", "0"), ("--host", "localhost"), ("--unknown", "x")):
            result = subprocess.run([str(binary), "--state", str(directory / "invalid"),
                "--port", "12345", *arguments], capture_output=True, timeout=3)
            if result.returncode != 2: raise RuntimeError(f"invalid CLI accepted: {arguments}")
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        unavailable_port = reservation.getsockname()[1]
        # Leave it bound without listen: connection refusal is deterministic.
        failed = subprocess.run([str(initiator), "--state", str(directory / "unavailable"),
            "--port", str(unavailable_port), "--timeout-ms", "150"], capture_output=True, timeout=5)
        if failed.returncode != 1: raise RuntimeError("unavailable endpoint did not fail within its deadline")


def exercise(acceptor, initiator, directory):
    validate_cli(acceptor, initiator, directory)
    server = subprocess.Popen([str(acceptor), "--state", str(directory / "sell"),
        "--port", "0", "--timeout-ms", "1000"], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=directory)
    try:
        ready = selectors.DefaultSelector()
        ready.register(server.stdout, selectors.EVENT_READ)
        if not ready.select(10): raise RuntimeError("acceptor did not publish its endpoint")
        line = server.stdout.readline()
        match = re.fullmatch(r"LISTENING 127\.0\.0\.1:(\d+)\n", line)
        if not match: raise RuntimeError(f"invalid acceptor endpoint: {line!r}")
        conflicting = subprocess.run([str(acceptor), "--state", str(directory / "conflict"),
            "--port", match[1], "--timeout-ms", "150"], capture_output=True, timeout=5)
        if conflicting.returncode != 1: raise RuntimeError("listener bind conflict did not fail promptly")
        for index in range(2):
            identifier = f"EXTERNAL-{index}"
            client = subprocess.run([str(initiator), "--state", str(directory / "buy"),
                "--port", match[1], "--cl-ord-id", identifier, "--timeout-ms", "5000"],
                text=True, capture_output=True, timeout=18, cwd=directory)
            if client.returncode or "received matching New ExecutionReport" not in client.stdout:
                raise RuntimeError(f"initiator failed: {client.stdout}\n{client.stderr}")
            if identifier not in client.stdout: raise RuntimeError("wrong order correlation")
        server.terminate()
        stdout, stderr = server.communicate(timeout=8)
        if server.returncode: raise RuntimeError(f"acceptor failed: {stdout}\n{stderr}")
        if "EXTERNAL-0" not in stdout or "EXTERNAL-1" not in stdout:
            raise RuntimeError(f"acceptor did not process both orders: {stdout}")
        if len(list((directory / "buy").rglob("*.nfxj"))) != 1: raise RuntimeError("initiator identity was not stable")
        if len(list((directory / "sell").rglob("*.nfxj"))) != 1: raise RuntimeError("acceptor identity was not stable")
    finally:
        if server.poll() is None: server.kill(); server.communicate()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--acceptor", type=Path, required=True)
    parser.add_argument("--initiator", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nimblefix-two-process-") as temporary:
        exercise(args.acceptor.resolve(), args.initiator.resolve(), Path(temporary))
    print("separate acceptor/initiator processes and persistent client restart passed")


if __name__ == "__main__": main()
