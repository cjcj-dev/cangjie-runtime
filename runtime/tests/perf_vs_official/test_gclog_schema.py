#!/usr/bin/env python3
"""Adversarial matrix for the sole GCLOG/ZSTAT schema reader."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from analyze_stw import cycle_pauses, cycle_durs
from gclog_schema import parse_gclog, phase_ns_records


GOOD_PHASE = "[GCLOG] v=5 rec=phase seq=7 gc_tag=- name=young.probe kind=conc start_ns=1 ns=999"
GOOD_CYCLE = "[GCLOG] v=6 rec=cycle seq=7 gc_tag=- name=Minor_Collection cause=young event=start"
GOOD_STW = "[GCLOG] v=5 rec=stw seq=7 gc_tag=- reason=young start_ns=1 wait_ns=3 held_ns=4"


class GcLogSchemaTest(unittest.TestCase):
    def test_current_record_families_exact(self):
        gc = parse_gclog("\n".join((GOOD_CYCLE, GOOD_PHASE, GOOD_STW))).validate_complete()
        self.assertEqual((len(gc.cycles), len(gc.phases), len(gc.stw)), (1, 1, 1))
        self.assertEqual((gc.phases[0].ns, gc.stw[0].held_ns, gc.truncated), (999, 4, 1))

    def test_subphase_is_not_concurrent(self) -> None:
        row = GOOD_PHASE.replace("v=5", "v=5").replace("kind=conc", "kind=subphase")
        self.assertEqual(parse_gclog(row).phases[0].kind, "subphase")
        with self.assertRaises(ValueError):
            parse_gclog(row.replace("kind=subphase", "kind=invalid"))

    def test_generation_record_preserves_identity_and_ns(self) -> None:
        row = ("[GCLOG] v=6 rec=generation seq=7 gc_tag=Y name=Young_Generation "
               "event=end start_ns=1 dur_ns=99 used_at_collection_start=9 used_at_collection_end=8")
        record = parse_gclog(row).generations[0]
        self.assertEqual((record.seq, record.gc_tag, record.dur_ns), (7, "Y", 99))
        for bad in (row.replace("dur_ns=99", "dur_ns=x"), row + " extra=1",
                    row.replace("gc_tag=Y", "gc_tag=x")):
            with self.subTest(row=bad), self.assertRaises(ValueError):
                parse_gclog(bad)

    def test_generation_tags_preserved(self) -> None:
        text = "\n".join(GOOD_PHASE.replace("gc_tag=-", f"gc_tag={tag}") for tag in "yYO-")
        self.assertEqual([phase.gc_tag for phase in parse_gclog(text).phases], list("yYO-"))

    def test_missing_unknown_tag_and_v3_rejected(self) -> None:
        bad = (GOOD_PHASE.replace("gc_tag=- ", ""),
               GOOD_PHASE.replace("gc_tag=-", "gc_tag=x"),
               GOOD_PHASE.replace("v=5", "v=3"))
        for line in bad:
            with self.subTest(line=line), self.assertRaises(ValueError):
                parse_gclog(line)

    def test_sub_microsecond_phase_keeps_ns(self) -> None:
        text = "\n".join((
            "[GCLOG] v=5 rec=phase seq=1 gc_tag=- name=one kind=pause start_ns=1 ns=1",
            "[GCLOG] v=5 rec=phase seq=1 gc_tag=- name=nine_nine_nine kind=conc start_ns=1 ns=999",
        ))
        self.assertEqual(phase_ns_records(text), [(1, "one", 1), (1, "nine_nine_nine", 999)])

    # Preserved ns-schema test names.
    def test_sub_microsecond_sample_keeps_nanoseconds(self) -> None:
        self.assertEqual(phase_ns_records(GOOD_PHASE), [(7, "young.probe", 999)])

    def test_v2_is_rejected_instead_of_scaled_as_v4(self) -> None:
        with self.assertRaises(ValueError):
            parse_gclog("[GCLOG] v=2 rec=phase seq=7 gc_tag=- name=young.probe kind=conc start_ns=1 us=1")

    def test_malformed_v4_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            parse_gclog("[GCLOG] v=5 rec=phase seq=7 gc_tag=- name=young.probe kind=conc start_ns=1 us=1")

    def test_mixed_bad_version_fails_closed(self) -> None:
        with self.assertRaises(ValueError):
            parse_gclog(GOOD_PHASE + "\n[GCLOG] v=x rec=phase seq=8 gc_tag=- name=bad kind=conc start_ns=1 ns=1")

    def test_mixed_missing_version_fails_closed(self) -> None:
        with self.assertRaises(ValueError):
            parse_gclog(GOOD_PHASE + "\n[GCLOG] rec=phase seq=8 gc_tag=- name=bad kind=conc start_ns=1 ns=1")

    def test_negative_duplicate_unknown_version_rejected(self) -> None:
        bad = (
            "[GCLOG] v=-1 rec=phase seq=1 gc_tag=- name=p kind=conc start_ns=1 ns=1",
            "[GCLOG] v=5 v=4 rec=phase seq=1 gc_tag=- name=p kind=conc start_ns=1 ns=1",
            "[GCLOG] v=6 rec=phase seq=1 gc_tag=- name=p kind=conc start_ns=1 ns=1",
        )
        for line in bad:
            with self.subTest(line=line), self.assertRaises(ValueError):
                parse_gclog(line)

    def test_old_phase_v2_and_leaf_v1_rejected(self) -> None:
        bad = (
            "[GCLOG] v=2 rec=phase seq=1 gc_tag=- name=p kind=conc start_ns=1 us=1",
            "[GCLOG] v=1 rec=phase_leaf seq=1 gc_tag=- name=p us=1 kind=pause path=p",
        )
        for line in bad:
            with self.subTest(line=line), self.assertRaises(ValueError):
                parse_gclog(line)

    def test_phase_family_unknown_record_rejected(self) -> None:
        with self.assertRaises(ValueError):
            parse_gclog("[GCLOG] v=5 rec=phase_extra seq=1 gc_tag=- name=p ns=1")

    def test_non_numeric_and_overflow_numbers_rejected(self) -> None:
        bad = (
            "[GCLOG] v=5 rec=phase seq=x gc_tag=- name=p kind=conc start_ns=1 ns=1",
            "[GCLOG] v=5 rec=phase seq=1 gc_tag=- name=p kind=conc start_ns=1 ns=x",
            f"[GCLOG] v=5 rec=phase seq={1 << 64} gc_tag=- name=p kind=conc start_ns=1 ns=1",
            f"[GCLOG] v=5 rec=phase seq=1 gc_tag=- name=p kind=conc start_ns=1 ns={1 << 64}",
        )
        for line in bad:
            with self.subTest(line=line), self.assertRaises(ValueError):
                parse_gclog(line)

    def test_missing_reordered_duplicate_extra_fields_rejected(self) -> None:
        bad = (
            "[GCLOG] v=5 rec=phase seq=1 gc_tag=- name=p",
            "[GCLOG] v=5 rec=phase name=p seq=1 gc_tag=- kind=conc start_ns=1 ns=1",
            "[GCLOG] v=5 rec=phase seq=1 gc_tag=- seq=1 name=p kind=conc start_ns=1 ns=1",
            "[GCLOG] v=5 rec=phase seq=1 gc_tag=- name=p kind=conc start_ns=1 ns=1 extra=1",
        )
        for line in bad:
            with self.subTest(line=line), self.assertRaises(ValueError):
                parse_gclog(line)

    def test_retired_leaf_rejected(self):
        with self.assertRaises(ValueError):
            parse_gclog("[GCLOG] v=4 rec=phase_leaf seq=7 gc_tag=- name=p ns=1 kind=pause depth=1 path_ok=1 path=p")

    def test_subject_malformed_gclog_is_not_masked_by_official_fallback(self) -> None:
        with self.assertRaises(ValueError):
            cycle_pauses("stw time 9 us\n" + GOOD_CYCLE.replace("v=6", "v=x"))
        self.assertEqual(cycle_pauses("stw time 9 us"), {1: 9000})



# A1-A7 exercise the same completed-run validation used by the analyzers.
def collection(seq=1, event="start", kind="Minor"):
    row = f"[GCLOG] v=6 rec=cycle seq={seq} gc_tag=- name={kind}_Collection cause=young event={event}"
    return row + (" start_ns=1 dur_ns=100 used_at_start=9 used_at_end=8" if event == "end" else "")


def young_rows(seq=1):
    prefix = f"[GCLOG] v=6 rec=generation seq={seq} gc_tag=y name=Young_Generation event="
    names = [("Pause_Mark_Start", "pause"), ("Concurrent_Mark", "conc"), ("Pause_Mark_End", "pause"),
             ("Concurrent_Mark_Free", "conc"), ("Concurrent_Reset_Relocation_Set", "conc"),
             ("Concurrent_Select_Relocation_Set", "conc"), ("Pause_Relocate_Start", "pause"),
             ("Concurrent_Relocate", "conc")]
    return [prefix + "start"] + [
        f"[GCLOG] v=5 rec=phase seq={seq} gc_tag=y name={name} kind={kind} start_ns={i + 2} ns=1"
        for i, (name, kind) in enumerate(names)] + [
        prefix + "end start_ns=1 dur_ns=99 used_at_collection_start=9 used_at_collection_end=8"]


def complete_rows(seq=1):
    return [collection(seq)] + young_rows(seq) + [collection(seq, "end")]


class CompletenessTest(unittest.TestCase):
    def reject(self, label, rows, error):
        with self.assertRaisesRegex(ValueError, error):
            cycle_durs("\n".join(rows))
        print(f"TARGET_ASSERTION {label} executed", flush=True)

    def test_A1_missing_start(self):
        self.reject("A1", complete_rows()[1:] + complete_rows(2), "seq=1 missing collection start")

    def test_A2_missing_end(self):
        self.reject("A2", complete_rows()[:-1] + complete_rows(2), "seq=1 missing collection end")

    def test_A3_missing_phase(self):
        self.reject("A3", [r for r in complete_rows() if "name=Pause_Relocate_Start " not in r],
                    "seq=1 generation=Young_Generation missing phase Pause_Relocate_Start")

    def test_A4_abort(self):
        rows = [collection(), young_rows()[0], young_rows()[-1].split("event=")[0] + "event=abort", collection(event="abort")]
        records = parse_gclog("\n".join(rows)).validate_complete()
        self.assertEqual((records.aborted, records.truncated), (1, 0))
        self.assertEqual(cycle_durs("\n".join(rows)), ({}, {}))
        print("TARGET_ASSERTION A4 executed", flush=True)

    def test_A5_truncated_last(self):
        records = parse_gclog("\n".join(complete_rows() + [collection(2)])).validate_complete()
        self.assertEqual((records.aborted, records.truncated), (0, 1))
        print("TARGET_ASSERTION A5 executed", flush=True)

    def test_A7_analyzer_does_not_return_empty(self):
        self.reject("A7", complete_rows()[1:], "seq=1 missing collection start")

    def test_counts_change_with_input(self):
        self.assertEqual(len(cycle_durs("\n".join(complete_rows()))[0]), 1)
        self.assertEqual(len(cycle_durs("\n".join(complete_rows() + complete_rows(2)))[0]), 2)

    def test_wrong_generation_cannot_supply_phase(self):
        rows = [r.replace("gc_tag=y", "gc_tag=Y") if "name=Pause_Relocate_Start " in r else r for r in complete_rows()]
        self.reject("generation-isolation", rows, "missing phase Pause_Relocate_Start")

    def test_duplicate_terminal_rejected(self):
        self.reject("duplicate-end", complete_rows() + [collection(event="end")], "duplicate collection end")

if __name__ == "__main__":
    unittest.main(verbosity=2)
