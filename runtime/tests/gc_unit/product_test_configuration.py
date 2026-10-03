#!/usr/bin/env python3
"""Read test macros from the compile recipe bound to the selected product pair."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import sys
import struct
import subprocess

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'build'))
from resolve_runtime_headers import resolve


def product_inputs(runtime, library, output):
    if not (output / 'runtime-build-inputs.txt').is_file():
        output = publication_root(runtime, library)
    inputs = json.loads((output / 'runtime-build-inputs.txt').read_text())
    for name in ('libcangjie-runtime.so', 'libboundscheck.so'):
        digest = hashlib.sha256((library / name).read_bytes()).hexdigest()
        if inputs['products'][name] != digest:
            raise ValueError(f'compile recipe does not describe selected product: {name}')
    return inputs, output.resolve()


def configuration(runtime, library, output):
    inputs, _ = product_inputs(runtime, library, output)
    entries = [entry for entry in inputs['commands']
               if entry['file'].endswith('/Heap/z/zGeneration.cpp')]
    if len(entries) != 1:
        raise ValueError('expected one zGeneration.cpp product compile command')
    entry = entries[0]
    arguments = entry.get('arguments') or shlex.split(entry['command'])
    def enabled(name):
        return int(any(arg in ('-D' + name, '-D' + name + '=1') for arg in arguments))
    return (enabled('MRT_TESTABLE_INTERNALS'), enabled('MRT_GC_UNIT_TESTS'),
            enabled('MRT_GC_UNIT_OHOS_HOST'), enabled('NDEBUG'), enabled('MRT_DEBUG'))


def elf_identity(path):
    with path.open('rb') as stream:
        header = stream.read(20)
    if len(header) != 20 or header[:4] != b'\x7fELF' or header[5] not in (1, 2):
        raise ValueError(f'not a supported ELF: {path}')
    return header[4], header[5], struct.unpack('<H' if header[5] == 1 else '>H', header[18:20])[0]


def publication_root(runtime, library):
    root = library.resolve().parent.parent
    if (root / 'runtime-build-inputs.txt').is_file():
        return root
    if elf_identity(library / 'libcangjie-runtime.so')[2] == 183:
        raise ValueError('ARM product outside publication requires explicit GCV2_RUNTIME_OUTPUT_ROOT; '
                         'SO hashes alone do not identify its internal Copy object')
    return resolve(runtime, library)


def copy_object(runtime, library, output, compiler):
    inputs, output = product_inputs(runtime, library, output)
    identity = elf_identity(library / 'libcangjie-runtime.so')
    target = subprocess.run(shlex.split(compiler) + ['-dumpmachine'],
                            check=True, capture_output=True, text=True).stdout.strip().split('-')[0]
    machines = {'aarch64': (2, 1, 183), 'arm64': (2, 1, 183),
                'x86_64': (2, 1, 62), 'amd64': (2, 1, 62),
                'arm': (1, 1, 40), 'armv7': (1, 1, 40), 'armv7a': (1, 1, 40),
                'armv7l': (1, 1, 40), 'i386': (1, 1, 3), 'i686': (1, 1, 3)}
    if machines.get(target) != identity:
        raise ValueError(f'compiler target {target} differs from selected runtime ELF {identity}')
    if identity[2] != 183:
        return ''
    record = inputs.get('internal_test_objects', {}).get('copy_disjoint_words')
    if not record:
        raise ValueError('published product lacks internal Copy object')
    path = (output / record['path']).resolve()
    if not path.is_relative_to(output) or not path.is_file():
        raise ValueError('internal Copy object is missing or outside publication root')
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    hashes = json.loads((output / 'runtime-product-hashes.json').read_text())
    if digest != record['sha256'] or hashes.get(record['path']) != digest:
        raise ValueError('internal Copy object hash differs')
    if elf_identity(path) != identity:
        raise ValueError('internal Copy object ELF differs from selected runtime')
    return str(path)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runtime', type=Path)
    parser.add_argument('library', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--copy-object', action='store_true')
    parser.add_argument('--resolve-root', action='store_true')
    parser.add_argument('--with-debug', action='store_true')
    parser.add_argument('--compiler', default='clang++')
    args = parser.parse_args()
    try:
        if args.resolve_root:
            print(publication_root(args.runtime, args.library))
        elif args.copy_object:
            print(copy_object(args.runtime, args.library, args.output, args.compiler))
        else:
            values = configuration(args.runtime, args.library, args.output)
            print(*(values if args.with_debug else values[:4]))
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f'GC_UNIT_PRODUCT_CONFIGURATION_FAIL: {error}', file=sys.stderr)
        sys.exit(2)
