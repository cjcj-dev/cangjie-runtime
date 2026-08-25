#!/usr/bin/env python3
"""Semantic boundary tests for the young-STW analyzer."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from analyze_youngstw import evac_ghost_regions, report_timer_ends, timed_report_lines


class AnalyzeYoungStwTest(unittest.TestCase):
    def test_evac_ghost_window_excludes_previous_cycle_retirement(self) -> None:
        report_lines = timed_report_lines("\n".join((
            "2026-01-01 00:00:00.003000 1 young.ref_fix_bulk time: 30us",
            # ExpireKeptFromPreviousCycle runs in this inter-phase gap.
            "2026-01-01 00:00:00.003100 1 [GCV2][ghost-dispel] region=previous",
            # Current-cycle evacuation-tail retirement.
            "2026-01-01 00:00:00.003300 1 [GCV2][ghost-dispel] region=current",
            "2026-01-01 00:00:00.003500 1 young.evac_prepare_next time: 20us",
            "2026-01-01 00:00:00.003550 1 [GCV2][ghost-dispel] region=after-window",
            # The outer timer started at .003200 and closes after its nested timer.
            "2026-01-01 00:00:00.003600 1 young.evac_finish time: 400us",
        )))
        self.assertEqual(evac_ghost_regions(report_lines, report_timer_ends(report_lines)), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
