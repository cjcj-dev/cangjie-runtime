#!/usr/bin/env python3
"""Exercise invocation ownership through the real gate entry."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


GATE = Path(__file__).with_name('gate_gc_unit.sh')


def inventory(root):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob('*') if path.is_file()}


class GateEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='gate-evidence.', dir=os.environ['TMPDIR'])
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def invoke(self, name, mode='invalid', skip=False):
        handoff = self.root / (name + '.json')
        env = dict(os.environ, GC_UNIT_OUT=str(self.root / 'unit'),
                   GC_UNIT_GATE_STATUS=str(self.root / 'latest.status'),
                   GC_UNIT_GATE_RESULT=str(handoff), GC_UNIT_GATE_LANGUAGE_TESTS=mode,
                   GC_UNIT_GATE_CONTRACT_SELFTEST='1', GC_UNIT_GATE_SKIP=str(int(skip)),
                   MRT_TESTABLE_INTERNALS='0')
        result = subprocess.run(['bash', str(GATE)], env=env, capture_output=True, text=True)
        locator = json.loads(handoff.read_text())
        run = Path(locator['evidence_dir'])
        receipt = json.loads((run / 'invocation.json').read_text())
        self.assertEqual(receipt['gate_rc'], result.returncode)
        self.assertEqual(locator['run_id'], run.name)
        self.assertIn('RUN_ID=' + run.name, (run / 'gate.status').read_text())
        self.assertEqual(receipt['verified_identity'], None)
        return result, run, receipt

    def test_failure_then_skip_retains_history(self):
        result, first, receipt = self.invoke('failure')
        self.assertEqual(result.returncode, 2)
        self.assertEqual(receipt['reason'], 'INVALID_LANGUAGE_TEST_MODE')
        self.assertIn('must be all, defer, or only', (first / 'gate.stderr.log').read_text())
        before = inventory(first)
        result, second, receipt = self.invoke('skip', mode='all', skip=True)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(receipt['state'], 'NOT_RUN')
        self.assertEqual(receipt['reason'], 'EXPLICIT_SKIP')
        self.assertNotEqual(first, second)
        self.assertEqual(before, inventory(first))
        print('HISTORY_ASSERT failure_then_skip immutable=True', flush=True)

    def test_concurrent_failures_are_disjoint(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            futures = [executor.submit(self.invoke, name) for name in ('one', 'two')]
            results = [future.result() for future in futures]
        self.assertEqual([result[0].returncode for result in results], [2, 2])
        self.assertNotEqual(results[0][1], results[1][1])
        for result, run, receipt in results:
            self.assertEqual(receipt['state'], 'FAIL')
            self.assertEqual(result.stderr, (run / 'gate.stderr.log').read_text())
            for name, digest in receipt['outputs'].items():
                self.assertEqual(hashlib.sha256((run / name).read_bytes()).hexdigest(), digest)
        print('HISTORY_ASSERT concurrent_disjoint=True', flush=True)


if __name__ == '__main__':
    unittest.main()
