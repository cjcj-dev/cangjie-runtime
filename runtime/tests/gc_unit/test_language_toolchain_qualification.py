#!/usr/bin/env python3
"""Qualify admission against real SDK files, including one-byte changes.

All modified files live in --out. No compiler or runtime is substituted.
The acceptance/control group can also be run against a temporarily cut record.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest


class QualificationTests(unittest.TestCase):
    def invoke(self, changes=None):
        env = dict(os.environ, **(changes or {}))
        env.pop('CJC', None)
        result = subprocess.run([sys.executable, str(UNIT / 'language_toolchain.py')],
                                env=env, capture_output=True, text=True)
        path = OUT / f'admission-{self._testMethodName}-{self.invocations}.json'
        self.invocations += 1
        path.write_text(json.dumps(dict(rc=result.returncode, stdout=result.stdout,
                                        stderr=result.stderr), indent=2))
        return result

    def setUp(self):
        self.invocations = 0

    def test_accept_current_tuple(self):
        result = self.invoke()
        print(f'CURRENT_TUPLE_ACCEPTANCE_ASSERT executed rc={result.returncode}', flush=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        identity = json.loads(result.stdout)
        self.assertEqual(identity['language']['std_core'],
                         '47a91409292a5005f1dd3449bbc6b0840f89f3bbdb3ebba1235837cb768d3f60')
        self.assertEqual(identity['language']['cjc'],
                         'a4ae3418cb59c07b273056e6f2fd9a6908c4420da15f98bf63a8dfcf62ed15ad')

    def test_reject_old_qualification(self):
        result = self.invoke({'GC_UNIT_LANGUAGE_QUALIFICATION': str(OLD)})
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn('LANGUAGE_QUALIFICATION_UNKNOWN', result.stderr)
        print('OLD_QUALIFICATION_REJECTION_ASSERT executed rc=2', flush=True)

    def test_reject_each_changed_component(self):
        sdk = OUT / 'changed-sdk'
        shutil.copytree(Path(os.environ['GC_UNIT_LANGUAGE_SDK']), sdk, symlinks=True)
        proof = json.loads(Path(os.environ['GC_UNIT_LANGUAGE_QUALIFICATION']).read_text())
        changes = {'GC_UNIT_LANGUAGE_SDK': str(sdk),
                   'GC_UNIT_CJC_RUNTIME_LIB_DIR': str(sdk / 'host/compiler')}
        paths = {'bin/cjc': 'component=cjc',
                 'third_party/llvm/bin/llc': 'component=llc',
                 'third_party/llvm/bin/opt': 'component=opt',
                 'runtime/lib/linux_x86_64_cjnative/libcangjie-runtime.so': 'component=runtime'}
        for archive in sorted((sdk / 'lib/linux_x86_64_cjnative').glob('libcangjie-std-*.a')):
            paths[str(archive.relative_to(sdk))] = 'component=std'
        # sdk_identity runs before the additional file bindings. Aliases are
        # tested once at their actual executable, so the expected first reject
        # names the language entry when a component is also one of those files.
        for relative in proof['components']:
            target = (sdk / relative).resolve()
            existing = next((name for name in paths if (sdk / name).resolve() == target), None)
            if existing is None:
                paths[relative] = f'component={relative}'
        cases = [(sdk / relative, reason, changes) for relative, reason in paths.items()]
        cases += [(file, 'LANGUAGE_COMPILER_HOST_INCOMPATIBLE', changes)
                  for file in sorted((sdk / 'host/compiler').iterdir()) if file.is_file()]
        for variable in ('GC_UNIT_COLOUR_CHECKER', 'GC_UNIT_COLOUR_HOST_RUNTIME'):
            copy = OUT / variable.lower()
            shutil.copy2(os.environ[variable], copy)
            cases.append((copy, 'LANGUAGE_COLOUR_REFERENCE_INCOMPATIBLE',
                          dict(changes, **{variable: str(copy)})))
        try:
            for file, reason, env in cases:
                with self.subTest(component=str(file.relative_to(OUT))):
                    # Follow SDK aliases only within this independent copy.
                    target = file.resolve()
                    self.assertTrue(target.is_relative_to(OUT.resolve()), str(target))
                    mode = target.stat().st_mode
                    target.chmod(mode | 0o200)
                    with target.open('r+b') as stream:
                        original = stream.read(1)
                        self.assertTrue(original, str(file))
                        stream.seek(0)
                        stream.write(bytes([original[0] ^ 1]))
                    try:
                        result = self.invoke(env)
                        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                        self.assertIn(reason, result.stderr)
                        print(f'ONE_BYTE_REJECTION_ASSERT component={file.relative_to(OUT)} '
                              f'rc=2 reason={reason}', flush=True)
                    finally:
                        with target.open('r+b') as stream:
                            stream.write(original)
                        target.chmod(mode)
        finally:
            shutil.rmtree(sdk)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--old-qualification', type=Path, required=True)
    parser.add_argument('--group', choices=('acceptance', 'mutations'), default='acceptance')
    args = parser.parse_args()
    UNIT = Path(__file__).resolve().parent
    OUT = args.out.resolve()
    OUT.mkdir(parents=True, exist_ok=False)
    OLD = args.old_qualification.resolve(strict=True)
    names = (['test_accept_current_tuple', 'test_reject_old_qualification']
             if args.group == 'acceptance' else ['test_reject_each_changed_component'])
    suite = unittest.TestSuite(QualificationTests(name) for name in names)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(not result.wasSuccessful())
