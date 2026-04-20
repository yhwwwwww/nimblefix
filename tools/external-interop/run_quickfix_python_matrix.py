#!/usr/bin/env python3

import pathlib
import shutil
import signal
import socket
import subprocess
import sys
import time


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILD_DIR = REPO_ROOT / "build"
ARTIFACT_PATH = BUILD_DIR / "sample-basic.art"
DICTGEN_BIN = REPO_ROOT / "build/linux/x86_64/release/nimblefix-dictgen"
ACCEPTOR_BIN = REPO_ROOT / "build/linux/x86_64/release/nimblefix-acceptor"
INITIATOR_BIN = REPO_ROOT / "build/linux/x86_64/release/nimblefix-initiator"
QUICKFIX_IMAGE = "optimalflow/python-quickfix:3.12"
QUICKFIX_SCRIPT = "/work/tools/external-interop/quickfix_python_echo.py"
WORK_ROOT = BUILD_DIR / "external-interop"


def ensure_prerequisites() -> None:
    required = [DICTGEN_BIN, ACCEPTOR_BIN, INITIATOR_BIN]
    missing = [path for path in required if not path.exists()]
    if missing:
        raise RuntimeError(
            "required release binaries are missing; run xmake first: " + ", ".join(str(path) for path in missing))

    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    if not ARTIFACT_PATH.exists():
        run_checked([
            str(DICTGEN_BIN),
            "--input",
            str(REPO_ROOT / "samples/basic_profile.ffd"),
            "--output",
            str(ARTIFACT_PATH),
        ], name="dictgen")


def run_checked(command, name: str) -> None:
    print(f"==> {name}: {' '.join(command)}", flush=True)
    result = subprocess.run(command, cwd=REPO_ROOT, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"{name} failed with exit code {result.returncode}")


def start_process(command, name: str) -> subprocess.Popen:
    print(f"==> {name}: {' '.join(command)}", flush=True)
    return subprocess.Popen(command, cwd=REPO_ROOT)


def wait_for_port(host: str, port: int, timeout_seconds: float, process: subprocess.Popen | None = None) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        if process is not None and process.poll() is not None:
            raise RuntimeError(f"process exited before port {host}:{port} became ready")
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.settimeout(0.2)
            if sock.connect_ex((host, port)) == 0:
                return
        time.sleep(0.1)
    raise RuntimeError(f"timed out waiting for {host}:{port}")


def stop_process(process: subprocess.Popen | None, name: str) -> None:
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
    print(f"<== {name}: exit={process.returncode}", flush=True)


def quickfix_command(mode: str, port: int, sender: str, target: str, work_dir: pathlib.Path, expected_party_id: str,
                     outbound_party_id: str) -> list[str]:
    return [
        "docker",
        "run",
        "--rm",
        "--network",
        "host",
        "-v",
        f"{REPO_ROOT}:/work",
        "-w",
        "/work",
        QUICKFIX_IMAGE,
        "python",
        QUICKFIX_SCRIPT,
        "--mode",
        mode,
        "--host",
        "127.0.0.1",
        "--port",
        str(port),
        "--sender",
        sender,
        "--target",
        target,
        "--work-dir",
        str(pathlib.Path("/work") / work_dir.relative_to(REPO_ROOT)),
        "--expected-party-id",
        expected_party_id,
        "--outbound-party-id",
        outbound_party_id,
        "--timeout-seconds",
        "20",
    ]


def run_matrix() -> None:
    ensure_prerequisites()
    shutil.rmtree(WORK_ROOT, ignore_errors=True)
    WORK_ROOT.mkdir(parents=True, exist_ok=True)

    acceptor = None
    try:
        port = 9945
        acceptor = start_process([
            str(ACCEPTOR_BIN),
            "--artifact",
            str(ARTIFACT_PATH),
            "--bind",
            "127.0.0.1",
            "--port",
            str(port),
            "--sender",
            "SELL",
            "--target",
            "BUY",
        ], name="nimblefix-acceptor")
        wait_for_port("127.0.0.1", port, 10.0, acceptor)
        run_checked(
            quickfix_command(
                mode="initiator",
                port=port,
                sender="BUY",
                target="SELL",
                work_dir=WORK_ROOT / "quickfix-initiator",
                expected_party_id="QF-INITIATOR",
                outbound_party_id="QF-INITIATOR",
            ),
            name="quickfix-python-initiator",
        )
    finally:
        stop_process(acceptor, "nimblefix-acceptor")

    quickfix_acceptor = None
    try:
        port = 9946
        quickfix_acceptor = start_process(
            quickfix_command(
                mode="acceptor",
                port=port,
                sender="SELL",
                target="BUY",
                work_dir=WORK_ROOT / "quickfix-acceptor",
                expected_party_id="INITIATOR-PARTY",
                outbound_party_id="QF-ACCEPTOR",
            ),
            name="quickfix-python-acceptor",
        )
        wait_for_port("127.0.0.1", port, 10.0, quickfix_acceptor)
        run_checked([
            str(INITIATOR_BIN),
            "--artifact",
            str(ARTIFACT_PATH),
            "--host",
            "127.0.0.1",
            "--port",
            str(port),
            "--sender",
            "BUY",
            "--target",
            "SELL",
        ], name="nimblefix-initiator")
        exit_code = quickfix_acceptor.wait(timeout=20)
        if exit_code != 0:
            raise RuntimeError(f"quickfix-python-acceptor failed with exit code {exit_code}")
        print(f"<== quickfix-python-acceptor: exit={exit_code}", flush=True)
    finally:
        stop_process(quickfix_acceptor, "quickfix-python-acceptor")


def main() -> int:
    try:
        run_matrix()
    except Exception as exc:
        print(str(exc), file=sys.stderr, flush=True)
        return 1
    print("quickfix-python-matrix-ok", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())