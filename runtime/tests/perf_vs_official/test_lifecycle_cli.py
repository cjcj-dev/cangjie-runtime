#!/usr/bin/env python3
"""Exercise the shipped analyzer CLI, including precise malformed-ledger errors."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from test_gclog_schema import complete_rows
from gclog_schema import parse_gclog

TOOL = Path(__file__).resolve().parents[3] / 'tools/zstat_pillars.py'


def ledger():
    rows = complete_rows()
    pauses = [p for p in parse_gclog('\n'.join(rows)).phases if p.kind == 'pause']
    rows[-1:-1] = [f'[GCLOG] v=5 rec=stw seq=1 gc_tag=y reason={p.name} start_ns={p.start_ns} wait_ns=0 held_ns={p.ns}' for p in pauses]
    return rows


class LifecycleCliTest(unittest.TestCase):
    def run_cli(self, rows):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'stderr'
            log.write_text('\n'.join(rows))
            return subprocess.run([sys.executable, str(TOOL), str(log), '--wall-ns', '1000'], capture_output=True, text=True)

    def test_current_records(self):
        result = self.run_cli(ledger())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('WORK_KIND kind=conc inclusive_ns=5', result.stdout)
        self.assertIn('CONTRACT_2_PAUSE_WALL verdict=PASS', result.stdout)
        print('PILLARS_POSITIVE_TARGET executed')

    def test_missing_collection(self):
        result = self.run_cli(ledger()[1:])
        self.assertEqual(result.returncode, 1)
        self.assertIn('seq=1 missing collection start', result.stdout)
        print('PILLARS_MISSING_COLLECTION_TARGET executed')

    def test_missing_phase(self):
        result = self.run_cli([r for r in ledger() if 'name=Pause_Relocate_Start ' not in r])
        self.assertEqual(result.returncode, 1)
        self.assertIn('seq=1 generation=Young_Generation missing phase Pause_Relocate_Start', result.stdout)
        print('PILLARS_MISSING_PHASE_TARGET executed')

    def test_pause_window(self):
        result = self.run_cli([r.replace('held_ns=1', 'held_ns=0') if 'rec=stw' in r else r for r in ledger()])
        self.assertEqual(result.returncode, 1)
        self.assertIn('CONTRACT_2_PAUSE_WALL verdict=FAIL', result.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
