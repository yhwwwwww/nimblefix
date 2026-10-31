#!/usr/bin/env python3
"""Generate and apply a host-specific runtime baseline budget after repeated runs."""
import argparse
import json
from pathlib import Path
import statistics
import sys

WORKLOAD_FIELDS = ('cpu_affinity','kernel','payload_bytes','request_rate_per_second',
    'producer_threads','in_flight_window','journal_filesystem','journal_segment_bytes',
    'max_journal_segments','retain_outbound_messages')

def cases(report):
    result={}
    for run in report['runs']:
        for name,value in run['cases'].items():result.setdefault(name,[]).append(value)
    return result


def make_budget(report):
    entries={}
    for name,runs in cases(report).items():
        entries[name]={
            'samples_at_least':min(value['samples'] for value in runs),
            'p50_ns_at_most':int(max(value['round_trip_ns']['p50'] for value in runs)*1.25),
            'p99_ns_at_most':int(max(value['round_trip_ns']['p99'] for value in runs)*1.5),
            'throughput_at_least':min(value['throughput_per_second'] for value in runs)*.8,
            'observed_p50_max_min_ratio':max(value['round_trip_ns']['p50'] for value in runs)/min(value['round_trip_ns']['p50'] for value in runs),
            'observed_p99_max_min_ratio':max(value['round_trip_ns']['p99'] for value in runs)/min(value['round_trip_ns']['p99'] for value in runs),
            'fsync_calls':0,'fdatasync_calls':0,'msync_calls':0}
    return {'scope':'dedicated repeat runs on the recorded development host and identical workload; not a portable latency SLO',
        'method':'p50 ceiling 1.25 * worst repeated p50; p99 ceiling 1.5 * worst repeated p99; throughput floor 0.8 * lowest repeated throughput; candidate compares medians',
        **{field:report[field] for field in WORKLOAD_FIELDS},
        'baseline_source_sha256':report['source_sha256'],'cases':entries}


def check(report,budget):
    failures=[]
    if len(report['runs'])<3:failures.append('at least three candidate runs are required')
    for field in WORKLOAD_FIELDS:
        if report.get(field)!=budget.get(field):failures.append(f'incompatible workload/environment: {field}')
    measured=cases(report)
    for name,limits in budget['cases'].items():
        runs=measured.get(name)
        if not runs:failures.append(f'{name}: missing case');continue
        if min(value['samples'] for value in runs)<limits['samples_at_least']:failures.append(f'{name}: too few samples')
        for quantile in ('p50','p99'):
            value=statistics.median(row['round_trip_ns'][quantile] for row in runs)
            if value>limits[f'{quantile}_ns_at_most']:failures.append(f'{name}: {quantile} {value}ns exceeds {limits[f"{quantile}_ns_at_most"]}ns')
        if statistics.median(row['throughput_per_second'] for row in runs)<limits['throughput_at_least']:failures.append(f'{name}: throughput below baseline floor')
        for row in runs:
            if sum(row['receipt_counts'].values())!=row['samples']:failures.append(f'{name}: incomplete receipts')
            for operation in ('fsync_calls','fdatasync_calls','msync_calls'):
                if row['diagnostics'][operation]!=0:failures.append(f'{name}: unexpected {operation}')
    return failures


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--report',type=Path,required=True)
    action=parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--write-budget',type=Path)
    action.add_argument('--budget',type=Path)
    args=parser.parse_args()
    report=json.loads(args.report.read_text())
    if args.write_budget:
        if len(report['runs'])<3:parser.error('at least three completed runs are required before setting a numeric budget')
        args.write_budget.write_text(json.dumps(make_budget(report),indent=2)+'\n')
        print(f'Wrote repeated-run budget: {args.write_budget}');return 0
    failures=check(report,json.loads(args.budget.read_text()))
    if failures:
        print('\n'.join(failures),file=sys.stderr);return 1
    print('Runtime receipts, synchronization, latency and throughput budgets passed');return 0


if __name__=='__main__':sys.exit(main())
