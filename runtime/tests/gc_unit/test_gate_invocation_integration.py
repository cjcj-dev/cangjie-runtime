#!/usr/bin/env python3
"""Verify native file ownership via an actual gate or native build invocation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def run_inventory(unit):
    """Path->digest of every file each finished gate invocation owns."""
    runs = {}
    for run in sorted((unit / 'gate-runs').glob('run.*')) if (unit / 'gate-runs').is_dir() else []:
        runs[run] = {str(path.relative_to(run)): hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in sorted(run.rglob('*')) if path.is_file()}
    return runs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--lib', type=Path)
    parser.add_argument('--gate', type=Path)
    parser.add_argument('--sdk', type=Path)
    parser.add_argument('--mode', choices=('defer', 'only', 'all'), default='defer')
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--calls', type=int, default=2)
    parser.add_argument('--expected-rc', type=int, default=0)
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    root = args.work.resolve()
    env = dict(os.environ, GC_UNIT_OUT=str(root / 'unit'),
               GC_UNIT_GATE_STATUS=str(root / 'latest.status'),
               GC_UNIT_GATE_LANGUAGE_TESTS=args.mode, GC_UNIT_GATE_SKIP='0',
               GC_UNIT_GATE_CONTRACT_SELFTEST='1')
    env.pop('GC_UNIT_GATE_RESULT', None)
    if args.lib:
        env['GCV2_RUNTIME_LIB_DIR'] = str(args.lib.resolve())
    if args.sdk:
        env.update(CANGJIE_HOME=str(args.sdk), CJC=str(args.sdk / 'bin/cjc'),
                   GC_UNIT_CJC_RUNTIME_LIB_DIR=str(args.sdk / 'host/compiler'))
    command = (['python3', 'build.py', 'build', '--target', 'native', '--build-type', 'release',
                '-v', '1.3.0-alpha.06'] if args.native else
               ['bash', str(args.gate.resolve() if args.gate else
                            args.runtime.resolve() / 'tests/gc_unit/gate_gc_unit.sh')])
    records = []
    errors = []
    # Round-boundary history. A terminal self-consistency check cannot tell a
    # preserved first round from a first round rewritten by the second one, so
    # the path->digest baseline of every run is taken the moment its own call
    # returns and is compared after each later call.
    history = {}
    for call in range(args.calls):
        start = time.monotonic()
        with (root / f'call-{call}.stdout.log').open('w') as stdout, (
                root / f'call-{call}.stderr.log').open('w') as stderr:
            result = subprocess.run(command, cwd=args.runtime, env=env, stdout=stdout, stderr=stderr)
        records.append({'call': call, 'rc': result.returncode, 'wall': time.monotonic() - start})
        if result.returncode != args.expected_rc:
            errors.append(f'ENTRY_RC call={call} actual={result.returncode} expected={args.expected_rc}')
        runs = run_inventory(root / 'unit')
        for run, before in history.items():
            changed = sorted(name for name, digest in before.items() if runs.get(run, {}).get(name) != digest)
            print(f'ROUND_HISTORY_ASSERT after_call={call} run={run.name} changed={changed}', flush=True)
            if changed:
                errors.append(f'ROUND_HISTORY_ASSERT after_call={call} run={run.name} changed={changed}')
        for run, snapshot in runs.items():
            history.setdefault(run, snapshot)
    receipts = sorted((root / 'unit/gate-runs').glob('*/invocation.json'))
    if not receipts:
        errors.append('NO_INVOCATIONS')
    identities = []
    for path in receipts:
        receipt = json.loads(path.read_text())
        identities.append(receipt['run_id'])
        outputs = receipt['outputs']
        required = ['gate.status', 'gate.stdout.log', 'gate.stderr.log']
        if args.mode != 'only':
            required.extend(['test-manifest.tsv', 'gate_tally.txt', 'gate_run.log'])
        if args.mode != 'defer':
            required.extend(['finalizer_trigger.build.log', 'finalizer_trigger.run.log',
                             'phase_entry_trigger.build.log'])
        cached = receipt['status']['CPP_SUITE_SOURCE'] == 'CACHE'
        if cached:
            required = [name for name in required if name not in (
                'test-manifest.tsv', 'gate_tally.txt', 'gate_run.log')]
        if receipt['status']['FINALIZER_TRIGGER_SOURCE'] == 'CACHE':
            required = [name for name in required if name not in (
                'finalizer_trigger.build.log', 'finalizer_trigger.run.log',
                'phase_entry_trigger.build.log')]
        if cached or receipt['status']['FINALIZER_TRIGGER_SOURCE'] == 'CACHE':
            source = receipt['cache_source']
            if not source or not (Path(source['evidence_dir']) / 'invocation.json').is_file():
                errors.append(f'CACHE_SOURCE_ASSERT {receipt["run_id"]} missing source')
        missing = sorted(set(required) - set(outputs))
        print(f'OWNERSHIP_ASSERT run={receipt["run_id"]} missing={missing}', flush=True)
        if missing:
            errors.append(f'OWNERSHIP_ASSERT {receipt["run_id"]} missing={missing}')
        changed = []
        for name, digest in outputs.items():
            file = path.parent / name
            if not file.is_file() or hashlib.sha256(file.read_bytes()).hexdigest() != digest:
                changed.append(name)
        print(f'IMMUTABILITY_ASSERT run={receipt["run_id"]} changed={changed}', flush=True)
        if changed:
            errors.append(f'IMMUTABILITY_ASSERT {receipt["run_id"]} changed={changed}')
    if len(identities) != len(set(identities)):
        errors.append('RUN_ID_COLLISION')
    (root / 'integration-result.json').write_text(json.dumps({
        'command': command, 'calls': records, 'run_ids': identities, 'errors': errors,
    }, indent=2) + '\n')
    for error in errors:
        print(error, flush=True)
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
