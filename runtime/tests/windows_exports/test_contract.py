#!/usr/bin/env python3
"""Exercise the shipped export CLI; no imported checker or duplicate model."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

PARSER = argparse.ArgumentParser()
PARSER.add_argument('--generator', type=Path, default=Path(__file__).resolve().parents[2] / 'build/generate_windows_exports.py')
PARSER.add_argument('--raw', type=Path)
ARGS, UNIT_ARGS = PARSER.parse_known_args()
SCRIPT = ARGS.generator.resolve()


class Contract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.raw = self.root / 'raw.def'
        self.expected = self.root / 'expected.def'
        self.refs = self.root / 'refs.json'
        self.raw.write_text('EXPORTS\n CJ_MCC_Object @1\n state @2 DATA\n _ZNSt3__1swapB9nqn220107Ev @3\n')
        self.refs.write_text(json.dumps({'symbols': {'CJ_MCC_Object': ['compiler/emitter.cpp:1'], 'state': ['std/native.cj:1']}}))
        self.expected.write_text('EXPORTS\n CJ_MCC_Object @1\n state @2 DATA\n')

    def cli(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)], capture_output=True, text=True)

    def check(self, raw=None):
        return self.cli('check', raw or self.raw, self.expected, '--references', self.refs)

    def test_generator_uses_only_consumer_references(self):
        result = self.cli('write', self.raw, self.expected, '--source-ref', 'fixture', '--references', self.refs)
        self.assertEqual(result.returncode, 0, result.stderr)
        contents = self.expected.read_text()
        self.assertIn('CJ_MCC_Object @1', contents)
        self.assertIn('state @2 DATA', contents)
        self.assertNotIn('_ZNSt3__1', contents)
        print('ASSERT generator_contract_projection PASS', flush=True)

    def test_missing_contract_is_exact_failure(self):
        self.raw.write_text(self.raw.read_text().replace(' CJ_MCC_Object @1\n', ''))
        result = self.check()
        self.assertEqual((result.returncode, result.stderr.splitlines()),
                         (1, ['FATAL: missing export: CJ_MCC_Object']))
        self.assertIn('missing=1 changed=0', result.stdout)
        print('ASSERT missing_contract_is_exact_failure PASS', flush=True)

    def test_abi_marker_and_ordinal_change_are_not_contract(self):
        for marker in ('220107', '220108'):
            self.raw.write_text(f'EXPORTS\n CJ_MCC_Object @17\n state @29 DATA\n _ZNSt3__1swapB9nqn{marker}Ev @42\n')
            result = self.check()
            self.assertEqual(result.returncode, 0, result.stderr)
        print('ASSERT two_abi_markers PASS', flush=True)

    def test_data_kind_remains_strict(self):
        self.raw.write_text(self.raw.read_text().replace(' DATA', ''))
        result = self.check()
        self.assertEqual(result.returncode, 1)
        self.assertIn('FATAL: changed export:', result.stderr)
        self.assertIn("name='state'", result.stderr)

    def test_missing_registered_entry_cannot_shrink_contract(self):
        self.expected.write_text('EXPORTS\n state @2 DATA\n')
        result = self.check()
        self.assertEqual(result.returncode, 2)
        self.assertIn('does not match the consumer reference set', result.stderr)

    def test_generator_rejects_missing_contract(self):
        self.raw.write_text('EXPORTS\n state @2 DATA\n')
        result = self.cli('write', self.raw, self.expected, '--source-ref', 'fixture', '--references', self.refs)
        self.assertEqual(result.returncode, 2)
        self.assertIn('missing consumer exports: CJ_MCC_Object', result.stderr)

    def test_collection_from_callers_resolves_constants_ignores_comments(self):
        source = self.root / 'compiler'
        source.mkdir()
        (source / 'emit.cpp').write_text('const std::string PREFIX = "CJ_";\nconst std::string BACKEND = PREFIX + "MCC_";\nemit(BACKEND + "Object");\n// _ZNSt3__1swapB9nqn220107Ev\nstate();\n')
        result = self.cli('collect', self.raw, self.refs, '--consumer', f'emitter:compiler@{"a" * 40}={source}')
        self.assertEqual(result.returncode, 0, result.stderr)
        references = json.loads(self.refs.read_text())
        self.assertEqual(references['symbols'], {'CJ_MCC_Object': ['compiler/emit.cpp:3']})
        self.assertEqual(references['inputs'][0]['files'], 1)

    def test_malformed_raw_is_rejected(self):
        self.raw.write_text('EXPORTS\n CJ_MCC_Object @1\n state @1 DATA\n')
        result = self.check()
        self.assertEqual(result.returncode, 2)
        self.assertIn('duplicate export ordinal', result.stderr)

    @unittest.skipUnless(ARGS.raw, 'pass --raw for the complete captured Windows export set')
    def test_real_input_and_each_contract_deletion(self):
        product = Path(__file__).resolve().parents[2]
        references = product / 'src/windows_export_references.json'
        expected = product / 'src/windows_x86_64_exports.def'
        raw_text = ARGS.raw.read_text()
        symbols = json.loads(references.read_text())['symbols']
        for marker in ('220107', '220108'):
            self.raw.write_text(raw_text.replace('B9nqn220107', 'B9nqn' + marker))
            result = self.cli('check', self.raw, expected, '--references', references)
            self.assertEqual(result.returncode, 0, result.stderr)
        # Every captured consumer symbol is independently necessary. Extra
        # implementation exports remain in each input, so failures are exact.
        for name in symbols:
            with self.subTest(symbol=name):
                self.raw.write_text('\n'.join(line for line in raw_text.splitlines()
                                              if not line.strip().startswith(name + ' @')) + '\n')
                result = self.cli('check', self.raw, expected, '--references', references)
                self.assertEqual((result.returncode, result.stderr.splitlines()),
                                 (1, ['FATAL: missing export: ' + name]))
        print(f'ASSERT real_input_each_deletion PASS n={len(symbols)} raw_sha256={hashlib.sha256(raw_text.encode()).hexdigest()}', flush=True)


if __name__ == '__main__':
    print(f'PRODUCT generator={SCRIPT} sha256={hashlib.sha256(SCRIPT.read_bytes()).hexdigest()}', flush=True)
    unittest.main(argv=[sys.argv[0], *UNIT_ARGS], verbosity=2)
