"""The log observer releases only a completed minor, never a phase or a major."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_phase_entry_guard import cycle, entry

WAITER = Path(__file__).resolve().parents[1] / "gc_unit/wait_phase_entry_cycle.py"


class PhaseEntryWaitTest(unittest.TestCase):
    def run_fixture(self, record, expect_ack):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / "fixture"
            # This is a producer fixture for the observer, not a GC model.
            fixture.write_text(f"#!{sys.executable}\n" +
                               "import pathlib, sys, time\n" +
                               f"print({record!r}, flush=True)\n" +
                               ("receipt = pathlib.Path(sys.argv[1])\n"
                                "deadline = time.monotonic() + 2\n"
                                "while not receipt.exists() and time.monotonic() < deadline:\n"
                                "    time.sleep(0.01)\n"
                                "assert receipt.read_text() == 'seq=1\\n'\n"
                                if expect_ack else ""))
            fixture.chmod(0o700)
            result = subprocess.run([sys.executable, str(WAITER), str(fixture), str(root / "run.log")],
                                    capture_output=True, text=True, timeout=5)
            log = (root / "run.log").read_text()
            self.assertEqual(result.returncode, 0 if expect_ack else 1, log + result.stderr)
            self.assertEqual("PHASE_ENTRY_CYCLE_ACK seq=1" in log, expect_ack, log)
            self.assertIn(record, log)
            self.assertFalse(list(root.glob("cycle-ack-*")))

    def test_completed_minor_releases_receipt(self):
        self.run_fixture(cycle(1), True)

    def test_major_does_not_release_minor(self):
        self.run_fixture(cycle(1, "major"), False)

    def test_phase_does_not_release_minor(self):
        self.run_fixture(entry(1), False)


if __name__ == "__main__":
    unittest.main()
