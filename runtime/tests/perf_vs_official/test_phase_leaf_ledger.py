#!/usr/bin/env python3
"""Structural five-pillar ledger and cycle-join regression tests."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from analyze_stw import pillars
from gclog_schema import phase_leaf_ledger


ROOT = Path(__file__).resolve().parents[3]


def cycle(seq: int, dur_ns: int = 100) -> str:
    return (
        f"[GCLOG] v=3 rec=cycle seq={seq} kind=minor reason=young start_ns=1 dur_ns={dur_ns} "
        "live_before=9 live_after=8 collected=1 heap_used=8 threshold=10 rss_kb=11"
    )


def phase(seq: int, name: str, ns: int) -> str:
    return f"[GCLOG] v=3 rec=phase seq={seq} name={name} ns={ns}"


def leaf(seq: int, name: str, ns: int, path: str | None = None, kind: str = "unknown") -> str:
    path = path or name
    return (
        f"[GCLOG] v=3 rec=phase_leaf seq={seq} name={name} ns={ns} kind={kind} "
        f"depth={len(path.split('>'))} path_ok=1 path={path}"
    )


class PhaseLeafLedgerTest(unittest.TestCase):
    # Preserved leaf-b3 test names, now expressed in the unified v3/ns contract.
    def test_dynamic_parent_and_leaf_use_runtime_structure(self) -> None:
        text = "\n".join((
            phase(1, "young.ref_fix", 90),
            phase(1, "young.ref_fix_bulk", 50),
            leaf(1, "young.ref_fix_bulk_roots", 20,
                 "young.ref_fix_bulk_roots>young.ref_fix_bulk>young.ref_fix", "pause"),
            leaf(1, "young.ref_fix_bulk_heap", 30,
                 "young.ref_fix_bulk_heap>young.ref_fix_bulk>young.ref_fix", "pause"),
            leaf(2, "young.ref_fix_bulk", 40, kind="pause"),
            cycle(1, 100),
            cycle(2, 50),
        ))
        rows = phase_leaf_ledger(text)["cycles"]
        self.assertEqual([row["pillars_ns"]["ref_fix"] for row in rows], [50, 40])

    def test_five_pillar_consumer_ignores_inclusive_parent_records(self) -> None:
        text = "\n".join((
            phase(1, "young.concurrent_relocate", 80),
            phase(1, "young.copy", 70),
            leaf(1, "ForwardFromRegions", 70,
                 "ForwardFromRegions>young.copy>young.concurrent_relocate", "conc"),
            cycle(1, 100),
        ))
        row = phase_leaf_ledger(text)["cycles"][0]
        self.assertEqual(row["leaf_pillar_ns"], 70)
        self.assertEqual(row["pillars_ns"]["copy"], 70)
        aggregated, names, total, _conc, _wait = pillars(text)
        self.assertEqual(aggregated, {"ref_fix": 0.0, "mark": 0.0, "evac_finish": 0.0,
                                      "drain": 0.0, "copy": 0.07})
        self.assertEqual(names["young.concurrent_relocate"], 0.08)
        self.assertAlmostEqual(total, 0.15)

    def test_cycle_bound_rejects_overfull_leaf_set(self) -> None:
        with self.assertRaisesRegex(ValueError, r"inequality_1.*seq=1 sum_ns=101 dur_ns=100"):
            phase_leaf_ledger("\n".join((leaf(1, "young.copy", 101), cycle(1, 100))))

    def test_inclusive_only_input_is_rejected_not_silently_zeroed(self) -> None:
        # The unified cycle-master rule supersedes the old empty-result behavior: the inclusive
        # parent is still excluded, but the cycle remains visible as an explicit zero row.
        text = "\n".join((phase(1, "young.copy", 70), cycle(1)))
        row = phase_leaf_ledger(text)["cycles"][0]
        self.assertEqual(row["leaf_pillar_ns"], 0)
        self.assertEqual(set(row["pillars_ns"].values()), {0})
        aggregated, names, total, _conc, _wait = pillars(text)
        self.assertEqual(set(aggregated.values()), {0.0})
        self.assertEqual(names["young.copy"], 0.07)
        self.assertEqual(total, 0.07)

    def test_cycle_left_join_keeps_zero_leaf_cycle(self) -> None:
        rows = phase_leaf_ledger("\n".join((leaf(1, "young.copy", 5), cycle(1), cycle(2))))["cycles"]
        self.assertEqual([row["seq"] for row in rows], [1, 2])
        self.assertEqual(rows[1]["leaf_pillar_ns"], 0)
        self.assertEqual(set(rows[1]["pillars_ns"].values()), {0})

    def test_positive_seq_orphan_and_duplicate_cycle_rejected(self) -> None:
        bad = (
            "\n".join((cycle(1), leaf(2, "young.copy", 1))),
            "\n".join((cycle(1), cycle(1))),
        )
        for text in bad:
            with self.subTest(text=text), self.assertRaises(ValueError):
                phase_leaf_ledger(text)

    def test_unowned_nonpillar_leaf_is_reported(self) -> None:
        result = phase_leaf_ledger("\n".join((cycle(1), leaf(0, "Finalizer", 7))))
        self.assertEqual(result["unowned_nonpillar_leaf_records"], 1)
        self.assertEqual(result["unowned_nonpillar_leaf_ns"], 7)
        self.assertEqual(result["unowned_nonpillar_names"], ["Finalizer"])

    def test_unknown_kind_is_explicitly_unavailable(self) -> None:
        result = phase_leaf_ledger("\n".join((cycle(1), leaf(1, "young.copy", 7))))
        self.assertEqual(result["phase_leaf_kind_classification"], "unavailable")
        self.assertEqual(result["owned_phase_leaf_kind_counts"],
                         {"pause": 0, "conc": 0, "unknown": 1})

    def test_unowned_five_pillar_leaf_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "unowned five-pillar"):
            phase_leaf_ledger("\n".join((cycle(1), leaf(0, "young.copy", 1))))

    def test_first_match_mark_before_drain(self) -> None:
        row = phase_leaf_ledger("\n".join((cycle(1), leaf(1, "young.mark_from_remset", 9))))["cycles"][0]
        self.assertEqual(row["pillars_ns"]["mark"], 9)
        self.assertEqual(row["pillars_ns"]["drain"], 0)

    def test_first_match_priority_beats_ancestor_distance(self) -> None:
        row = phase_leaf_ledger("\n".join((
            cycle(1), leaf(1, "young.copy_leaf", 9, "young.copy_leaf>young.ref_fix"),
        )))["cycles"][0]
        self.assertEqual(row["pillars_ns"]["ref_fix"], 9)
        self.assertEqual(row["pillars_ns"]["copy"], 0)

    def test_nonpillar_leaf_never_enters_inequality_1(self) -> None:
        # Historical test identifier retained: nonpillar leaves still never enter the five-pillar
        # subtotal, but the full structural-leaf bound now deliberately includes them.
        result = phase_leaf_ledger("\n".join((cycle(1, 1000), leaf(1, "Finalizer", 999))))
        self.assertEqual(result["cycles"][0]["leaf_pillar_ns"], 0)
        self.assertEqual(result["cycles"][0]["structural_leaf_ns"], 999)
        self.assertEqual(result["excluded_nonpillar_leaf_records"], 1)

    def test_overfull_same_cycle_five_pillar_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, r"inequality_1.*seq=8 sum_ns=11 dur_ns=10"):
            phase_leaf_ledger("\n".join((
                cycle(8, 10), leaf(8, "young.copy", 6), leaf(8, "young.mark", 5),
            )))

    def test_nonpillar_positive_seq_leaf_enters_inequality_1(self) -> None:
        with self.assertRaisesRegex(ValueError, r"inequality_1.*seq=1 sum_ns=101 dur_ns=100"):
            phase_leaf_ledger("\n".join((cycle(1, 100), leaf(1, "young.flush_alloc", 101))))

    def test_gc_external_unowned_and_internal_nonpillar_owned_in_same_log(self) -> None:
        text = "\n".join((
            cycle(1, 100),
            leaf(0, "contract.external", 999),
            leaf(1, "young.flush_alloc", 99),
        ))
        result = phase_leaf_ledger(text)
        self.assertEqual(result["unowned_nonpillar_names"], ["contract.external"])
        self.assertEqual(result["cycles"][0]["structural_leaf_ns"], 99)
        with self.assertRaisesRegex(ValueError, r"inequality_1.*sum_ns=101 dur_ns=100"):
            phase_leaf_ledger(text.replace("name=young.flush_alloc ns=99", "name=young.flush_alloc ns=101"))

    @staticmethod
    def inequality_fixture(*, pillar_ns: int = 20, nonpillar_ns: int = 10,
                           zpause_ns: int = 30, cycle_ns: int = 100,
                           leaf_kind: str = "pause") -> str:
        return "\n".join((
            cycle(1, cycle_ns),
            leaf(1, "young.copy", pillar_ns, kind=leaf_kind),
            leaf(1, "young.flush_alloc", nonpillar_ns, kind=leaf_kind),
            "[GCLOG] v=3 rec=stw seq=1 reason=young wait_ns=1 held_ns=40",
            f"[ZSTAT] v=1 rec=zphase seq=1 name=young.copy pause_ns={zpause_ns} conc_ns=5 n=1",
            f"[ZSTAT] v=1 rec=zcycle seq=1 pause_ns={zpause_ns} conc_ns=5 "
            f"max_pause_ns={zpause_ns} phases=1",
        ))

    def run_inequality_checker(self, text: str, wall_ns: int = 120) -> subprocess.CompletedProcess[str]:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8") as stream:
            stream.write(text)
            stream.flush()
            return subprocess.run(
                [sys.executable, str(ROOT / "tools/zstat_pillars.py"), stream.name,
                 "--wall-ns", str(wall_ns)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
            )

    def test_three_inequality_checker_normal_is_green(self) -> None:
        result = self.run_inequality_checker(self.inequality_fixture())
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("INEQUALITY_2 zphase_pause_ns=30 held_ns=40 verdict=PASS", result.stdout)
        self.assertIn("INEQUALITY_3 cycle_ns=100 wall_ns=120 ratio=", result.stdout)
        self.assertNotIn("verdict=FAIL", result.stdout)

    def test_five_pillar_overfull_only_fails_inequality_1(self) -> None:
        result = self.run_inequality_checker(self.inequality_fixture(pillar_ns=101))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("inequality_1 structural leaf sum exceeds cycle", result.stdout)
        self.assertNotIn("INEQUALITY_2", result.stdout)
        self.assertNotIn("INEQUALITY_3", result.stdout)

    def test_nonpillar_overfull_only_fails_inequality_1(self) -> None:
        result = self.run_inequality_checker(self.inequality_fixture(nonpillar_ns=101))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("inequality_1 structural leaf sum exceeds cycle", result.stdout)
        self.assertNotIn("INEQUALITY_2", result.stdout)
        self.assertNotIn("INEQUALITY_3", result.stdout)

    def test_zphase_held_extreme_mismatch_only_fails_inequality_2(self) -> None:
        result = self.run_inequality_checker(self.inequality_fixture(zpause_ns=100))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("INEQUALITY_2 zphase_pause_ns=100 held_ns=40 verdict=FAIL", result.stdout)
        self.assertIn("INEQUALITY_3 cycle_ns=100 wall_ns=120 ratio=", result.stdout)
        self.assertEqual(result.stdout.count("verdict=FAIL"), 1)

    def test_cycle_wall_overflow_only_fails_inequality_3(self) -> None:
        result = self.run_inequality_checker(self.inequality_fixture(cycle_ns=200))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("INEQUALITY_2 zphase_pause_ns=30 held_ns=40 verdict=PASS", result.stdout)
        self.assertIn("INEQUALITY_3 cycle_ns=200 wall_ns=120 ratio=", result.stdout)
        self.assertEqual(result.stdout.count("verdict=FAIL"), 1)

    def test_unknown_kind_classification_fails_closed_without_pause_share(self) -> None:
        result = self.run_inequality_checker(self.inequality_fixture(leaf_kind="unknown"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("PHASE_KIND_CLASSIFICATION_UNAVAILABLE", result.stdout)
        self.assertNotIn("pause-share%", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
