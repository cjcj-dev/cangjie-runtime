#!/usr/bin/env python3
"""Validate current GC lifecycle, pause-wall and phase-window records; report inclusive work."""
from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "runtime/tests/perf_vs_official"))
from gclog_schema import PILLARS, parse_gclog, pillar_for


def union_measure(intervals):
    merged = []
    for start, end in sorted((start, end) for start, end in intervals if end > start):
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return sum(end - start for start, end in merged)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--wall-ns", type=int, required=True)
    args = parser.parse_args()
    if args.wall_ns <= 0:
        print("RED: --wall-ns must be a positive measured process interval")
        return 1
    try:
        records = parse_gclog(args.log.read_text(encoding="utf-8", errors="replace")).validate_complete()
    except ValueError as exc:
        print(f"RED: structured ledger invalid: {exc}")
        return 1
    cycles = {r.seq: r for r in records.cycles if r.event == "end"}
    print(f"LIFECYCLE_COMPLETE completed={len(cycles)} aborted={records.aborted} truncated={records.truncated}")
    if not cycles:
        print("RED: no completed collections; duration and phase shares unavailable")
        return 1
    phases = [r for r in records.phases if r.seq in cycles and r.kind != "critical"]
    if not phases or sum(r.ns for r in phases) == 0:
        print("RED: no positive phase work in completed collections")
        return 1
    by_kind = defaultdict(int)
    by_pillar = defaultdict(int)
    for record in phases:
        by_kind[record.kind] += record.ns
        key = pillar_for(record.name)
        if key is not None:
            by_pillar[key] += record.ns
    for kind, ns in sorted(by_kind.items()):
        print(f"WORK_KIND kind={kind} inclusive_ns={ns}")
    for pillar, _ in PILLARS:
        # A pillar without a matching producer is unavailable, not a measured zero.
        print(f"WORK_PILLAR name={pillar} inclusive_ns={by_pillar.get(pillar, 'unavailable')}")

    windows = defaultdict(list)
    for record in records.stw:
        if record.seq in cycles:
            windows[(record.seq, record.gc_tag)].append(
                (record.start_ns, record.start_ns + record.wait_ns + record.held_ns))
    pause_mismatches = []
    pauses = [r for r in phases if r.kind == "pause"]
    for record in pauses:
        if not any(start <= record.start_ns and record.start_ns + record.ns <= end
                   for start, end in windows[(record.seq, record.gc_tag)]):
            pause_mismatches.append(record)
            print(f"PAUSE_PHASE_CONTAINMENT seq={record.seq} generation={record.gc_tag} name={record.name} verdict=FAIL")
    pause_ok = bool(pauses) and not pause_mismatches
    print(f"CONTRACT_2_PAUSE_WALL verdict={'PASS' if pause_ok else 'FAIL'} samples={len(pauses)} mismatches={len(pause_mismatches)}")

    bound_mismatches = []
    for record in phases:
        cycle = cycles[record.seq]
        if not cycle.start_ns <= record.start_ns <= record.start_ns + record.ns <= cycle.start_ns + cycle.dur_ns:
            bound_mismatches.append(record)
            print(f"PHASE_CYCLE_BOUND seq={record.seq} name={record.name} verdict=FAIL")
    bounds_ok = not bound_mismatches
    print(f"CONTRACT_3_PHASE_CYCLE verdict={'PASS' if bounds_ok else 'FAIL'} samples={len(phases)} mismatches={len(bound_mismatches)}")
    # The two ZGC drivers can overlap (zDriver.cpp:201-225,463-488). Wall
    # coverage is the union, not the sum of their independent durations.
    cycle_ns = union_measure([(r.start_ns, r.start_ns + r.dur_ns) for r in cycles.values()])
    wall_ok = cycle_ns <= args.wall_ns
    print(f"COLLECTION_WALL union_ns={cycle_ns} wall_ns={args.wall_ns} verdict={'PASS' if wall_ok else 'FAIL'}")
    return 0 if pause_ok and bounds_ok and wall_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
