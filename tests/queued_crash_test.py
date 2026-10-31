#!/usr/bin/env python3
"""Kill after a queued dependent reply is on wire, before delivery completion."""
import argparse
import json
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
from cli_test import Events, Reader, frame


def start(binary, root, phase, strategy):
    child = subprocess.Popen([str(binary), str(root), phase, strategy], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    events = Events(child.stdout)
    ready = events.read()
    assert ready['event'] == 'listening'
    peer = socket.create_connection(('127.0.0.1', ready['port']), timeout=5)
    return child, events, peer, Reader(peer)


def test(binary, root, strategy):
    child, events, peer, reader = start(binary, root, 'first', strategy)
    try:
        peer.sendall(frame(1, 'A', ((98, 0), (108, 30))))
        assert reader.read()[b'35'] == b'A'
        peer.sendall(frame(2, 'D', ((11, 'RECOVER-ME'), (55, 'AAPL'))))
        callback = events.read()
        assert callback['event'] == 'callback' and not callback['recovery']
        response = reader.read()
        assert response[b'35'] == b'8' and response[b'34'] == b'2' and response[b'11'] == b'RECOVER-ME'
        child.kill(); child.communicate(timeout=5)
        assert child.returncode == -signal.SIGKILL
    finally:
        peer.close()
        if child.poll() is None: child.kill(); child.communicate()
    child, events, peer, reader = start(binary, root, 'recover', strategy)
    try:
        peer.sendall(frame(3, 'A', ((98, 0), (108, 30), (369, 2))))
        assert reader.read()[b'35'] == b'A'
        recovered = events.read()
        assert recovered['event'] == 'callback' and recovered['recovery']
        assert recovered['input_id'] == callback['input_id'] and recovered['sequence'] == 2
        peer.sendall(frame(4, '2', ((7, 2), (16, 2))))
        replay = reader.read()
        assert replay[b'35'] == b'8' and replay[b'34'] == b'2' and replay[b'43'] == b'Y'
        assert replay[b'11'] == b'RECOVER-ME' and replay[b'17'] == b'DURABLE-REPLY' and b'122' in replay
        child.send_signal(signal.SIGTERM)
        assert reader.read()[b'35'] == b'5'
        peer.sendall(frame(5, '5'))
        stdout, stderr = child.communicate(timeout=10)
        assert child.returncode == 0, (child.returncode, stdout, stderr)
        verified = next(json.loads(line) for line in stdout.splitlines() if json.loads(line).get('event') == 'verified')
        assert verified['delivered'] and verified['replies'] == 1
    finally:
        peer.close()
        if child.poll() is None: child.kill(); child.communicate()


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary',type=Path,required=True)
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='nimblefix-queued-crash-') as temporary:
        for strategy in ('journal','ring'):
            root=Path(temporary)/strategy; root.mkdir()
            test(args.binary.resolve(),root,strategy)
    print('Queued journal/ring SIGKILL recovery preserved input id, one durable reply and completion ordering')


if __name__=='__main__':main()
