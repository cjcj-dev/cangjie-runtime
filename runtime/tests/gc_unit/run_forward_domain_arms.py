#!/usr/bin/env python3
"""Run #181's explicit return-domain contract against retained product libraries.

The frozen main includes #62 and accepts every non-null validated receipt.
The baseline/restored arms leave that contract untouched; cuts reject valid
answers or bypass the corresponding inner fail-closed branch.
Each filter is exact (the unit runner does not implement wildcard filters).
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

from run_remap_window_arms import sha, replace_one, patch, HEADER, PRODUCER, ENTRY

TABLE = 'runtime/src/Heap/Allocator/ForwardingTable.cpp'
WAIT = 'runtime/src/Heap/Collector/Relocate.cpp'
TESTS = ('Identity', 'NonIdentityCopy', 'RetiredHit', 'MissingEntry', 'WrongLifecycle', 'Unavailable')
ARMS = ('baseline', 'cut_identity', 'cut_copy_return', 'cut_retired',
        'cut_missing', 'cut_life', 'cut_unavailable', 'cut_wait', 'cut_entry', 'restored')


def mutate(originals, arm):
    texts = dict(originals)
    if arm not in ARMS:
        raise ValueError(f'unknown arm: {arm}')
    # Validate the current contract even for unmodified arms. An obsolete
    # input must fail before any build rather than silently becoming a control.
    replace_one(texts[HEADER], 'if (resolved != nullptr)', 'if (resolved != nullptr)')
    if arm == 'cut_identity':
        texts[HEADER] = replace_one(texts[HEADER], 'if (resolved != nullptr)',
                                   'if (resolved != nullptr && resolved != obj)')
    elif arm == 'cut_copy_return':
        texts[HEADER] = replace_one(texts[HEADER], 'if (resolved != nullptr)',
                                   'if (resolved != nullptr && resolved == obj)')
    elif arm == 'cut_retired':
        texts[TABLE] = replace_one(texts[TABLE], '    if (retired != 0) {\n        g_armedHit',
                                   '    if (retired != 0 && false) {\n        g_armedHit')
    elif arm in ('cut_missing', 'cut_life', 'cut_unavailable'):
        condition = {
            'cut_missing': 'lastLookup.answer == ForwardingTable::ToAnswer::ArmedMiss',
            'cut_life': '(static_cast<unsigned>(lastLookup.unavailableCause) & 1u) != 0',
            'cut_unavailable': '(static_cast<unsigned>(lastLookup.unavailableCause) & 16u) != 0',
        }[arm]
        before = '        CHECK_DETAIL(false,\n                     "WCollector::WaitRoutedTipReady.%s consumer='
        after = '        if (' + condition + ') { return from; }\n' + before
        texts[WAIT] = replace_one(texts[WAIT], before, after)
    elif arm == 'cut_wait':
        texts[HEADER] = replace_one(texts[HEADER],
            'BaseObject* resolved = WaitRoutedTipReady(obj, to, forwarding, provenance);',
            'BaseObject* resolved = nullptr;')
    elif arm == 'cut_entry':
        texts[ENTRY] = replace_one(texts[ENTRY], '        DoYoungGarbageCollection();', '        (void)0;')
    return texts


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ('source', 'build', 'elf', 'lib-dir', 'output'):
        p.add_argument('--' + key, required=True, type=Path)
    p.add_argument('--cores', required=True)
    p.add_argument('--samples', type=int, default=3)
    p.add_argument('--arms', default=','.join(ARMS))
    p.add_argument('--tests', default=','.join(TESTS))
    a = p.parse_args()
    if a.samples < 1: p.error('--samples must be positive')
    selected_arms = a.arms.split(',')
    selected_tests = a.tests.split(',')
    if set(selected_arms) - set(ARMS) or set(selected_tests) - set(TESTS): p.error('unknown arm or test')
    src, out = a.source.resolve(), a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    original = {name: (src / name).read_text() for name in (HEADER, PRODUCER, ENTRY, TABLE, WAIT)}
    elf = out / 'cj_gc_unit'
    shutil.copy2(a.elf, elf)
    # Exact restoration is an artifact contract. Compiler-cache path rewriting
    # can otherwise mix debug information from a caller's supplied build with
    # newly compiled mutation objects. Start from a full product build and use
    # the same uncached compiler environment for every arm. The ELF is already
    # retained outside the build tree before --clean-first can remove it.
    build_env = dict(os.environ, GC_UNIT_GATE_SKIP='1', CCACHE_DISABLE='1')
    for key in ('CCACHE_BASEDIR', 'CCACHE_NOHASHDIR', 'CCACHE_SLOPPINESS'):
        build_env.pop(key, None)
    record = {'source_head': subprocess.check_output(['git', '-C', str(src), 'rev-parse', 'HEAD'], text=True).strip(),
              'cores': a.cores, 'samples': a.samples, 'elf_sha256': sha(elf), 'arms': {},
              'build_environment': {key: build_env.get(key) for key in
                                    ('CCACHE_BASEDIR', 'CCACHE_NOHASHDIR', 'CCACHE_SLOPPINESS',
                                     'CCACHE_DISABLE', 'CCACHE_CONFIGPATH', 'SOURCE_DATE_EPOCH')},
              'source_before': {name: sha(src / name) for name in original}}
    manifest = out / 'manifest.json'
    def save(): manifest.write_text(json.dumps(record, indent=2) + '\n')
    try:
        for arm in selected_arms:
            dest = out / arm
            dest.mkdir()
            texts = mutate(original, arm)
            accepted = mutate(original, 'baseline')
            (dest / 'cut.diff').write_text(''.join(patch(accepted[n], texts[n], n) for n in original))
            (dest / 'guard-control.diff').write_text(patch(original[HEADER], accepted[HEADER], HEADER))
            for name, value in texts.items():
                if (src / name).read_text() != value: (src / name).write_text(value)
            env = build_env.copy()
            item = {'runs': [], 'build_started': time.time(),
                    'source_mtime': {name: (src / name).stat().st_mtime for name in original}}
            record['arms'][arm] = item
            with (dest / 'build.log').open('w') as log:
                command = ['taskset', '-c', a.cores, 'cmake', '--build', str(a.build),
                           '--target', 'cangjie-runtime', '-j16']
                if arm == 'baseline':
                    command.append('--clean-first')
                item['build_command'] = command
                built = subprocess.run(command, env=env,
                                       stdout=log, stderr=subprocess.STDOUT)
            item.update(build_rc=built.returncode, build_finished=time.time())
            save()
            if built.returncode: raise RuntimeError(f'{arm}: build rc={built.returncode}')
            for name in ('libcangjie-runtime.so', 'libboundscheck.so'): shutil.copy2(a.lib_dir / name, dest / name)
            item['sha256'] = {name: sha(dest / name) for name in ('libcangjie-runtime.so', 'libboundscheck.so')}
            item['elf_sha256'] = sha(elf)
            item['lineage'] = [s for s in subprocess.check_output(['strings', str(dest / 'libcangjie-runtime.so')],
                                text=True).splitlines() if 'CJRT-COMMIT:' in s]
            item['uptime_before'] = subprocess.check_output(['uptime'], text=True).strip()
            env.update(LD_LIBRARY_PATH=str(dest), CJ_GC_UNIT_FORWARD_DOMAIN='1', CJRT_LIFECLOCK_ENFORCE='1')
            env.pop('GC_UNIT_OTHER_VM_CHILD', None)
            cases = [(t, 'enforce') for t in selected_tests]
            if 'WrongLifecycle' in selected_tests: cases.append(('WrongLifecycle', 'audit'))
            cases.append(('ColourAddress.UncolorRoundTripAllRemapOneHot', 'control'))
            for sample in range(a.samples):
                for name, mode in cases:
                    test = name if mode == 'control' else 'ForwardReturnDomain.' + name
                    env['CJRT_LIFECLOCK_ENFORCE'] = '0' if mode == 'audit' else '1'
                    env['LIFECLOCK_AUDIT'] = '1' if mode == 'audit' else '0'
                    path = dest / f'{name}.{mode}.{sample}.log'
                    with path.open('w') as log:
                        run = subprocess.run(['taskset', '-c', a.cores, 'timeout', '35', str(elf),
                                              '--gtest_filter=' + test], env=env, stdout=log, stderr=subprocess.STDOUT)
                    output = path.read_text()
                    fails = ((arm == 'cut_identity' and
                              (name in ('Identity', 'RetiredHit') or mode == 'audit')) or
                             (arm == 'cut_copy_return' and name == 'NonIdentityCopy') or
                             (arm == 'cut_retired' and name == 'RetiredHit') or
                             (arm == 'cut_missing' and name == 'MissingEntry') or
                             (arm == 'cut_life' and name == 'WrongLifecycle' and mode == 'enforce') or
                             (arm == 'cut_unavailable' and name == 'Unavailable') or
                             (arm in ('cut_wait', 'cut_entry') and mode != 'control'))
                    expected = 1 if fails else 0
                    rec = {'test': test, 'mode': mode, 'sample': sample, 'rc': run.returncode,
                           'expected_rc': expected, 'started': '[  RUN   ] ' + test in output,
                           'single_test_tally': '[========] 1 tests:' in output,
                           'done0': 'done=0 status=' in output, 'done1': 'done=1 status=' in output,
                           'target_passes': output.count('DOMAIN target_assertion executed=1 matched=1'),
                           'target_failures': output.count('DOMAIN target_assertion executed=1 matched=0'),
                           'wait_entries': output.count('DOMAIN wait_entry=1'),
                           'producer_receipts': output.count('DOMAIN producer_receipt=1'),
                           'consumer_returns': output.count('DOMAIN consumer_return='),
                           'outer_guard_checks': output.count('ZRelocate::forward_object requires a forwarding entry'),
                           'log': str(path)}
                    rec['valid'] = rec['started'] and rec['single_test_tally'] and run.returncode == expected
                    if mode != 'control':
                        rec['valid'] &= ((rec['target_failures'] == 2 and rec['done0'] and rec['done1']) if fails else
                                         (rec['target_passes'] == 2 and rec['done0'] and rec['done1']))
                        if arm not in ('cut_wait', 'cut_entry'):
                            rec['valid'] &= rec['wait_entries'] == 2 and rec['producer_receipts'] == 2
                        if fails and arm in ('cut_identity', 'cut_copy_return'):
                            rec['valid'] &= rec['outer_guard_checks'] == 2
                        if fails and arm in ('cut_missing', 'cut_life', 'cut_unavailable'):
                            # A lost death assertion is only causal evidence if
                            # the product actually returned an invented answer.
                            rec['valid'] &= rec['consumer_returns'] == 2
                    item['runs'].append(rec)
                    print(f'{arm} {test} {mode} sample={sample} rc={run.returncode} expected={expected} valid={rec["valid"]}', flush=True)
                    save()
            item['uptime_after'] = subprocess.check_output(['uptime'], text=True).strip()
            save()
    finally:
        for name, value in original.items():
            if (src / name).read_text() != value: (src / name).write_text(value)
        record['source_after'] = {name: sha(src / name) for name in original}
        save()
    failures = [r for item in record['arms'].values() for r in item['runs'] if not r['valid']]
    if 'baseline' in record['arms'] and 'restored' in record['arms']:
        if record['arms']['baseline']['sha256'] != record['arms']['restored']['sha256']:
            failures.append({'restore_mismatch': True})
    record['failures'] = failures
    save()
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
