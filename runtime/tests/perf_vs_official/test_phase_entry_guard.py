#!/usr/bin/env python3
"""Exercise the exact command-line consumer used by run_phase_entry_trigger.sh."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


GUARD = Path(__file__).with_name("phase_entry_guard.py")


def cycle(seq, kind="minor"):
    return (f"[GCLOG] v=3 rec=cycle seq={seq} kind={kind} reason=young "
            "start_ns=1 dur_ns=100 live_before=9 live_after=8 collected=1 "
            "heap_used=8 threshold=10 rss_kb=11")


def entry(seq, ns=1):
    return (f"[GCLOG] v=3 rec=phase seq={seq} name=young.flush_alloc "
            f"kind=pause start_ns=1 ns={ns}")


def log(mode="minor"):
    # Outer records deliberately satisfy all pre-existing guards on their own.
    # Keeping them in negative cases isolates the entry assertion.
    return [
        cycle(1),
        "[GCLOG] v=3 rec=stw seq=1 reason=prepare start_ns=1 wait_ns=1 held_ns=1",
        "[GCLOG] v=3 rec=phase seq=1 name=outer kind=pause start_ns=1 ns=1",
        "[GCLOG] v=3 rec=phase_leaf seq=1 name=outer ns=1 kind=pause "
        "depth=2 path_ok=1 path=outer>parent",
        f"PHASE_ENTRY_{mode.upper()}_OK checksum=1",
    ] + ([cycle(2, "major")] if mode == "major" else [])


class PhaseEntryGuardTest(unittest.TestCase):
    def check_guard(self, rows, mode="minor", missing=()):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "run.log"
            path.write_text("\n".join(rows) + "\n")
            result = subprocess.run([sys.executable, str(GUARD), mode, str(path)],
                                    capture_output=True, text=True)
        expected = ",".join(f"minor_entry_missing_seq={seq}" for seq in missing) or "none"
        # Both checks run: an unrelated early failure cannot stand in for entry rejection.
        with self.subTest(assertion="entry-result"):
            self.assertIn(f"errors={expected}\n", result.stdout, result.stderr)
        with self.subTest(assertion="exit-code"):
            self.assertEqual(result.returncode, int(bool(missing)), result.stdout + result.stderr)
        print(f"ENTRY_ASSERTION {self._testMethodName} rc={result.returncode} expected={expected}", flush=True)

    def test_normal_minor(self):
        self.check_guard(log() + [entry(1)])

    def test_normal_major_with_young_prelude(self):
        self.check_guard(log("major") + [entry(1)], mode="major")

    def test_outer_minor_without_entry(self):
        self.check_guard(log(), missing=(1,))

    def test_major_prelude_without_entry(self):
        self.check_guard(log("major"), mode="major", missing=(1,))

    def test_another_cycle_cannot_supply_entry(self):
        self.check_guard(log() + [cycle(2, "major"), entry(2)], missing=(1,))

    def test_unowned_entry_cannot_supply_entry(self):
        self.check_guard(log() + [entry(0)], missing=(1,))

    def test_every_minor_requires_entry(self):
        self.check_guard(log() + [entry(1), cycle(2)], missing=(2,))

    def test_zero_duration_entry_is_still_entry(self):
        self.check_guard(log() + [entry(1, ns=0)])


if __name__ == "__main__":
    unittest.main()
