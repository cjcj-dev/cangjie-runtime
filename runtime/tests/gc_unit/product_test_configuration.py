#!/usr/bin/env python3
"""Read test macros from the compile recipe bound to the selected product pair."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'build'))
from resolve_runtime_headers import resolve


def configuration(runtime, library, output):
    if not (output / 'runtime-build-inputs.txt').is_file():
        output = resolve(runtime, library)
    inputs = json.loads((output / 'runtime-build-inputs.txt').read_text())
    for name in ('libcangjie-runtime.so', 'libboundscheck.so'):
        digest = hashlib.sha256((library / name).read_bytes()).hexdigest()
        if inputs['products'][name] != digest:
            raise ValueError(f'compile recipe does not describe selected product: {name}')
    entries = [entry for entry in inputs['commands']
               if entry['file'].endswith('/Heap/z/zGeneration.cpp')]
    if len(entries) != 1:
        raise ValueError('expected one zGeneration.cpp product compile command')
    entry = entries[0]
    arguments = entry.get('arguments') or shlex.split(entry['command'])
    def enabled(name):
        return int(any(arg in ('-D' + name, '-D' + name + '=1') for arg in arguments))
    return enabled('MRT_TESTABLE_INTERNALS'), enabled('MRT_GC_UNIT_TESTS')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runtime', type=Path)
    parser.add_argument('library', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        print(*configuration(args.runtime, args.library, args.output))
    except (OSError, ValueError, KeyError) as error:
        print(f'GC_UNIT_PRODUCT_CONFIGURATION_FAIL: {error}', file=sys.stderr)
        sys.exit(2)
