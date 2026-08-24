#!/usr/bin/env python3
"""Adversarial matrix for the sole GCLOG/ZSTAT schema reader."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from analyze_stw import cycle_pauses
from gclog_schema import parse_gclog, parse_zstat, phase_ns_records


GOOD_PHASE = "[GCLOG] v=3 rec=phase seq=7 name=young.probe ns=999"
GOOD_LEAF = (
    "[GCLOG] v=3 rec=phase_leaf seq=7 name=young.copy_leaf ns=19 kind=conc "
    "depth=2 path_ok=1 path=young.copy_leaf>young.copy"
)
GOOD_CYCLE = (
    "[GCLOG] v=3 rec=cycle seq=7 kind=minor reason=young start_ns=1 dur_ns=100 "
    "live_before=9 live_after=8 collected=1 heap_used=8 threshold=10 rss_kb=11"
)
GOOD_STW = "[GCLOG] v=3 rec=stw seq=7 reason=young wait_ns=3 held_ns=4"
GOOD_ZPHASE = "[ZSTAT] v=1 rec=zphase seq=7 name=young.copy pause_ns=0 conc_ns=19 n=1"
GOOD_ZCYCLE = "[ZSTAT] v=1 rec=zcycle seq=7 pause_ns=0 conc_ns=19 max_pause_ns=0 phases=1"


class GcLogSchemaTest(unittest.TestCase):
    def test_v3_all_record_families_exact(self) -> None:
        text = "\n".join((GOOD_CYCLE, GOOD_PHASE, GOOD_LEAF, GOOD_STW, GOOD_ZPHASE, GOOD_ZCYCLE))
        gc = parse_gclog(text)
        zs = parse_zstat(text)
        self.assertEqual((len(gc.cycles), len(gc.phases), len(gc.phase_leaves), len(gc.stw)), (1, 1, 1, 1))
        self.assertEqual((gc.phases[0].ns, gc.phase_leaves[0].ns, gc.stw[0].held_ns), (999, 19, 4))
        self.assertEqual((len(zs.phases), len(zs.cycles), zs.phases[0].conc_ns), (1, 1, 19))

    def test_sub_microsecond_phase_keeps_ns(self) -> None:
        text = "\n".join((
            "[GCLOG] v=3 rec=phase seq=1 name=one ns=1",
            "[GCLOG] v=3 rec=phase seq=1 name=nine_nine_nine ns=999",
        ))
        self.assertEqual(phase_ns_records(text), [(1, "one", 1), (1, "nine_nine_nine", 999)])

    # Preserved ns-schema test names.
    def test_sub_microsecond_sample_keeps_nanoseconds(self) -> None:
        self.assertEqual(phase_ns_records(GOOD_PHASE), [(7, "young.probe", 999)])

    def test_v2_is_rejected_instead_of_scaled_as_v3(self) -> None:
        with self.assertRaises(ValueError):
            parse_gclog("[GCLOG] v=2 rec=phase seq=7 name=young.probe us=1")

    def test_malformed_v3_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            parse_gclog("[GCLOG] v=3 rec=phase seq=7 name=young.probe us=1")

    def test_mixed_bad_version_fails_closed(self) -> None:
        with self.assertRaises(ValueError):
            parse_gclog(GOOD_PHASE + "\n[GCLOG] v=x rec=phase seq=8 name=bad ns=1")

    def test_mixed_missing_version_fails_closed(self) -> None:
        with self.assertRaises(ValueError):
            parse_gclog(GOOD_PHASE + "\n[GCLOG] rec=phase seq=8 name=bad ns=1")

    def test_negative_duplicate_unknown_version_rejected(self) -> None:
        bad = (
            "[GCLOG] v=-1 rec=phase seq=1 name=p ns=1",
            "[GCLOG] v=3 v=3 rec=phase seq=1 name=p ns=1",
            "[GCLOG] v=4 rec=phase seq=1 name=p ns=1",
        )
        for line in bad:
            with self.subTest(line=line), self.assertRaises(ValueError):
                parse_gclog(line)

    def test_old_phase_v2_and_leaf_v1_rejected(self) -> None:
        bad = (
            "[GCLOG] v=2 rec=phase seq=1 name=p us=1",
            "[GCLOG] v=1 rec=phase_leaf seq=1 name=p us=1 kind=pause path=p",
        )
        for line in bad:
            with self.subTest(line=line), self.assertRaises(ValueError):
                parse_gclog(line)

    def test_phase_family_unknown_record_rejected(self) -> None:
        with self.assertRaises(ValueError):
            parse_gclog("[GCLOG] v=3 rec=phase_extra seq=1 name=p ns=1")

    def test_non_numeric_and_overflow_numbers_rejected(self) -> None:
        bad = (
            "[GCLOG] v=3 rec=phase seq=x name=p ns=1",
            "[GCLOG] v=3 rec=phase seq=1 name=p ns=x",
            f"[GCLOG] v=3 rec=phase seq={1 << 64} name=p ns=1",
            f"[GCLOG] v=3 rec=phase seq=1 name=p ns={1 << 64}",
        )
        for line in bad:
            with self.subTest(line=line), self.assertRaises(ValueError):
                parse_gclog(line)

    def test_missing_reordered_duplicate_extra_fields_rejected(self) -> None:
        bad = (
            "[GCLOG] v=3 rec=phase seq=1 name=p",
            "[GCLOG] v=3 rec=phase name=p seq=1 ns=1",
            "[GCLOG] v=3 rec=phase seq=1 seq=1 name=p ns=1",
            "[GCLOG] v=3 rec=phase seq=1 name=p ns=1 extra=1",
        )
        for line in bad:
            with self.subTest(line=line), self.assertRaises(ValueError):
                parse_gclog(line)

    def test_leaf_name_matches_first_path_component(self) -> None:
        with self.assertRaisesRegex(ValueError, "name/path mismatch"):
            parse_gclog(GOOD_LEAF.replace("name=young.copy_leaf", "name=other"))

    def test_leaf_path_component_delimiter_and_empty_component_rejected(self) -> None:
        bad = (
            GOOD_LEAF.replace("path=young.copy_leaf>young.copy", "path=young.copy_leaf>>young.copy"),
            GOOD_LEAF.replace("name=young.copy_leaf", "name=young>copy_leaf"),
        )
        for line in bad:
            with self.subTest(line=line), self.assertRaises(ValueError):
                parse_gclog(line)

    def test_leaf_depth_mismatch_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "depth mismatch"):
            parse_gclog(GOOD_LEAF.replace("depth=2", "depth=3"))

    def test_leaf_path_overflow_marker_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "overflow marker"):
            parse_gclog(GOOD_LEAF.replace("path_ok=1", "path_ok=0"))

    def test_malformed_zstat_candidate_rejected(self) -> None:
        with self.assertRaises(ValueError):
            parse_zstat(GOOD_ZPHASE + " extra=1")

    def test_subject_malformed_gclog_is_not_masked_by_official_fallback(self) -> None:
        with self.assertRaises(ValueError):
            cycle_pauses("stw time 9 us\n" + GOOD_CYCLE.replace("v=3", "v=x"))
        self.assertEqual(cycle_pauses("stw time 9 us"), {1: 9000})


if __name__ == "__main__":
    unittest.main(verbosity=2)
