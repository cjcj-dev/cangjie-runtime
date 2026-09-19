#!/usr/bin/env python3
"""Exercise the managed runner and the actual diff JSON consumer with fixtures.

Compiler/ELF fixtures test harness classification only, not runtime behavior.
Run on kkk2: python3 -m unittest <this file> -v
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class ManagedBuildStatus(unittest.TestCase):
    def run_managed(self, compiler_rc=0, emit=True, runtime_rc=0, stale=False):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sdk = root / "sdk"
            (sdk / "bin").mkdir(parents=True)
            cjc = sdk / "bin/cjc"
            cjc.write_text('#!/bin/bash\n' +
                           ('cp /bin/true "$2"\n' if emit else '') +
                           f'exit {compiler_rc}\n')
            cjc.chmod(0o755)
            fixtures = root / "source/runtime/tests/gc_unit"
            fixtures.mkdir(parents=True)
            names = {"finalizer_trigger": ["finalizer_trigger"],
                     "segmented_array_managed": ["segmented_array_managed"],
                     "phase_entry_trigger": ["phase_entry_minor", "phase_entry_major"]}
            for name, bins in names.items():
                script = fixtures / f"run_{name}.sh"
                script.write_text('#!/bin/bash\nset -e\n' +
                                  ''.join(f'"$CJC" -o "$GC_UNIT_OUT/{binary}"\n' for binary in bins) +
                                  f'"$GC_UNIT_OUT/{bins[0]}"\nexit {runtime_rc}\n')
            out = root / "out"
            if stale:
                for arm in ('h48', 'stained'):
                    for name, bins in zip(('finalizer', 'segmented', 'phase'), names.values()):
                        directory = out / f'{arm}_{name}_n1'
                        directory.mkdir(parents=True)
                        for binary in bins:
                            (directory / binary).write_bytes(Path('/bin/true').read_bytes())
            env = dict(os.environ, CANGJIE_HOME=str(sdk), SRCROOT=str(root / 'source'),
                       OUT=str(out), LANE=str(root), N='1')
            env.pop('CJC', None)
            result = subprocess.run(['bash', str(HERE / 'kkk2_managed.sh'), 'fixture-sha'],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return json.loads((out / 'kkk2_managed.json').read_text())

    def assert_build_fail(self, result):
        self.assertEqual(len(result['build_fail']), 6)
        self.assertEqual(result['failed'], [])
        for arm in result['arms'].values():
            self.assertEqual(list(arm['runs'].values()), [[], [], []])
            self.assertFalse(arm['all_zero'])

    def test_compiler_error_with_elf_is_build_fail(self):
        result = self.run_managed(compiler_rc=139)
        self.assert_build_fail(result)
        self.assertEqual(result['build_fail'][0]['compile_rc'], [139])
        print('TARGET compiler_rc_nonzero_with_elf classified build_fail')

    def test_missing_elf_with_success_rc_is_build_fail(self):
        self.assert_build_fail(self.run_managed(emit=False))

    def test_old_elf_cannot_hide_build_fail(self):
        self.assert_build_fail(self.run_managed(emit=False, stale=True))

    def test_runtime_139_stays_runtime_result(self):
        result = self.run_managed(runtime_rc=139)
        self.assertEqual(result['build_fail'], [])
        self.assertEqual(len(result['failed']), 6)
        for arm in result['arms'].values():
            self.assertEqual(list(arm['runs'].values()), [[139], [139], [139]])

    def test_successful_run(self):
        result = self.run_managed()
        self.assertEqual(result['build_fail'], [])
        self.assertEqual(result['failed'], [])
        self.assertTrue(all(arm['all_zero'] for arm in result['arms'].values()))

    def test_diff_marks_each_build_failure_not_run(self):
        # Execute the consumer embedded in the shipped service, not a model of it.
        script = (HERE / 'kkk2_diff.sh').read_text()
        consumer = script.split('<<\'PY\'\n', 1)[1].split('\nPY\n', 1)[0]
        for target in ('default', 'filler', 'testable', 'managed', None):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                for who in ('cand', 'base'):
                    lines = []
                    for arm in ('default', 'filler', 'testable', 'managed'):
                        fail = who == 'cand' and arm == target
                        rc = '123' if fail and arm != 'managed' else '0'
                        lines += [f'== {arm} rc={rc}', '== total tests 6']
                        if arm == 'managed':
                            lines += ['== build_fail ' + json.dumps([{'compile_rc': [139]}] if fail else [])]
                    (root / f'{who}.txt').write_text('\n'.join(lines) + '\n')
                result = subprocess.run(['python3', '-', tmp, 'a'*40, 'b'*40, 'hash'],
                                        input=consumer, text=True, capture_output=True)
                self.assertEqual(result.returncode, 3 if target else 0, result.stderr)
                data = json.loads((root / 'DIFF.json').read_text())
                for arm, value in data['arms'].items():
                    self.assertEqual(value['status'], 'NOT_RUN' if arm == target else 'ran')
                    self.assertEqual(value['cand_only'], None if arm == target else [])
                print(f'TARGET diff build_fail={target} status verified')


if __name__ == '__main__':
    unittest.main(verbosity=2)
