#!/usr/bin/env python3
"""Independent raw peers verify both roles for every declared transport version."""
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

SOH = b'\x01'


def frame(version, sequence, kind, fields=()):
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H:%M:%S.%f')[:-3]
    body = f'35={kind}\x0149=BUY\x0156=SELL\x0134={sequence}\x0152={stamp}\x01'.encode()
    body += b''.join(f'{tag}={value}\x01'.encode() for tag, value in fields)
    prefix = f'8={version}\x019={len(body)}\x01'.encode() + body
    return prefix + f'10={sum(prefix) % 256:03d}\x01'.encode()


class Reader:
    def __init__(self, stream): self.stream, self.buffer = stream, b''
    def read(self):
        while True:
            first = self.buffer.find(SOH)
            second = self.buffer.find(SOH, first + 1) if first >= 0 else -1
            if second >= 0:
                total = second + 1 + int(self.buffer[first + 3:second]) + 7
                if len(self.buffer) >= total:
                    raw, self.buffer = self.buffer[:total], self.buffer[total:]
                    assert raw[-7:-4] == b'10=' and sum(raw[:-7]) % 256 == int(raw[-4:-1])
                    return dict(part.split(b'=', 1) for part in raw[:-1].split(SOH))
            chunk = self.stream.recv(65536)
            if not chunk: raise RuntimeError('peer closed before expected FIX frame')
            self.buffer += chunk


class Events:
    def __init__(self, stream):
        self.stream, self.buffer = stream, b''
        self.selector = selectors.DefaultSelector()
        self.selector.register(stream, selectors.EVENT_READ)
    def until(self, kind):
        end = time.monotonic() + 10
        while time.monotonic() < end:
            if b'\n' in self.buffer:
                line, self.buffer = self.buffer.split(b'\n', 1)
                event = json.loads(line)
                if event.get('event') == kind: return event
                continue
            if not self.selector.select(max(0, end - time.monotonic())): break
            chunk = os.read(self.stream.fileno(), 4096)
            if not chunk: break
            self.buffer += chunk
        raise RuntimeError(f'missing CLI {kind} event')


def exchange(binary, parent, version, role, dispatch):
    directory = parent / f'{version}-{role}-{dispatch}'
    directory.mkdir()
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        listener.settimeout(10)
        config = directory / 'session.toml'
        common = f'''version = 1
[engine]
dispatch = "{dispatch}"
schema = "none"
logout_timeout_ms = 1000
[engine.storage]
directory = "./state"
confirmation = "appended"
'''
        if role == 'acceptor':
            declaration = f'''[[listeners]]
name = "main"
endpoint = "127.0.0.1:0"
[[sessions]]
name = "peer"
role = "acceptor"
listener = "main"
local_comp_id = "SELL"
remote_comp_id = "BUY"
transport = "{version}"
'''
        else:
            declaration = f'''[[sessions]]
name = "peer"
role = "initiator"
sender_comp_id = "SELL"
target_comp_id = "BUY"
endpoint = "127.0.0.1:{listener.getsockname()[1]}"
transport = "{version}"
'''
        config.write_text(common + declaration)
        process = subprocess.Popen([str(binary), 'run', '--config', str(config), '--json'],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        events = Events(process.stdout)
        try:
            if role == 'acceptor':
                endpoint = events.until('listening')
                peer = socket.create_connection(('127.0.0.1', endpoint['port']), timeout=5)
            else:
                peer, _ = listener.accept()
                peer.settimeout(5)
            with peer:
                reader = Reader(peer)
                logon_fields = [(98, 0), (108, 30)]
                if version == 'FIXT.1.1': logon_fields.append((1137, 9))
                if role == 'acceptor': peer.sendall(frame(version, 1, 'A', logon_fields))
                logon = reader.read()
                assert logon[b'8'] == version.encode() and logon[b'35'] == b'A'
                assert logon[b'49'] == b'SELL' and logon[b'56'] == b'BUY'
                if version == 'FIXT.1.1': assert logon[b'1137'] == b'9'
                if role == 'initiator': peer.sendall(frame(version, 1, 'A', logon_fields))
                events.until('active')
                peer.sendall(frame(version, 2, 'D', ((11, 'VERSION-ORDER'), (55, 'AAPL'))))
                assert events.until('message')['business_messages'] == 1
                peer.sendall(frame(version, 3, '1', ((112, 'INDEPENDENT-TEST'),)))
                heartbeat = reader.read()
                assert heartbeat[b'35'] == b'0' and heartbeat[b'112'] == b'INDEPENDENT-TEST'
                assert int(heartbeat[b'369']) >= 2
                peer.sendall(frame(version, 4, '2', ((7, 1), (16, 2))))
                gapfill = reader.read()
                assert gapfill[b'35'] == b'4' and gapfill[b'43'] == b'Y'
                assert gapfill[b'123'] == b'Y' and gapfill[b'34'] == b'1' and gapfill[b'36'] == b'3'
                process.send_signal(signal.SIGTERM)
                logout = reader.read()
                assert logout[b'35'] == b'5'
                peer.sendall(frame(version, 5, '5'))
            stdout, stderr = process.communicate(timeout=10)
            assert process.returncode == 128 + signal.SIGTERM, (stdout, stderr)
            stopped = [json.loads(line) for line in stdout.splitlines() if json.loads(line).get('event') == 'stopped']
            assert stopped and stopped[-1]['code'] == 0 and stopped[-1]['business_messages'] == 1, (stdout, stderr)
        except Exception as error:
            if process.poll() is None: process.kill()
            stdout, stderr = process.communicate(timeout=5)
            raise RuntimeError(f'{version}/{role}/{dispatch}: {error}\n{stdout.decode()}\n{stderr.decode()}') from error
        finally:
            if process.poll() is None: process.kill(); process.wait()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='nimblefix-versions-') as temporary:
        for version in ('FIX.4.2', 'FIX.4.3', 'FIX.4.4', 'FIXT.1.1'):
            for role in ('initiator', 'acceptor'):
                for dispatch in ('inline', 'queued'):
                    exchange(args.binary.resolve(), Path(temporary), version, role, dispatch)
    print('16 transport/role/dispatch exchanges passed: Logon, delivery, 369, heartbeat, replay, Logout')


if __name__ == '__main__': main()
