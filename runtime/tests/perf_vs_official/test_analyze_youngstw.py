#!/usr/bin/env python3
"""Semantic boundary tests for the young-STW analyzer."""

import json
import subprocess
import sys
import tempfile
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

    @staticmethod
    def _write_fixture(root: Path, cas_fail: int) -> None:
        phases = [
            "young.flush_alloc", "young.prepare_candidates", "young.remset_drain",
            "young.root_enum", "young.mark_closure", "young.remset_rescan",
            "young.mark_from_remset", "young.pre_evac_clear", "young.ref_fix_prepare",
            "young.ref_fix_root_pass1", "young.ref_fix_bulk", "young.evac_finish",
            "young.evac_prepare_next",
        ]
        phase_ns = {
            name: 10000 for name in phases
        }
        phase_ns.update({
            "young.ref_fix_bulk": 20000,
            "young.evac_finish": 100000,
            "young.evac_prepare_next": 20000,
        })
        stderr = "\n".join(
            f"[GCLOG] v=3 rec=phase seq=1 name={name} kind=pause start_ns=0 ns={phase_ns[name]}"
            for name in phases
        ) + "\n" + "\n".join(
            f"[GCLOG] v=3 rec=stw seq=1 reason={reason} start_ns=0 wait_ns=0 held_ns=1000000"
            for reason in ("young_collection", "young_post-relocate", "other_a", "other_b")
        ) + "\n"
        report = "\n".join((
            "2026-01-01 00:00:00.001000 1 young.mark_from_remset time: 10us",
            "2026-01-01 00:00:00.001200 1 young.pre_evac_clear time: 100us",
            "2026-01-01 00:00:00.003200 1 young.ref_fix_bulk time: 20us",
            "2026-01-01 00:00:00.003250 1 [GCV2][ghost-dispel] region=previous",
            "2026-01-01 00:00:00.003500 1 [GCV2][ghost-dispel] region=current",
            "2026-01-01 00:00:00.003900 1 young.evac_prepare_next time: 20us",
            "2026-01-01 00:00:00.004000 1 young.evac_finish time: 700us",
            "[GCV2Minor] run=1 candidates=2 candidateBytes=100 liveBytes=10 remembered=1",
            "[GCV2][markpar] reachable_n=2",
            "[GCV2Minor] y2yDirtyHolders=1",
            "[GCV2][remsetdrain] recorded=3 live=1 consumed=1 interiors=1",
            f"[GCV2][reffix][conc_heap] nObj=1 nSlot=3 cas_ok=1 cas_fail={cas_fail}",
            "remembered-set promoteReplay=1 residualPromote=1 youngRegionCount=2",
            "[PROMODOMAIN][DISCHARGE] edges=1 ns=10000 registered=1 tableBytes≈64",
        ))
        runtime = "\n".join((
            "[GCV2][installdomain] pregrant grant=1 already=1 tooLate=0 skip=0",
            "[GCV2][youngstatic] pregrant_static young=1 ensureCalls=1 missAfter=0",
        ))
        reasons = root / "reasons"
        for index in range(10):
            run = reasons / f"r{index:02d}-subject"
            run.mkdir(parents=True, exist_ok=True)
            (run / "classification").write_text("COMPLETE\n")
            (run / "stderr").write_text(stderr)
            # Keep the ten samples byte-distinct while preserving the same
            # boundary invariant and event cardinality.
            run_report = report.replace(
                "00:00:00.003500", f"00:00:00.0035{index:02d}")
            (run / "report.log.1").write_text(run_report)
            (run / "runtime.log.1").write_text(runtime)

    def test_cli_rejects_injected_counter_and_accepts_legal_input(self) -> None:
        analyzer = Path(__file__).with_name("analyze_youngstw.py")
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self._write_fixture(root, cas_fail=0)
            green = subprocess.run(
                [sys.executable, str(analyzer), str(root), "--compact"],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(green.returncode, 0, green.stderr)
            self.assertEqual(json.loads(green.stdout)["n"], 10)

            self._write_fixture(root, cas_fail=999)
            red = subprocess.run(
                [sys.executable, str(analyzer), str(root), "--compact"],
                text=True, capture_output=True, check=False,
            )
            self.assertNotEqual(red.returncode, 0)
            self.assertIn("cas_ok + cas_fail <= n_slot", red.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
