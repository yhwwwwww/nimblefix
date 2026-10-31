#!/usr/bin/env python3
"""Archive a report tree with deterministic compressed raw samples and portable links."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path


def archive(directory):
    directory=directory.resolve()
    replacements={}
    raw_paths = set(directory.rglob('*.csv')) | set(directory.rglob('*-samples.json'))
    for path in sorted(raw_paths):
        raw=path.read_bytes()
        target=path.with_suffix(path.suffix+'.gz')
        with target.open('wb') as file:
            with gzip.GzipFile(filename='',mode='wb',fileobj=file,mtime=0) as compressed:
                compressed.write(raw)
        if gzip.decompress(target.read_bytes()) != raw: raise RuntimeError('archive verification failed')
        relative=target.relative_to(directory).as_posix()
        replacements[str(path.resolve())]=relative
        replacements[path.relative_to(directory).as_posix()]=relative
        path.unlink()
    def normalize(value, parent):
        if isinstance(value,dict):return {key:normalize(item, parent) for key,item in value.items()}
        if isinstance(value,list):return [normalize(item, parent) for item in value]
        if isinstance(value,str):
            if value in replacements:return replacements[value]
            if value.startswith(str(directory)+'/'):return Path(value).relative_to(directory).as_posix()
            if value.endswith(('.csv', '-samples.json')):
                candidate=(parent / value).resolve()
                if str(candidate) in replacements:return replacements[str(candidate)]
                compressed=candidate.with_suffix(candidate.suffix+'.gz')
                if compressed.is_relative_to(directory) and compressed.is_file():
                    return compressed.relative_to(directory).as_posix()
        return value
    for path in sorted(directory.rglob('*.json')):
        path.write_text(json.dumps(normalize(json.loads(path.read_text()), path.parent),indent=2)+'\n')
    checksums={}
    archives = set(directory.rglob('*.csv.gz')) | set(directory.rglob('*-samples.json.gz'))
    for path in sorted(archives):
        raw=gzip.decompress(path.read_bytes())
        checksums[path.relative_to(directory).as_posix()]={
            'uncompressed_sha256':hashlib.sha256(raw).hexdigest(),'uncompressed_bytes':len(raw)}
    if checksums:
        (directory/'raw-manifest.json').write_text(json.dumps(checksums,indent=2)+'\n')
    print(f'Archived {len(checksums)} raw sample files in {directory}')


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('directory',type=Path)
    archive(parser.parse_args().directory)
