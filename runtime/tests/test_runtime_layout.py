#!/usr/bin/env python3
"""Exercise the actual generator with real runtime assertion inputs."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

RUNTIME = Path(__file__).resolve().parents[1]
GENERATOR = RUNTIME / 'tools/generate-runtime-layout.py'


class RuntimeLayoutTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=RUNTIME / 'tests')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        # Keep input selection identical to the production generator.
        import runpy
        for source in runpy.run_path(str(GENERATOR))['SOURCES']:
            target = self.root / 'src' / source
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(RUNTIME / 'src' / source, target)
        (self.root / 'include').mkdir()
        shutil.copyfile(RUNTIME / 'include/CangjieRuntimeLayout.h', self.header)

    @property
    def header(self):
        return self.root / 'include/CangjieRuntimeLayout.h'

    def run_generator(self, *args):
        return subprocess.run([sys.executable, str(GENERATOR), '--runtime', str(self.root), *args],
                              text=True, capture_output=True)

    def test_real_runtime_input(self):
        result = self.run_generator()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('RUNTIME_LAYOUT checked:', result.stdout)

    def test_runtime_assertion_change_requires_header_regeneration(self):
        source = self.root / 'src/Sync/Sync.cpp'
        source.write_text(source.read_text().replace(
            'offsetof(CJMutex, state) == 24', 'offsetof(CJMutex, state) == 32'))
        result = self.run_generator()
        self.assertEqual(result.returncode, 1)
        self.assertIn('differs from runtime assertions', result.stderr)
        self.assertEqual(self.run_generator('--write').returncode, 0)
        self.assertIn('MutexStateOffset = 32;', self.header.read_text())
        self.assertEqual(self.run_generator().returncode, 0)

    def test_consumer_copy_drift(self):
        copy = self.root / 'compiler-copy.h'
        copy.write_text(self.header.read_text().replace('MutexStateOffset = 24', 'MutexStateOffset = 32'))
        result = self.run_generator('--header', str(copy))
        self.assertEqual(result.returncode, 1)
        self.assertIn('differs from runtime assertions', result.stderr)
        self.assertEqual(self.run_generator('--header', str(copy), '--write').returncode, 0)
        self.assertEqual(copy.read_bytes(), self.header.read_bytes())

    def test_malformed_assertion_is_not_silently_dropped(self):
        source = self.root / 'src/Sync/Sync.cpp'
        source.write_text(source.read_text().replace(
            'offsetof(CJMutex, state) == 24', 'offsetof(CJMutex, state) == (24)'))
        result = self.run_generator()
        self.assertEqual(result.returncode, 1)
        self.assertIn('unrecognized compiler layout assertion', result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
