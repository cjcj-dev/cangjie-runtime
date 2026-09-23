#!/usr/bin/env python3
"""Exercise the exact command-line consumer used by run_phase_entry_trigger.sh."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


GUARD = Path(__file__).with_name("phase_entry_guard.py")


def generation(seq, name, tag, start=1, duration=100):
    return (f"[GCLOG] v=5 rec=generation seq={seq} gc_tag={tag} name={name} "
            f"start_ns={start} dur_ns={duration} live_before=9 live_after=8")


def cycle(seq, kind="minor"):
    return generation(seq, "Young_Generation" if kind == "minor" else "Old_Generation",
                      "y" if kind == "minor" else "O")


def entry(seq, ns=1, start=1, tag="y", name="Pause_Mark_Start"):
    return (f"[GCLOG] v=4 rec=phase seq={seq} gc_tag={tag} name={name} "
            f"kind=pause start_ns={start} ns={ns}")


def log(mode="minor"):
    # Outer records deliberately satisfy all pre-existing guards on their own.
    # Keeping them in negative cases isolates the entry assertion.
    return [
        cycle(1, mode),
        "[GCLOG] v=4 rec=stw seq=1 gc_tag=- reason=prepare start_ns=1 wait_ns=1 held_ns=1",
        "[GCLOG] v=4 rec=phase seq=1 gc_tag=- name=outer kind=pause start_ns=1 ns=1",
        "[GCLOG] v=4 rec=phase_leaf seq=1 gc_tag=- name=outer ns=1 kind=pause "
        "depth=2 path_ok=1 path=outer>parent",
        f"PHASE_ENTRY_{mode.upper()}_OK checksum=1",
    ]


def major_log(entries=True):
    rows = log("major")[1:]
    for name, start, tag in (("Young_Generation__Promote_All_", 10, "Y"),
                             ("Young_Generation__Collect_Roots_", 30, "Y"), ("Old_Generation", 50, "O")):
        if entries and tag == "Y":
            rows.append(entry(1, start=start + 1, tag=tag,
                              name="Pause_Mark_Start" if start == 10 else "Pause_Mark_Start__Major_"))
        rows.append(generation(1, name, tag, start, 10))
    return rows


class PhaseEntryGuardTest(unittest.TestCase):
    def check_guard(self, rows, mode="minor", missing=(), errors=None):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "run.log"
            path.write_text("\n".join(rows) + "\n")
            result = subprocess.run([sys.executable, str(GUARD), mode, str(path)],
                                    capture_output=True, text=True)
        expected = errors if errors is not None else (",".join(f"minor_entry_missing_seq={seq}" for seq in missing) or "none")
        # Both checks run: an unrelated early failure cannot stand in for entry rejection.
        with self.subTest(assertion="entry-result"):
            self.assertIn(f"errors={expected}\n", result.stdout, result.stderr)
        with self.subTest(assertion="exit-code"):
            self.assertEqual(result.returncode, int(expected != "none"), result.stdout + result.stderr)
        print(f"ENTRY_ASSERTION {self._testMethodName} rc={result.returncode} expected={expected}", flush=True)

    def test_normal_minor(self):
        self.check_guard(log() + [entry(1)])

    def test_normal_major_with_young_prelude(self):
        self.check_guard(major_log(), mode="major")

    def test_outer_minor_without_entry(self):
        self.check_guard(log(), missing=(1,))

    def test_major_prelude_without_entry(self):
        self.check_guard(major_log(entries=False), mode="major",
                         errors="major_entry_missing_seq=1_span=major.preclean,"
                                "major_entry_missing_seq=1_span=major.full_roots")

    def test_another_cycle_cannot_supply_entry(self):
        self.check_guard(log() + [cycle(2, "major"), entry(2)], missing=(1,))

    def test_unowned_entry_cannot_supply_entry(self):
        self.check_guard(log() + [entry(0)], missing=(1,))

    def test_every_minor_requires_entry(self):
        self.check_guard(log() + [entry(1), cycle(2)], missing=(2,))

    def test_zero_duration_entry_is_still_entry(self):
        self.check_guard(log() + [entry(1, ns=0)])


    def test_each_major_span_is_required(self):
        for name in ("Young_Generation__Promote_All_", "Young_Generation__Collect_Roots_", "Old_Generation"):
            with self.subTest(span=name):
                self.check_guard([row for row in major_log() if f"name={name} " not in row],
                                 mode="major", errors="major_spans_seq=1")

    def test_extra_major_generation_span_is_rejected(self):
        rows = major_log()
        rows.append(generation(1, "Young_Generation", "Y", 70, 10))
        self.check_guard(rows, mode="major", errors="major_spans_seq=1")

    def test_major_spans_must_be_ordered(self):
        rows = major_log()
        rows[-1] = rows[-1].replace("start_ns=50", "start_ns=20")
        self.check_guard(rows, mode="major", errors="major_span_order_seq=1")

    def test_major_span_generation_is_checked(self):
        rows = [row.replace("gc_tag=O", "gc_tag=Y") for row in major_log()]
        self.check_guard(rows, mode="major", errors="major_span_tags_seq=1")

    def test_other_request_cannot_supply_major_spans(self):
        rows = [row.replace("seq=1", "seq=2") if "name=Old_Generation " in row else row
                for row in major_log()]
        self.check_guard(rows, mode="major", errors="major_request_ids=1,2")

    def test_preclean_entry_cannot_supply_full_roots(self):
        rows = [row for row in major_log() if "name=Pause_Mark_Start" not in row or "start_ns=11" in row]
        self.check_guard(rows, mode="major", errors="major_entry_missing_seq=1_span=major.full_roots")

    def test_separate_minor_cycle_is_not_major_prelude(self):
        self.check_guard(log("major") + [cycle(2), entry(2)], mode="major",
                         errors="major_request_ids=1,2,major_spans_seq=1")


if __name__ == "__main__":
    unittest.main()
