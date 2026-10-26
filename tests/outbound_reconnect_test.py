#!/usr/bin/env python3
"""The independent peer loses half an outbound order and requests its original sequence."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile
from initiator_reconnect_test import Peer, expect, frame


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nimblefix-outbound-reconnect-") as directory:
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0)); listener.listen(2); listener.settimeout(8)
            child = subprocess.Popen([str(args.binary.resolve()), str(listener.getsockname()[1]), directory],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                connection, _ = listener.accept()
                with connection:
                    peer = Peer(connection)
                    expect(peer, "A", 1)
                    connection.sendall(frame(1, "A", ((98, 0), (108, 30))))
                    partial = b""
                    connection.settimeout(5)
                    while partial.count(b"\x01") < 2:
                        chunk = connection.recv(1)
                        assert chunk
                        partial += chunk
                    first = partial.find(b"\x01")
                    total = len(partial) + int(partial[first + 3:-1]) + 7
                    while len(partial) < total // 2:
                        chunk = connection.recv(total // 2 - len(partial))
                        assert chunk
                        partial += chunk
                    # Close before a complete FIX frame is accepted. The sender
                    # has already appended the full order and its seq decision.
                connection, _ = listener.accept()
                with connection:
                    peer = Peer(connection)
                    expect(peer, "A", 3)
                    connection.sendall(frame(2, "A", ((98, 0), (108, 30))) +
                        frame(3, "2", ((7, 2), (16, 2))))
                    orders = {}
                    while len(orders) != 2:
                        message = peer.read()
                        assert message[b"35"] == b"D", message
                        orders[int(message[b"34"])] = message
                    assert set(orders) == {2, 4}, orders
                    assert orders[2][b"11"] == b"1" and orders[2][b"43"] == b"Y", orders[2]
                    assert orders[2][b"122"] <= orders[2][b"52"], orders[2]
                    assert orders[4][b"11"] == b"2" and b"43" not in orders[4], orders[4]
                    connection.sendall(frame(4, "8", ((11, 1),)) + frame(5, "8", ((11, 2),)))
                    expect(peer, "5", 5)
                    connection.sendall(frame(6, "5"))
                stdout, stderr = child.communicate(timeout=8)
                assert child.returncode == 0, (stdout, stderr)
                result = json.loads(stdout)
                assert result["active"] == 2 and result["reports"] == 2, result
                assert len(list(Path(directory).rglob("*.nfxj"))) == 1
                print("independent TCP partial outbound loss and original-sequence PossDup replay passed")
            finally:
                if child.poll() is None: child.kill(); child.communicate()


if __name__ == "__main__": main()
