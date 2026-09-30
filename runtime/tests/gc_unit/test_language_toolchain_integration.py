#!/usr/bin/env python3
"""Real SDK admission checks; no substitute compiler or runtime implementation."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--official-sdk', type=Path, required=True)
    parser.add_argument('--rejection-only', action='store_true')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    unit = Path(__file__).parent.resolve()
    env = dict(os.environ, GC_UNIT_GATE_CONTRACT_SELFTEST='1',
               GC_UNIT_GATE_LANGUAGE_TESTS='only', GC_UNIT_GATE_SKIP='0')
    env.pop('CJC', None)
    qualification = Path(env['GC_UNIT_LANGUAGE_QUALIFICATION'])
    changed = args.out / 'changed-qualification.json'
    proof = json.loads(qualification.read_text())
    proof['source'] += '; unapproved change'
    changed.write_text(json.dumps(proof, sort_keys=True))
    cases = [('unknown-qualification', {'GC_UNIT_LANGUAGE_QUALIFICATION': str(changed)})]
    if not args.rejection_only:
        cases += [('official', {'GC_UNIT_LANGUAGE_SDK': str(args.official_sdk)}),
                  ('missing-language', {'GC_UNIT_LANGUAGE_SDK': ''}),
                  ('missing-build', {'GC_UNIT_BUILD_SDK': ''}),
                  ('missing-host', {'GC_UNIT_CJC_RUNTIME_LIB_DIR': ''}),
                  ('missing-qualification', {'GC_UNIT_LANGUAGE_QUALIFICATION': ''}),
                  ('cjc-conflict', {'CJC': str(args.official_sdk / 'bin/cjc')})]
    commands = ['gate_gc_unit.sh', 'run_finalizer_trigger.sh',
                'run_phase_entry_trigger.sh', 'run_segmented_array_managed.sh',
                'kkk2_managed.sh']

    def invoke(case, changes, command):
        out = args.out / case / command.removesuffix('.sh')
        out.mkdir(parents=True, exist_ok=True)
        local = dict(env, **changes, GC_UNIT_OUT=str(out),
                     GC_UNIT_GATE_STATUS=str(out / 'latest.status'))
        arguments = ['bash', str(unit / command)]
        if command == 'kkk2_managed.sh':
            local.update(LANE=str(out), OUT=str(out), SRCROOT=str(unit.parents[2]),
                         STAINED_RT=env['GCV2_RUNTIME_LIB_DIR'], N='1')
            arguments.append('admission-contract')
        start = time.monotonic()
        result = subprocess.run(arguments, env=local,
                                capture_output=True, text=True)
        (out / 'stdout.log').write_text(result.stdout)
        (out / 'stderr.log').write_text(result.stderr)
        binaries = [str(path) for path in out.rglob('*') if path.is_file() and
                    (path.name.endswith('.build.log') or path.name in
                     ('finalizer_trigger', 'phase_entry_minor', 'phase_entry_major',
                      'segmented_array_managed'))]
        status_ok = True
        if command == 'gate_gc_unit.sh':
            status = (out / 'latest.status').read_text()
            status_ok = 'LANGUAGE_TESTS=NOT_RUN\n' in status and 'GATE=FAIL\n' in status
        passed = (result.returncode == 2 and 'LANGUAGE_ADMISSION_NOT_RUN' in
                  result.stderr and not binaries and status_ok)
        record = dict(case=case, command=command, rc=result.returncode,
                      wall=time.monotonic() - start, compile_artifacts=binaries,
                      assertion='ADMISSION_BEFORE_COMPILATION', passed=passed)
        (out / 'result.json').write_text(json.dumps(record, sort_keys=True))
        print(f'ADMISSION_BEFORE_COMPILATION {"PASS" if passed else "FAIL"} '
              f'case={case} command={command} rc={result.returncode} artifacts={binaries}', flush=True)
        return record

    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as executor:
        futures = [executor.submit(invoke, case, changes, command)
                   for case, changes in cases for command in commands
                   if not (case == 'missing-build' and command == 'kkk2_managed.sh')]
        records = [future.result() for future in futures]
    (args.out / 'results.json').write_text(json.dumps(records, indent=2, sort_keys=True))
    return int(any(not record['passed'] for record in records))


if __name__ == '__main__':
    raise SystemExit(main())
