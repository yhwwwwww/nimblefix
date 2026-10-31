#!/usr/bin/env python3
"""Independent TestRequest/Heartbeat load against a real Inline FIX initiator.

This is an administrative session boundary, separately reported from D/8
business workloads. The initiator uses the normal configured mmap journal.
"""
import argparse
import csv
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import selectors
import signal
import socket
import subprocess
import tempfile
import time
from run import output, source_digest
from tcp_run import frame, extract

ROOT=Path(__file__).resolve().parents[1]


def run(binary, destination, state_parent, samples, rate):
    destination.mkdir(parents=True,exist_ok=True)
    state_parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='nimblefix-initiator-bench-',dir=state_parent.resolve()) as state:
        with socket.socket() as listener:
            listener.bind(('127.0.0.1',0));listener.listen(1);listener.settimeout(10)
            config=Path(state)/'initiator.toml'
            config.write_text(f'''version = 1
[engine]
dispatch = "inline"
schema = "none"
logout_timeout_ms = 1000
[engine.storage]
directory = "./journal"
confirmation = "appended"
[[sessions]]
name = "benchmark"
role = "initiator"
sender_comp_id = "SELL"
target_comp_id = "BUY"
endpoint = "127.0.0.1:{listener.getsockname()[1]}"
''')
            child=subprocess.Popen([str(binary),'run','--config',str(config),'--json'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            try:
                peer,_=listener.accept()
                with peer:
                    peer.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1);peer.settimeout(5)
                    buffered=b''
                    while True:
                        messages,buffered=extract(buffered+peer.recv(65536))
                        if messages:
                            assert messages[0][b'35']==b'A' and messages[0][b'34']==b'1'
                            break
                    peer.sendall(frame(1,'A',((98,0),(108,30))))
                    peer.setblocking(False)
                    selector=selectors.DefaultSelector();selector.register(peer,selectors.EVENT_READ)
                    pending=bytearray();sent=received=0;next_peer=2;expected_server=2
                    start=time.monotonic_ns()+10_000_000;interval=1_000_000_000//rate
                    rows=[];seen=set();deadline=time.monotonic()+max(10,samples/rate+5)
                    while received<samples and time.monotonic()<deadline:
                        now=time.monotonic_ns()
                        while sent<samples and now>=start+sent*interval and len(pending)<1_048_576:
                            pending+=frame(next_peer,'1',((112,str(sent)),(369,expected_server-1)))
                            sent+=1;next_peer+=1
                        selector.modify(peer,selectors.EVENT_READ|(selectors.EVENT_WRITE if pending else 0))
                        timeout=min(.005,max(0,(start+sent*interval-now)/1e9)) if sent<samples else .005
                        for _,events in selector.select(timeout):
                            if events&selectors.EVENT_WRITE and pending:
                                try: amount=peer.send(pending);del pending[:amount]
                                except BlockingIOError:pass
                            if events&selectors.EVENT_READ:
                                try: chunk=peer.recv(65536)
                                except BlockingIOError:continue
                                if not chunk:raise RuntimeError('initiator closed during administrative load')
                                messages,buffered=extract(buffered+chunk);observed=time.monotonic_ns()
                                for message in messages:
                                    assert int(message[b'34'])==expected_server
                                    expected_server+=1
                                    if message[b'35']==b'1':
                                        pending+=frame(next_peer,'0',((112,message[b'112'].decode()),));next_peer+=1
                                    elif message[b'35']==b'0' and b'112' in message:
                                        identifier=int(message[b'112'])
                                        assert identifier<samples and identifier not in seen
                                        seen.add(identifier);received+=1
                                        rows.append((identifier,observed-(start+identifier*interval)))
                                    elif message[b'35']!=b'0':raise RuntimeError(f'unexpected message {message[b"35"]!r}')
                    selector.close()
                    assert len(seen)==samples and sent==samples
                    elapsed=time.monotonic_ns()-start
                    peer.setblocking(True);peer.settimeout(3)
                    if pending:peer.sendall(pending)
                    child.send_signal(signal.SIGTERM)
                    while True:
                        messages,buffered=extract(buffered+peer.recv(65536))
                        if any(message[b'35']==b'5' for message in messages):break
                    peer.sendall(frame(next_peer,'5',((369,expected_server),)))
                stdout,stderr=child.communicate(timeout=10)
                assert child.returncode==128+signal.SIGTERM,(child.returncode,stdout,stderr)
                events=[json.loads(line) for line in stdout.splitlines()]
                stopped=next(event for event in events if event.get('event')=='stopped')
                assert stopped['code']==0 and stopped['error_events']==0,(stopped,stderr)
                with (destination/'samples.csv').open('w') as file:
                    writer=csv.writer(file);writer.writerow(('id','scheduled_to_heartbeat_ns'));writer.writerows(rows)
                values=sorted(row[1] for row in rows)
                report={
                    'boundary':'real Inline Engine initiator, appended v2 mmap journal; independent peer scheduled TestRequest to parsed Heartbeat with matching TestReqID; no business callback',
                    'confirmation':'appended; process-death recovery, no power-loss promise',
                    'samples':samples,'offered_rate':rate,'observed_rate':samples*1e9/elapsed,'warmup':0,
                    'p50_ns':values[math.ceil(samples*.5)-1],'p99_ns':values[math.ceil(samples*.99)-1],
                    'p999_ns':values[math.ceil(samples*.999)-1],'max_ns':values[-1],
                    'revision':output('git','rev-parse','HEAD'),'source_sha256':source_digest(),
                    'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
                    'filesystem':output('stat','-f','-c','%T',str(state_parent)),
                    'cpu_affinity':sorted(os.sched_getaffinity(0)),'tcp_nodelay':True,
                    'timestamp_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    'server_summary':stopped,'limitations':'development host; Python scheduling/framing included; no equivalence claim to business D/8 callback workload'}
                (destination/'report.json').write_text(json.dumps(report,indent=2)+'\n')
                print(json.dumps({key:report[key] for key in ('samples','p50_ns','p99_ns','p999_ns','max_ns')}))
            finally:
                if child.poll() is None:child.kill();child.communicate()


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--samples',type=int,default=2000)
    parser.add_argument('--rate',type=int,default=2000)
    parser.add_argument('--state-parent',type=Path,default=ROOT/'build/bench-state')
    args=parser.parse_args()
    if not 100<=args.samples<=20000 or not 1<=args.rate<=100000:parser.error('samples 100..20000, rate 1..100000')
    run(args.binary.resolve(),args.output,args.state_parent,args.samples,args.rate)
