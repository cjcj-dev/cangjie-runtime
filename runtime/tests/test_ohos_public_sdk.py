#!/usr/bin/env python3
"""Observe the actual runtime/cjthread build, without substituting either entry."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--arch', choices=('x86_64', 'aarch64'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--configured-only', action='store_true')
    args = parser.parse_args()
    sdk = args.sdk.absolute()
    checks = []
    artifacts = []

    def check(name, result, observed):
        checks.append({'name': name, 'pass': bool(result), 'observed': observed})
        print(('PASS ' if result else 'FAIL ') + name, flush=True)

    for label, build in [('runtime', args.build), ('cjthread', args.build / 'cjthread-build')]:
        database = build / 'compile_commands.json'
        commands = json.loads(database.read_text()) if database.is_file() else []
        check(label + '.compile_database', bool(commands), str(database))
        compilers, roots, triples = set(), set(), set()
        missing = []
        for entry in commands:
            words = entry.get('arguments') or shlex.split(entry['command'])
            compiler = next((w for w in words if Path(w).name in ('clang', 'clang++')), '')
            entry_roots = [w.split('=', 1)[1] for w in words if w.startswith('--sysroot=')]
            entry_triples = [w.split('=', 1)[1] for w in words if w.startswith('--target=')]
            compilers.add(compiler)
            roots.update(entry_roots)
            triples.update(entry_triples)
            if not compiler or not entry_roots or not entry_triples:
                missing.append(entry['file'])
        # All commands contribute; a missing parameter cannot disappear in a set union.
        check(label + '.sdk_route', bool(commands) and not missing
              and all(Path(c).parent == sdk / 'llvm/bin' for c in compilers)
              and roots == {str(sdk / 'sysroot')},
              {'compilers': sorted(compilers), 'sysroots': sorted(roots), 'missing': missing})
        check(label + '.target', bool(commands) and not missing
              and triples == {args.arch + '-linux-ohos'}, sorted(triples))

    renamed = list(args.build.rglob('libstdc++.so'))
    check('no_runtime_library_rename', not renamed, [str(p) for p in renamed])
    if not args.configured_only:
        for pattern in ('libcangjie-runtime.so', 'libcangjie-thread.a'):
            products = list(args.build.rglob(pattern))
            check('product.' + pattern, bool(products), [str(p) for p in products])
            for product in products:
                result = subprocess.run(['file', str(product)], capture_output=True, text=True)
                check('file.' + pattern, result.returncode == 0, result.stdout.strip())
                identity = {'sha256': hashlib.sha256(product.read_bytes()).hexdigest(),
                            'path': str(product), 'file': result.stdout.strip()}
                artifacts.append(identity)
                print(json.dumps(identity), flush=True)
                if product.suffix == '.so':
                    with product.open('rb') as stream:
                        header = stream.read(20)
                    machine = int.from_bytes(header[18:20], 'little')
                    check('elf.machine', header[:4] == b'\x7fELF'
                          and machine == {'x86_64': 62, 'aarch64': 183}[args.arch], machine)
    args.output.write_text(json.dumps({'checks': checks, 'artifacts': artifacts}, indent=2) + '\n')
    return 0 if all(c['pass'] for c in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
