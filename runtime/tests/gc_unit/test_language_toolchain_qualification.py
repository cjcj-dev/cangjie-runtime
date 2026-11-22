#!/usr/bin/env python3
"""Qualify admission against real SDK files, including one-byte changes.

Mutations are confined to --out and never execute a modified SDK component.
Use the acceptance group for read-only checks of a real installation.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

from language_toolchain import compiler_host_dirs, launcher_entry


def component_cases(sdk, proof, host_dirs):
    """Find each physical file and the first check that binds it."""
    language = {}
    entries = set()
    for name, relative in (
        ('cjc', 'bin/cjc'),
        ('llc', 'third_party/llvm/bin/llc'),
        ('opt', 'third_party/llvm/bin/opt'),
    ):
        launcher = sdk / relative
        language[name] = [launcher]
        entry = launcher_entry(sdk, launcher)
        if entry:
            language[f'{name}_entry'] = [entry['entry']]
            language[f'{name}_entity'] = [entry['entity']]
            entries.add(entry['entry'].resolve(strict=True))

    std = sdk / 'lib/linux_x86_64_cjnative'
    language['std'] = sorted(std.glob('libcangjie-std-*.a'))
    language['std_core'] = [std / 'libcangjie-std-core.a']
    language['runtime'] = [sdk / 'runtime/lib/linux_x86_64_cjnative/libcangjie-runtime.so']
    cases = {}

    def add(file, reason):
        target = file.resolve(strict=True)
        cases.setdefault(target, (file, reason, target in entries))

    # Admission compares language keys in record order, then path bindings,
    # then the host set. Aliases must expect the same first check as their file.
    for key in proof['language']:
        for file in language[key]:
            add(file, f'LANGUAGE_TOOLCHAIN_INCOMPATIBLE component={key}')
    for relative in proof['components']:
        add(sdk / relative, f'LANGUAGE_TOOLCHAIN_INCOMPATIBLE component={relative}')
    for directory in host_dirs:
        for file in sorted(directory.iterdir()):
            if file.is_file():
                add(file, 'LANGUAGE_COMPILER_HOST_INCOMPATIBLE')
    return list(cases.values())


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
                         '6484711bfc827990534249f65e2ee5f2c7979e4df284c4b92f88ad3814f50b98')
        self.assertEqual(identity['language']['cjc_entity'],
                         '3be32b480581efb2c9e40708551c4cac1f13a08113104e1c5007f9e99b01138c')
        self.assertEqual(identity['qualified_components'].get('third_party/llvm/lib/libLLVM-15.so'),
                         '8b344d0c1aa92bd40938e80faa783d1283ecbdb5b129a2c08605b51edf53e053')

    def test_reject_old_qualification(self):
        result = self.invoke({'GC_UNIT_LANGUAGE_QUALIFICATION': str(OLD)})
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn('LANGUAGE_QUALIFICATION_UNKNOWN', result.stderr)
        print('OLD_QUALIFICATION_REJECTION_ASSERT executed rc=2', flush=True)

    def test_reject_old_tuple(self):
        result = self.invoke({'GC_UNIT_LANGUAGE_SDK': str(OLD_SDK)})
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn('LANGUAGE_TOOLCHAIN_INCOMPATIBLE component=cjc', result.stderr)
        print('OLD_TUPLE_REJECTION_ASSERT executed rc=2 component=cjc', flush=True)

    def test_reject_each_changed_component(self):
        source = Path(os.environ['GC_UNIT_LANGUAGE_SDK']).resolve(strict=True)
        host = Path(os.environ['GC_UNIT_CJC_RUNTIME_LIB_DIR']).resolve(strict=True)
        proof = json.loads(Path(os.environ['GC_UNIT_LANGUAGE_QUALIFICATION']).read_text())
        sdk = OUT / 'changed-sdk'
        try:
            shutil.copytree(source, sdk, symlinks=True)
            host_dirs, legacy = compiler_host_dirs(sdk)
            # A legacy host can be supplied from an equivalent external directory.
            # Launcher hosts are bound to the configured directory inside the SDK.
            copied_host = host_dirs[0] if legacy else sdk / host.relative_to(source)
            changes = {'GC_UNIT_LANGUAGE_SDK': str(sdk),
                       'GC_UNIT_CJC_RUNTIME_LIB_DIR': str(copied_host)}
            cases = [(file, reason, is_entry, changes)
                     for file, reason, is_entry in component_cases(sdk, proof, host_dirs)]
            for variable in ('GC_UNIT_COLOUR_CHECKER', 'GC_UNIT_COLOUR_HOST_RUNTIME'):
                copy = OUT / variable.lower()
                shutil.copy2(os.environ[variable], copy)
                cases.append((copy, 'LANGUAGE_COLOUR_REFERENCE_INCOMPATIBLE', False,
                              dict(changes, **{variable: str(copy)})))

            for file, reason, is_entry, env in cases:
                with self.subTest(component=str(file.relative_to(OUT))):
                    target = file.resolve(strict=True)
                    self.assertTrue(target.is_relative_to(OUT.resolve()), str(target))
                    mode = target.stat().st_mode
                    target.chmod(mode | 0o200)
                    original = None
                    try:
                        with target.open('r+b') as stream:
                            original = stream.read() if is_entry else stream.read(1)
                            self.assertTrue(original, str(file))
                            if is_entry:
                                # Add one slash to a search directory. Keep a valid
                                # recipe and identical resolution so the hash check
                                # runs instead of failing in the entry parser.
                                lines = original.split(b'\n')
                                lines[5] += b'/'
                                changed = b'\n'.join(lines)
                            else:
                                changed = bytes([original[0] ^ 1])
                            stream.seek(0)
                            stream.write(changed)
                        result = self.invoke(env)
                        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                        self.assertEqual(result.stderr.strip(),
                                         f'LANGUAGE_ADMISSION_NOT_RUN reason={reason}')
                        print(f'ONE_BYTE_REJECTION_ASSERT component={file.relative_to(OUT)} '
                              f'rc=2 reason={reason}', flush=True)
                    finally:
                        if original:
                            with target.open('r+b') as stream:
                                stream.write(original)
                                if is_entry:
                                    stream.truncate()
                        target.chmod(mode)
        finally:
            if sdk.exists():
                shutil.rmtree(sdk)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--old-qualification', type=Path, required=True)
    parser.add_argument('--old-sdk', type=Path, required=True)
    parser.add_argument('--group', choices=('acceptance', 'mutations'), default='acceptance')
    args = parser.parse_args()
    UNIT = Path(__file__).resolve().parent
    OUT = args.out.resolve()
    OUT.mkdir(parents=True, exist_ok=False)
    OLD = args.old_qualification.resolve(strict=True)
    OLD_SDK = args.old_sdk.resolve(strict=True)
    names = (['test_accept_current_tuple', 'test_reject_old_qualification', 'test_reject_old_tuple']
             if args.group == 'acceptance' else ['test_reject_each_changed_component'])
    suite = unittest.TestSuite(QualificationTests(name) for name in names)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(not result.wasSuccessful())
