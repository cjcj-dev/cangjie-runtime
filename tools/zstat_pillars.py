#!/usr/bin/env python3
"""Enforce structural-leaf, work-ledger, pause-wall, and cycle contracts."""

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
    "zphase.pause_ns is inclusive work: a sum of Timer samples, not reported_pause_wall_ns. "
    "Contract 1 checks exact GCLOG↔ZSTAT sample and detail↔master conservation; Contract 2 "
    "checks each pause phase against an independently emitted same-seq STW interval, then "
    "checks the aggregate interval unions."
)


def union_measure(intervals):
    """Return the measure of a finite union of half-open [start,end) intervals."""
    merged = []
    for start, end in sorted((start, end) for start, end in intervals if end > start):
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return sum(end - start for start, end in merged)


def intersection_intervals(left, right):
    result = []
    for left_start, left_end in left:
        for right_start, right_end in right:
            start = max(left_start, right_start)
            end = min(left_end, right_end)
            if end > start:
                result.append((start, end))
    return result


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
    duplicate_cycle = False
    for record in zstat.cycles:
        if record.seq in zcycles:
            duplicate_cycle = True
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

    # Contract 1: exact conservation of each positive-seq Timer sample and the zcycle rollup.
    gc_phase = defaultdict(int)
    for record in gclog.phases:
        if record.seq > 0:
            gc_phase[(record.seq, record.name)] += record.ns
    zs_phase = defaultdict(int)
    for record in zstat.phases:
        if record.seq > 0:
            zs_phase[(record.seq, record.name)] += record.pause_ns + record.conc_ns
    ownership_mismatch = []
    for key in sorted(set(gc_phase) | set(zs_phase)):
        if gc_phase[key] != zs_phase[key]:
            ownership_mismatch.append((key[0], key[1], gc_phase[key], zs_phase[key]))

    zphase_by_seq = defaultdict(list)
    for record in zstat.phases:
        zphase_by_seq[record.seq].append(record)
    master_mismatch = []
    for seq in sorted(set(zcycles) | set(zphase_by_seq)):
        master = zcycles.get(seq)
        details = zphase_by_seq[seq]
        pause = sum(record.pause_ns for record in details)
        conc = sum(record.conc_ns for record in details)
        if master is None or (pause, conc, len(details)) != (
            master.pause_ns, master.conc_ns, master.phases
        ):
            master_mismatch.append((
                seq, pause, conc, len(details),
                None if master is None else master.pause_ns,
                None if master is None else master.conc_ns,
                None if master is None else master.phases,
            ))
    contract_1_ok = bool(zcycles) and not duplicate_cycle and not ownership_mismatch and not master_mismatch
    print(
        f"WORK_LEDGER_DETAIL zcycles={len(zcycles)} duplicate_cycle={int(duplicate_cycle)} "
        f"ownership_mismatches={len(ownership_mismatch)} master_mismatches={len(master_mismatch)}"
    )
    for seq, name, gc_ns, zs_ns in ownership_mismatch:
        print(f"WORK_LEDGER_OWNERSHIP seq={seq} name={name} gclog_inclusive_work_ns={gc_ns} zstat_inclusive_work_ns={zs_ns}")
    for row in master_mismatch:
        print("WORK_LEDGER_MASTER " + " ".join(str(value) for value in row))
    print(f"CONTRACT_1_WORK_LEDGER verdict={'PASS' if contract_1_ok else 'FAIL'}")

    # Contract 2: every independently classified pause-phase sample must fit inside one
    # independently emitted STW interval with the same cycle sequence.
    phase_intervals = defaultdict(list)
    pause_phase_records = defaultdict(list)
    stw_intervals = defaultdict(list)
    unknown_phase_kinds = []
    for record in gclog.phases:
        if record.seq > 0 and record.kind == "pause":
            phase_intervals[record.seq].append((record.start_ns, record.start_ns + record.ns))
            pause_phase_records[record.seq].append(record)
        elif record.seq > 0 and record.kind == "unknown":
            unknown_phase_kinds.append(record)
    for record in gclog.stw:
        if record.seq > 0:
            end_ns = record.start_ns + record.wait_ns + record.held_ns
            stw_intervals[record.seq].append((record.start_ns, end_ns))
    phase_all = [interval for values in phase_intervals.values() for interval in values]
    stw_all = [interval for values in stw_intervals.values() for interval in values]
    containment_mismatches = []
    for seq, intervals in sorted(phase_intervals.items()):
        candidates = stw_intervals[seq]
        for sample, (phase_start, phase_end) in enumerate(intervals):
            if not any(stw_start <= phase_start and phase_end <= stw_end
                       for stw_start, stw_end in candidates):
                phase_record = pause_phase_records[seq][sample]
                containment_mismatches.append((
                    seq, sample, phase_record.name, phase_start, phase_end, tuple(candidates)))
    intersect_all = []
    for seq in sorted(set(phase_intervals) | set(stw_intervals)):
        intersect_all.extend(intersection_intervals(phase_intervals[seq], stw_intervals[seq]))
    phase_union_ns = union_measure(phase_all)
    intersection_union_ns = union_measure(intersect_all)
    stw_union_ns = union_measure(stw_all)
    aggregate_ok = phase_union_ns == intersection_union_ns and intersection_union_ns <= stw_union_ns
    contract_2_ok = (
        bool(phase_all) and bool(stw_all) and not unknown_phase_kinds and
        not containment_mismatches and aggregate_ok
    )
    print(
        f"PAUSE_WALL_DETAIL pause_phase_samples={len(phase_all)} "
        f"unknown_phase_kinds={len(unknown_phase_kinds)} "
        f"containment_mismatches={len(containment_mismatches)} "
        f"union_pause_phase_ns={phase_union_ns} "
        f"union_phase_intersection_stw_ns={intersection_union_ns} "
        f"union_stw_ns={stw_union_ns} aggregate_subset={int(aggregate_ok)}"
    )
    for record in unknown_phase_kinds:
        print(
            f"PAUSE_PHASE_KIND seq={record.seq} name={record.name} "
            f"start_ns={record.start_ns} end_ns={record.start_ns + record.ns} kind=unknown verdict=FAIL"
        )
    for seq, sample, name, phase_start, phase_end, candidates in containment_mismatches:
        windows = ",".join(f"[{start},{end})" for start, end in candidates) or "none"
        print(
            f"PAUSE_PHASE_CONTAINMENT seq={seq} sample={sample} name={name} "
            f"phase=[{phase_start},{phase_end}) stw_candidates={windows} verdict=FAIL"
        )
    print(f"CONTRACT_2_PAUSE_WALL verdict={'PASS' if contract_2_ok else 'FAIL'}")
    print("ZSTAT_INCLUSIVE_WORK_EXPLANATION " + EXPLANATION)

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
    return 1 if legacy_red or not contract_1_ok or not contract_2_ok or not inequality_3_ok else 0


if __name__ == "__main__":
    raise SystemExit(main())
