#!/usr/bin/env python3
"""Check OHOS dispatch through the real runner/gate with a published SO pair.

Run on kkk2. No compiler, runner, product, or receipt is substituted. The legacy
OHOS environment variable is deliberately absent or contradictory; only the
selected product recipe determines the arm.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--publication', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--elf', type=Path)
    parser.add_argument('--entry', choices=('standalone', 'gate'), default='standalone')
    parser.add_argument('--legacy-env', choices=('unset', 'ON', '0', '1'), default='unset')
    args = parser.parse_args()
    source, publication, output = (p.resolve() for p in
                                   (args.source, args.publication, args.output))
    output.mkdir(parents=True, exist_ok=True)
    for name in ('ohos_host.receipt', 'gate.status'):
        (output / name).unlink(missing_ok=True)
    env = dict(os.environ)
    for key in ('MRT_GC_UNIT_OHOS_HOST', 'MRT_TESTABLE_INTERNALS',
                'GCV2_RUNTIME_CONFIG', 'GC_UNIT_GATE_SKIP', 'GC_UNIT_GATE_CONTRACT_SELFTEST',
                'GC_UNIT_OHOS_HOST_TEST_ELF', 'GC_UNIT_OHOS_HOST_RECEIPT'):
        env.pop(key, None)
    env.update(GCV2_RUNTIME_LIB_DIR=str(publication / 'lib'),
               GCV2_RUNTIME_OUTPUT_ROOT=str(publication), GC_UNIT_OUT=str(output),
               GC_UNIT_GATE_STATUS=str(output / 'gate.status'), GC_UNIT_GATE_LANGUAGE_TESTS='defer')
    if args.legacy_env != 'unset':
        env['MRT_GC_UNIT_OHOS_HOST'] = args.legacy_env
    if args.elf:
        env['GC_UNIT_OHOS_HOST_TEST_ELF'] = str(args.elf.resolve())
    identity = {name: hashlib.sha256((publication / 'lib' / name).read_bytes()).hexdigest()
                for name in ('libcangjie-runtime.so', 'libboundscheck.so')}
    script = 'run_standalone.sh' if args.entry == 'standalone' else 'gate_gc_unit.sh'
    with (output / 'run.log').open('w') as log:
        rc = subprocess.call(['bash', str(source / 'runtime/tests/gc_unit' / script)],
                             env=env, stdout=log, stderr=subprocess.STDOUT)
    receipt = output / 'ohos_host.receipt'
    fields = dict(line.split('=', 1) for line in receipt.read_text().splitlines() if '=' in line) if receipt.exists() else {}
    status = output / 'gate.status'
    gate = dict(line.split('=', 1) for line in status.read_text().splitlines() if '=' in line) if status.exists() else {}
    # Observe the selected arm before checking completion: a dispatch cut must
    # fail this invariant, not an earlier generic process-success assertion.
    dispatched = fields.get('CONFIGURATION') == 'MRT_GC_UNIT_OHOS_HOST'
    if args.entry == 'gate':
        dispatched = dispatched and gate.get('OHOS_HOST') == 'PASS'
    result = dict(entry=args.entry, legacy_env=args.legacy_env, runner_rc=rc,
                  product_sha256=identity, dispatched=dispatched, receipt=fields, gate=gate)
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'OHOS_DISPATCH_ASSERT entry={args.entry} legacy_env={args.legacy_env} '
          f'dispatched={int(dispatched)} runner_rc={rc}', flush=True)
    assert dispatched, 'OHOS product must dispatch the OHOS arm from its recipe'
    assert rc == 0 and fields.get('RESULT') == 'PASS', 'dispatched OHOS filters must complete'
    assert all(fields.get('FILTER_' + key) == 'PASS'
               for key in ('HANDLER', 'RELOCATE', 'CALLBACK', 'MAJOR', 'POST', 'EMPTY'))
    print('OHOS_CONFIGURATION_PASS', flush=True)


if __name__ == '__main__':
    main()
