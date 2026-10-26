#!/usr/bin/env python3
"""An independent FIX peer forces partial receive, EOF, high Logon and replay."""
import argparse
import datetime
import json
from pathlib import Path
import socket
import subprocess
import tempfile


def timestamp():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H:%M:%S.%f")[:-3]


def frame(seq, msgtype, fields=(), original=None):
    now = timestamp()
    body = f"35={msgtype}\x0149=SELL\x0156=BUY\x0134={seq}\x01".encode()
    if original: body += b"43=Y\x01"
    body += f"52={now}\x01".encode()
    if original: body += f"122={original}\x01".encode()
    body += b"".join(f"{tag}={value}\x01".encode() for tag, value in fields)
    prefix = b"8=FIX.4.4\x019=" + str(len(body)).encode() + b"\x01" + body
    return prefix + f"10={sum(prefix)%256:03d}\x01".encode()


class Peer:
    def __init__(self, connection):
        self.connection = connection
        connection.settimeout(5)
        self.buffer = b""

    def read(self):
        while True:
            first = self.buffer.find(b"\x01")
            second = self.buffer.find(b"\x01", first + 1)
            if first >= 0 and second >= 0:
                if not self.buffer.startswith(b"8=FIX.4.4\x019="): raise AssertionError("bad FIX framing")
                end = second + 1 + int(self.buffer[first + 3:second]) + 7
                if len(self.buffer) >= end:
                    raw, self.buffer = self.buffer[:end], self.buffer[end:]
                    if sum(raw[:-7]) % 256 != int(raw[-4:-1]): raise AssertionError("checksum")
                    return dict(field.split(b"=", 1) for field in raw[:-1].split(b"\x01"))
            received = self.connection.recv(65536)
            if not received: raise AssertionError("unexpected peer EOF")
            self.buffer += received


def expect(peer, msgtype, sequence):
    message = peer.read()
    assert message[b"35"] == msgtype.encode(), message
    assert int(message[b"34"]) == sequence, message
    assert message[b"49"] == b"BUY" and message[b"56"] == b"SELL", message
    return message


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nimblefix-raw-peer-") as directory:
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0)); listener.listen(2); listener.settimeout(8)
            child = subprocess.Popen([str(args.binary.resolve()), str(listener.getsockname()[1]), directory],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                connection, first_address = listener.accept()
                with connection:
                    peer = Peer(connection)
                    expect(peer, "A", 1)
                    connection.sendall(frame(1, "A", ((98, 0), (108, 30))))
                    assert expect(peer, "D", 2)[b"11"] == b"1"
                    incomplete = frame(2, "8", ((11, 1),))
                    original = dict(field.split(b"=", 1) for field in incomplete[:-1].split(b"\x01"))[b"52"].decode()
                    connection.sendall(incomplete[:len(incomplete)//2])
                    # No accepted inbound message exists for that partial frame.
                connection, second_address = listener.accept()
                with connection:
                    peer = Peer(connection)
                    expect(peer, "A", 3)
                    # The prior report had sequence 2. Its successor Logon is
                    # therefore higher than NimbleFIX's persisted NextNumIn.
                    connection.sendall(frame(3, "A", ((98, 0), (108, 30))))
                    request = expect(peer, "2", 4)
                    assert request[b"7"] == b"2" and request[b"16"] == b"2", request
                    connection.sendall(frame(2, "8", ((11, 1),), original=original))
                    assert expect(peer, "D", 5)[b"11"] == b"2"
                    connection.sendall(frame(4, "8", ((11, 2),)))
                    expect(peer, "5", 6)
                    connection.sendall(frame(5, "5"))
                stdout, stderr = child.communicate(timeout=8)
                assert child.returncode == 0, (stdout, stderr)
                result = json.loads(stdout)
                assert result["active"] == 2 and result["reports"] == 2, result
                assert len(list(Path(directory).rglob("*.nfxj"))) == 1
                assert first_address[1] != second_address[1]
                print("independent TCP half-frame EOF, reconnect, high Logon, replay and Logout passed")
            finally:
                if child.poll() is None:
                    child.kill(); child.communicate()


if __name__ == "__main__": main()
