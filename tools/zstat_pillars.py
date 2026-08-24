#!/usr/bin/env python3
"""Enforce structural-leaf, ZStat/STW, and cycle/wall ledger inequalities."""

from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime/tests/perf_vs_official"))

from gclog_schema import PILLARS, build_phase_leaf_ledger, parse_gclog, parse_zstat, pillar_for  # noqa: E402


REFERENCE = {"mark": 65.0, "drain": 19.0, "copy": 6.3, "evac_finish": 5.2, "ref_fix": 4.5}
EXPLANATION = (
    "ZStat pause is an inclusive Timer sample sum while STW held is a wall-clock interval; "
    "parent/child duplication, overlapping worker samples, and held regions without Timer coverage "
    "mean they are not an identity.  The fail-closed ledger contract only requires the aggregate "
    "inclusive pause sum not to exceed aggregate held time."
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--wall-ns", type=int, required=True)
    parser.add_argument("--legacy-tolerance", type=float)
    args = parser.parse_args()
    if args.wall_ns <= 0:
        print("RED: --wall-ns must be a positive measured process interval")
        return 1

    text = args.log.read_text(encoding="utf-8", errors="replace")
    try:
        gclog = parse_gclog(text)
        zstat = parse_zstat(text)
        ledger = build_phase_leaf_ledger(gclog)
    except ValueError as exc:
        print(f"RED: structured ledger invalid: {exc}")
        return 1

    owned_leaves = [record for record in gclog.phase_leaves if record.seq > 0]
    unknown_kinds = [record for record in owned_leaves if record.kind == "unknown"]
    if unknown_kinds:
        print(
            "PHASE_KIND_CLASSIFICATION_UNAVAILABLE "
            f"unknown={len(unknown_kinds)} owned_structural_leaves={len(owned_leaves)}; "
            "this MRT_ZSTAT=OFF-compatible ledger does not provide pause/conc classification"
        )
        return 1

    zcycles = {}
    for record in zstat.cycles:
        if record.seq in zcycles:
            print(f"RED: duplicate ZSTAT cycle seq={record.seq}")
            return 1
        zcycles[record.seq] = record
    if not zcycles:
        print("RED: no ZSTAT cycles (constant zero is a dead device, not no GC)")
        return 1
    for record in zstat.phases:
        if record.seq not in zcycles:
            print(f"RED: positive ZSTAT phase orphan seq={record.seq} name={record.name}")
            return 1

    all_phase_ns = sum(record.pause_ns + record.conc_ns for record in zstat.phases)
    if all_phase_ns == 0:
        print("RED: all ZSTAT phase totals are zero (dead-device guard)")
        return 1

    leaf_pillar_ns = defaultdict(int)
    leaf_pause_ns = defaultdict(int)
    for record in gclog.phase_leaves:
        key = pillar_for(record.path)
        if key is None or record.seq == 0:
            continue
        leaf_pillar_ns[key] += record.ns
        if record.kind == "pause":
            leaf_pause_ns[key] += record.ns

    print("INEQUALITY_1 all positive-seq structural leaves joined to the cycle master table")
    for row in ledger["cycles"]:
        pillars = " ".join(f"{name}_ns={row['pillars_ns'][name]}" for name, _pattern in PILLARS)
        print(
            f"CYCLE_BOUND seq={row['seq']} {pillars} leaf_pillar_ns={row['leaf_pillar_ns']} "
            f"structural_leaf_ns={row['structural_leaf_ns']} "
            f"cycle_dur_ns={row['cycle_dur_ns']} verdict=PASS"
        )
    print(
        "LEAF_DIAGNOSTIC "
        f"matched={ledger['matched_leaf_records']} excluded_nonpillar={ledger['excluded_nonpillar_leaf_records']} "
        f"unowned_nonpillar={ledger['unowned_nonpillar_leaf_records']}"
    )

    held_by_seq = defaultdict(int)
    for record in gclog.stw:
        if record.seq > 0:
            held_by_seq[record.seq] += record.held_ns
    zphase_by_seq = defaultdict(list)
    for record in zstat.phases:
        zphase_by_seq[record.seq].append(record)

    total_zpause = 0
    total_held = 0
    for seq, zcycle in sorted(zcycles.items()):
        held = held_by_seq[seq]
        zpause = sum(record.pause_ns for record in zphase_by_seq[seq])
        difference = zpause - held
        total_zpause += zpause
        total_held += held
        top = sorted(
            zphase_by_seq[seq], key=lambda record: record.pause_ns + record.conc_ns, reverse=True
        )[:5]
        contributors = ",".join(
            f"{record.name}:{record.pause_ns + record.conc_ns}" for record in top
        ) or "none"
        structural = next(
            (row["leaf_pillar_ns"] for row in ledger["cycles"] if row["seq"] == seq), 0
        )
        print(
            f"ZSTAT_CYCLE seq={seq} zpause_ns={zpause} held_ns={held} "
            f"pause_minus_held_ns={difference} top_inclusive_phase_contributors={contributors} "
            f"structural_leaf_pillar_ns={structural}"
        )
    print(
        f"ZSTAT_TOTAL zpause_ns={total_zpause} held_ns={total_held} "
        f"pause_minus_held_ns={total_zpause - total_held}"
    )
    inequality_2_ok = total_zpause <= total_held
    print(
        f"INEQUALITY_2 zphase_pause_ns={total_zpause} held_ns={total_held} "
        f"verdict={'PASS' if inequality_2_ok else 'FAIL'}"
    )
    print("ZSTAT_HELD_EXPLANATION " + EXPLANATION)

    pillar_total = sum(leaf_pillar_ns.values())
    print(f"{'pillar':<12} {'self-norm%':>10} {'ref%':>6} {'delta':>7}  pause-share%")
    legacy_red = False
    for key, _pattern in PILLARS:
        pct = 100.0 * leaf_pillar_ns[key] / pillar_total if pillar_total else 0.0
        ref = REFERENCE[key]
        delta = pct - ref
        pause_share = 100.0 * leaf_pause_ns[key] / leaf_pillar_ns[key] if leaf_pillar_ns[key] else 0.0
        if args.legacy_tolerance is not None and abs(delta) > args.legacy_tolerance:
            legacy_red = True
        print(f"{key:<12} {pct:>10.1f} {ref:>6.1f} {delta:>+7.1f}  {pause_share:>8.1f}")

    cycle_ns = sum(record.dur_ns for record in gclog.cycles)
    inequality_3_ok = cycle_ns <= args.wall_ns
    print(
        f"INEQUALITY_3 cycle_ns={cycle_ns} wall_ns={args.wall_ns} "
        f"ratio={cycle_ns / args.wall_ns:.9f} "
        f"verdict={'PASS' if inequality_3_ok else 'FAIL'}"
    )
    return 1 if legacy_red or not inequality_2_ok or not inequality_3_ok else 0


if __name__ == "__main__":
    raise SystemExit(main())
