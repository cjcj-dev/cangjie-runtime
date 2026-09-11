#!/usr/bin/env python3
"""Exercise standalone's actual AST compiler with a relocated product pair.

The compiler wrapper records arguments and execs the real compiler unchanged.
No synthetic compiler or replacement analyzer participates in this test.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--publication', required=True, type=Path)
    parser.add_argument('--work', required=True, type=Path)
    parser.add_argument('--entry', choices=('standalone', 'gate'), default='standalone')
    args = parser.parse_args()
    runtime, publication, work = args.runtime.resolve(), args.publication.resolve(), args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    fields = dict(line.split('=', 1) for line in
                  (publication / 'runtime-build-config.txt').read_text().splitlines() if '=' in line)
    library = work / 'relocated/deferred-sodepot'
    library.mkdir(parents=True, exist_ok=True)
    identities = {}
    for name in ('libcangjie-runtime.so', 'libboundscheck.so'):
        original = Path(fields['LIB_DIR']) / name
        shutil.copy2(original, library / name)
        assert sha(original) == sha(library / name)
        identities[name] = sha(library / name)
    # A compatible but stale header directory makes the historical consumer
    # complete compilation, so the target identity assertion is reached.
    stale = library.parent.parent / 'include'
    shutil.copytree(publication / 'include', stale, dirs_exist_ok=True)
    with (stale / 'log.h').open('a') as output:
        output.write('\n// copied-pair regression: stale header control\n')
    compiler = shutil.which(os.environ.get('CXX', 'clang++'))
    assert compiler, 'real compiler required'
    wrapper = work / 'compiler'
    trace = work / 'compiler.jsonl'
    trace.write_text('')
    wrapper.write_text('#!/usr/bin/env python3\nimport json, os, sys\n'
                       f'with open({str(trace)!r}, "a") as f: f.write(json.dumps(sys.argv[1:])+"\\n")\n'
                       f'os.execv({compiler!r}, [{compiler!r}] + sys.argv[1:])\n')
    wrapper.chmod(0o755)
    env = os.environ.copy()
    for name in ('GCV2_RUNTIME_CONFIG', 'GCV2_RUNTIME_OUTPUT_ROOT', 'CJRT_HEAP_FILLER',
                 'MRT_GC_UNIT_OHOS_HOST', 'MRT_TESTABLE_INTERNALS'):
        env.pop(name, None)
    env.update(GCV2_RUNTIME_LIB_DIR=str(library), CXX=str(wrapper),
               GC_UNIT_OUT=str(work / 'out'))
    if args.entry == 'standalone':
        env['GC_UNIT_MUTUALWAIT_MANIFEST_ONLY'] = '1'
        script = 'run_standalone.sh'
    else:
        env.pop('GC_UNIT_MUTUALWAIT_MANIFEST_ONLY', None)
        env.update(GC_UNIT_GATE_LANGUAGE_TESTS='defer', GC_UNIT_GATE_STATUS=str(work / 'gate.status'))
        script = 'gate_gc_unit.sh'
    with (work / 'standalone.log').open('w') as output:
        result = subprocess.run(['bash', str(runtime / 'tests/gc_unit' / script)],
                                env=env, stdout=output, stderr=subprocess.STDOUT)
    record = dict(pair=identities, entry=args.entry, standalone_rc=result.returncode)
    (work / 'result.json').write_text(json.dumps(record, indent=2))
    assert result.returncode == 0, 'AST prerequisite failed; this is not an identity assertion failure'
    print('COPIED_HEADERS_AST_CONTROL_PASS', flush=True)
    invocations = [json.loads(line) for line in trace.read_text().splitlines()]
    compiles = [argv for argv in invocations if '-ast-dump=json' in argv]
    assert compiles, 'real AST compiler invocation must be observed'
    expected = json.loads((publication / 'runtime-product-hashes.json').read_text())
    expected_headers = {name.removeprefix('include/'): digest for name, digest in expected.items()
                        if name.startswith('include/')}
    selected = []
    for argv in compiles:
        roots = [Path(arg[2:]) for arg in argv if arg.startswith('-I')]
        # These published headers have no earlier source include candidate in
        # the runner. Resolve in compiler search order, not directory order.
        observed = {}
        for name in expected_headers:
            source = next((root / name for root in roots if (root / name).is_file()), None)
            observed[name] = sha(source) if source else None
        selected.append(observed)
    record['compiler_header_hashes'] = selected
    (work / 'result.json').write_text(json.dumps(record, indent=2))
    print('COPIED_HEADERS_IDENTITY_ASSERT', flush=True)
    assert all(headers == expected_headers for headers in selected), 'compiler consumed different published header bytes'
    print('COPIED_HEADERS_IDENTITY_PASS', flush=True)


if __name__ == '__main__':
    main()
