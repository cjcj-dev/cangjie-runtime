#!/usr/bin/env python3
"""Analyze the hello_alloc single-cycle pause and walk populations.

Expected layout below ROOT:
  reasons/rNN-subject/  production-shaped timing arm
  census/rNN-subject/   MRT_GCV2_HAPILLAR_CENSUS=1 population arm

GCLOG is read once from stderr.  REPORT lines are read once from report.log*.
"""
from __future__ import annotations

import json
import math
import re
import sys
from collections import defaultdict
from pathlib import Path


STW = re.compile(
    r"rec=stw\b.*\bseq=(\d+)\s+reason=(\S+)\s+wait_ns=(\d+)\s+held_ns=(\d+)"
)
PHASE = re.compile(r"rec=phase\b.*\bname=(\S+)\s+us=(\d+)")
REFFIX = re.compile(r"\[GCV2\]\[reffix\]\[conc_heap\].*\bnObj=(\d+)\s+nSlot=(\d+)")
CENSUS = re.compile(r"\[GCV2\]\[hapillar\]\[postflip\]\s+(.*)")
KV = re.compile(r"([A-Za-z][A-Za-z0-9]*)=(\d+)")


def pctile(vals: list[float] | list[int], q: float) -> float:
    if not vals:
        return math.nan
    ordered = sorted(vals)
    if len(ordered) == 1:
        return float(ordered[0])
    pos = (len(ordered) - 1) * q
    lo, hi = math.floor(pos), math.ceil(pos)
    if lo == hi:
        return float(ordered[lo])
    frac = pos - lo
    return ordered[lo] * (1.0 - frac) + ordered[hi] * frac


def report_text(run: Path) -> str:
    seen: set[Path] = set()
    parts: list[str] = []
    for path in sorted(run.glob("report.log*")):
        resolved = path.resolve()
        if not path.is_file() or resolved in seen:
            continue
        seen.add(resolved)
        parts.append(path.read_text(errors="replace"))
    return "\n".join(parts)


def complete_runs(root: Path, arm: str) -> list[Path]:
    out: list[Path] = []
    for run in sorted((root / arm).glob("r*-subject")):
        classification = run / "classification"
        if classification.is_file() and classification.read_text().strip() == "COMPLETE":
            out.append(run)
    return out


def analyze_reasons(runs: list[Path]) -> tuple[dict, dict]:
    per_reason: dict[str, list[int]] = defaultdict(list)
    rows: list[dict] = []
    ref_rows: list[dict] = []
    for run in runs:
        stderr = (run / "stderr").read_text(errors="replace")
        events = [(int(seq), reason, int(held)) for seq, reason, _, held in STW.findall(stderr)]
        by_reason: dict[str, int] = defaultdict(int)
        for _, reason, held in events:
            by_reason[reason] += held
        total = sum(by_reason.values())
        for reason, held in by_reason.items():
            per_reason[reason].append(held)
        phases: dict[str, int] = defaultdict(int)
        for name, us in PHASE.findall(stderr):
            phases[name] += int(us)
        report = report_text(run)
        match = REFFIX.search(report)
        if match is None:
            raise RuntimeError(f"missing ref-fix population: {run}")
        n_obj, n_slot = (int(match.group(1)), int(match.group(2)))
        entries = n_obj + n_slot
        # The historical five-pillar regex sums every matching phase, including
        # children nested inside young.ref_fix.  Keep that ledger, but also emit
        # non-overlapping elapsed and bulk-only ledgers.
        pillar_us = sum(us for name, us in phases.items() if "ref_fix" in name.lower())
        unique_us = sum(phases.get(name, 0) for name in (
            "young.ref_fix", "young.ref_fix_bulk", "young.ref_fix_tail"
        ))
        bulk_us = phases.get("young.ref_fix_bulk", 0)
        ref_rows.append({
            "run": run.name,
            "n_obj": n_obj,
            "n_slot": n_slot,
            "entries": entries,
            "pillar_us": pillar_us,
            "unique_us": unique_us,
            "bulk_us": bulk_us,
        })
        rows.append({
            "run": run.name,
            "stw_events": len(events),
            "stw_total_ns": total,
            "reasons_ns": dict(by_reason),
        })

    total_all = sum(r["stw_total_ns"] for r in rows)
    reason_summary = []
    for reason, vals in per_reason.items():
        reason_summary.append({
            "reason": reason,
            "n": len(vals),
            "median_ns": pctile(vals, 0.5),
            "p90_ns": pctile(vals, 0.9),
            "share_of_cycle_sum_pct": 100.0 * sum(vals) / total_all,
        })
    reason_summary.sort(key=lambda r: -r["median_ns"])

    entry_sum = sum(r["entries"] for r in ref_rows)
    ref_summary = {
        "n": len(ref_rows),
        "n_obj_median": pctile([r["n_obj"] for r in ref_rows], 0.5),
        "n_obj_p90": pctile([r["n_obj"] for r in ref_rows], 0.9),
        "n_slot_median": pctile([r["n_slot"] for r in ref_rows], 0.5),
        "entries_total": entry_sum,
        "entries_median": pctile([r["entries"] for r in ref_rows], 0.5),
        "entries_p90": pctile([r["entries"] for r in ref_rows], 0.9),
        "pillar_total_us": sum(r["pillar_us"] for r in ref_rows),
        "pillar_ns_per_entry": 1000.0 * sum(r["pillar_us"] for r in ref_rows) / entry_sum,
        "unique_total_us": sum(r["unique_us"] for r in ref_rows),
        "unique_ns_per_entry": 1000.0 * sum(r["unique_us"] for r in ref_rows) / entry_sum,
        "bulk_total_us": sum(r["bulk_us"] for r in ref_rows),
        "bulk_ns_per_entry": 1000.0 * sum(r["bulk_us"] for r in ref_rows) / entry_sum,
        "runs": ref_rows,
    }
    return {"n": len(rows), "reasons": reason_summary, "runs": rows}, ref_summary


def analyze_census(runs: list[Path]) -> dict:
    rows: list[dict] = []
    for run in runs:
        matches = CENSUS.findall(report_text(run))
        if len(matches) != 1:
            raise RuntimeError(f"expected one postflip census, got {len(matches)}: {run}")
        row = {key: int(value) for key, value in KV.findall(matches[0])}
        row["run"] = run.name
        rows.append(row)
    keys = sorted(set().union(*(set(r) for r in rows)) - {"run"})
    return {
        "n": len(rows),
        "median": {key: pctile([r[key] for r in rows], 0.5) for key in keys},
        "p90": {key: pctile([r[key] for r in rows], 0.9) for key in keys},
        "sum": {key: sum(r[key] for r in rows) for key in keys},
        "runs": rows,
    }


def main(root: Path) -> None:
    reason_runs = complete_runs(root, "reasons")
    census_runs = complete_runs(root, "census")
    if len(reason_runs) < 10 or len(census_runs) < 10:
        raise SystemExit(f"need N>=10 COMPLETE per arm: reasons={len(reason_runs)} census={len(census_runs)}")
    reasons, ref_fix = analyze_reasons(reason_runs)
    result = {
        "root": str(root),
        "reason_arm": reasons,
        "ref_fix": ref_fix,
        "postflip_census_arm": analyze_census(census_runs),
    }
    out = root / "analysis"
    out.mkdir(exist_ok=True)
    (out / "hapillar.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main(Path(sys.argv[1]))
