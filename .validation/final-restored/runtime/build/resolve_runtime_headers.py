#!/usr/bin/env python3
"""Locate published headers for an explicitly selected, possibly copied SO pair."""
import hashlib
import json
from pathlib import Path
import sys


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def resolve(runtime, library):
    pair = {name: sha(library / name)
            for name in ('libcangjie-runtime.so', 'libboundscheck.so')}
    matches = []
    for manifest in sorted((runtime / 'output/temp').glob('*/runtime-build-config.txt')):
        fields = dict(line.split('=', 1) for line in manifest.read_text().splitlines() if '=' in line)
        if (fields.get('RUNTIME_SHA256') != pair['libcangjie-runtime.so'] or
                fields.get('BOUNDSCHECK_SHA256') != pair['libboundscheck.so']):
            continue
        root = manifest.parent.resolve()
        hashes = json.loads((root / 'runtime-product-hashes.json').read_text())
        headers = {name: digest for name, digest in hashes.items() if name.startswith('include/')}
        if not headers:
            raise ValueError(f'empty published header inventory: {root}')
        actual_names = {str(path.relative_to(root)) for path in (root / 'include').rglob('*') if path.is_file()}
        if actual_names != set(headers):
            raise ValueError(f'published header inventory differs: {root}')
        for name, digest in headers.items():
            path = (root / name).resolve()
            if not path.is_relative_to(root / 'include') or sha(path) != digest:
                raise ValueError(f'published header hash differs: {root / name}')
        matches.append((root, headers))
    if not matches:
        raise ValueError('no published headers match the selected runtime and boundscheck hashes; '
                         'set GCV2_RUNTIME_OUTPUT_ROOT for an external build')
    if any(headers != matches[0][1] for _, headers in matches):
        raise ValueError('selected SO pair has ambiguous published headers; set GCV2_RUNTIME_OUTPUT_ROOT')
    # Multiple publications with identical header bytes are interchangeable for
    # compilation. Never infer an identity from directory age or iteration order.
    return matches[0][0]


if __name__ == '__main__':
    try:
        print(resolve(Path(sys.argv[1]), Path(sys.argv[2])))
    except (OSError, ValueError, KeyError) as error:
        print(f'RUNTIME_HEADERS_RESOLVE_FAIL: {error}', file=sys.stderr)
        sys.exit(2)
