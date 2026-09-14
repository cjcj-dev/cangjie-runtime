#!/usr/bin/env python3
"""Exercise the exact command-line consumer used by run_phase_entry_trigger.sh."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


GUARD = Path(__file__).with_name("phase_entry_guard.py")


def cycle(seq, kind="minor"):
    return (f"[GCLOG] v=4 rec=cycle seq={seq} gc_tag=- kind={kind} reason=young "
            "start_ns=1 dur_ns=100 live_before=9 live_after=8 collected=1 "
            "heap_used=8 threshold=10 rss_kb=11")


def entry(seq, ns=1, start=1, tag="-"):
    return (f"[GCLOG] v=4 rec=phase seq={seq} gc_tag={tag} name=young.flush_alloc "
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
    rows = log("major")
    for name, start, tag in (("major.preclean", 10, "Y"),
                             ("major.full_roots", 30, "Y"), ("major.old", 50, "O")):
        if entries and tag == "Y":
            rows.append(entry(1, start=start + 1, tag=tag))
        rows.append(f"[GCLOG] v=4 rec=phase seq=1 gc_tag={tag} name={name} "
                    f"kind=unknown start_ns={start} ns=10")
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
        for name in ("major.preclean", "major.full_roots", "major.old"):
            with self.subTest(span=name):
                self.check_guard([row for row in major_log() if f"name={name} " not in row],
                                 mode="major", errors="major_spans_seq=1")

    def test_major_spans_must_be_ordered(self):
        rows = major_log()
        rows[-1] = rows[-1].replace("start_ns=50", "start_ns=20")
        self.check_guard(rows, mode="major", errors="major_span_order_seq=1")

    def test_major_span_generation_is_checked(self):
        rows = [row.replace("gc_tag=O", "gc_tag=Y") for row in major_log()]
        self.check_guard(rows, mode="major", errors="major_span_tags_seq=1")

    def test_other_request_cannot_supply_major_spans(self):
        rows = log("major") + [cycle(2)] + [entry(2)]
        rows += [row.replace("seq=1", "seq=2") for row in major_log()[5:]]
        self.check_guard(rows, mode="major",
                         errors="major_explicit_shape=major,minor,major_spans_seq=1")

    def test_preclean_entry_cannot_supply_full_roots(self):
        rows = [row for row in major_log() if "name=young.flush_alloc" not in row or "start_ns=11" in row]
        self.check_guard(rows, mode="major", errors="major_entry_missing_seq=1_span=major.full_roots")

    def test_separate_minor_cycle_is_not_major_prelude(self):
        self.check_guard(log("major") + [cycle(2), entry(2)], mode="major",
                         errors="major_explicit_shape=major,minor,major_spans_seq=1")


if __name__ == "__main__":
    unittest.main()
