#!/usr/bin/env python3
"""Reproduce the 0.2 release matrix using already built xmake executables."""
import argparse
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--bin-dir',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--samples',type=int,default=5000)
    parser.add_argument('--runs',type=int,default=3)
    parser.add_argument('--cpu-set',default='0-3')
    parser.add_argument('--micro-cpu',default='0')
    parser.add_argument('--long-samples',type=int,default=64000)
    parser.add_argument('--skip-long',action='store_true')
    parser.add_argument('--skip-quickfix',action='store_true')
    args=parser.parse_args()
    binary=args.bin_dir.resolve();destination=args.output.resolve()
    if not 100<=args.samples<=10000 or not 3<=args.runs<=10:parser.error('samples 100..10000 and runs 3..10')
    destination.mkdir(parents=True,exist_ok=True)
    commands=[]
    def execute(script,*parameters,micro=False):
        command=['taskset','-c',args.micro_cpu if micro else args.cpu_set,
            sys.executable,str(ROOT/'bench'/script),*map(str,parameters)]
        print('RUN',script,*map(str,parameters),flush=True)
        result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
        log=destination/f'command-{len(commands)+1:02d}.log'
        log.write_text(result.stdout+result.stderr)
        commands.append({'command':command,'exit_code':result.returncode,'log':log.name})
        (destination/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        if result.returncode:raise RuntimeError(f'{script} failed, see {log}')
    execute('run.py','--binary',binary/'codec-bench','--output',destination/'codec',
        '--samples',args.samples,'--runs',args.runs,micro=True)
    execute('session_run.py','--binary',binary/'session-bench','--output',destination/'session',
        '--samples',args.samples,'--runs',args.runs,micro=True)
    execute('journal_run.py','--binary',binary/'journal-bench','--output',destination/'journal',
        '--samples',args.samples,'--runs',args.runs,'--reopens',5,
        '--require-filesystem','btrfs',micro=True)
    execute('schema_run.py','--binary',binary/'schema-bench','--output',destination/'schema',
        '--samples',args.samples,'--runs',args.runs,'--standard-schemas',micro=True)
    execute('runtime_run.py','--binary',binary/'runtime-bench','--output',destination/'runtime',
        '--samples',args.samples,'--runs',args.runs,'--warmup',100,'--rate',2000,
        '--cpu-set',args.cpu_set,'--require-filesystem','btrfs')
    for rate in (2000,20000,60000):
        for repetition in range(1,args.runs+1):
            for peer,executable in (('nimblefix','tcp-bench-server'),('null-echo','null-tcp-server'),('quickfix','quickfix-tcp-server')):
                if peer=='quickfix' and args.skip_quickfix:continue
                parameters=['--binary',binary/executable,'--output',destination/'comparison'/peer/f'{rate}-run-{repetition}',
                    '--samples',args.samples,'--rate',rate,'--peer-label',peer]
                if peer=='quickfix':
                    parameters.extend(['--peer-metadata','QuickFIX 1.16.0 SocketAcceptor; FileStore fflush, no explicit fsync/fdatasync; UseDataDictionary=N; NullLog'])
                if peer=='null-echo':
                    parameters.extend(['--echo','--peer-metadata','raw TCP byte echo; no codec, session or store'])
                execute('tcp_run.py',*parameters)
    for repetition in range(1,args.runs+1):
        execute('initiator_run.py','--binary',binary/'nimblefix','--output',destination/'initiator'/f'run-{repetition}',
            '--samples',args.samples,'--rate',2000)
        execute('isolation_run.py','--binary',binary/'isolation-server','--output',destination/'isolation'/f'run-{repetition}',
            '--samples',2000,'--rate',1000)
    if not args.skip_long:
        execute('runtime_run.py','--binary',binary/'runtime-bench','--output',destination/'rotation-long',
            '--mode','queued-ring','--send-source','callback','--samples',args.long_samples,'--warmup',100,
            '--rate',1000,'--cpu-set',args.cpu_set,'--require-filesystem','btrfs',
            '--journal-segment-bytes',8192,'--max-journal-segments',8,'--retain-outbound-messages',16)
    execute('check_regression.py','--report',destination/'runtime/report.json',
        '--write-budget',destination/'runtime/budget.json')
    execute('check_regression.py','--report',destination/'runtime/report.json',
        '--budget',destination/'runtime/budget.json')
    execute('archive_results.py',destination)
    print('Release matrix complete:',destination,flush=True)


if __name__=='__main__':main()
