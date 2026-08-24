#!/usr/bin/env python3
"""Summarize the two young STW bodies from structured GC logs.

The input is the ``measure`` directory produced by the youngstw/hapillar
production-shaped runner.  Only ``reasons/`` is timing authority.  Timers
which nest another timer are deliberately excluded from the additive ledger.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import sys
from pathlib import Path
from statistics import median

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gclog_schema import parse_gclog

STAMP_RE = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+) \d+ (.*)$")
TIMER_RE = re.compile(r"^(.+) time: ([\d,]+)us$")

COLLECTION_PHASES = (
    "young.flush_alloc",
    "young.prepare_candidates",
    "young.remset_drain",
    "young.root_enum",
    "young.mark_closure",
    "young.remset_rescan",
    "young.mark_from_remset",
    "young.pre_evac_clear",
    "young.ref_fix_prepare",
    "young.ref_fix_pregrant",
    "young.ref_fix_root_pass1",
)


def percentile(values: list[float], q: float) -> float:
    """Linear interpolation, matching the campaign's hapillar convention."""
    ordered = sorted(values)
    if not ordered:
        raise ValueError("empty percentile input")
    at = (len(ordered) - 1) * q
    lo = int(at)
    hi = min(lo + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (at - lo)


def one_file(directory: Path, prefix: str) -> Path:
    matches = list(directory.glob(prefix + "*"))
    if len(matches) != 1:
        raise ValueError(f"{directory}: expected one {prefix}*, got {len(matches)}")
    return matches[0]


def numbers(pattern: str, text: str, names: tuple[str, ...]) -> dict[str, int]:
    match = re.search(pattern, text)
    if match is None:
        raise ValueError(f"missing counter pattern: {pattern}")
    return {name: int(match.group(name)) for name in names}


def timed_report_lines(text: str) -> list[tuple[dt.datetime, str]]:
    result: list[tuple[dt.datetime, str]] = []
    for line in text.splitlines():
        match = STAMP_RE.match(line)
        if match is None:
            continue
        result.append((dt.datetime.fromisoformat(match.group(1)), match.group(2)))
    return result


def load_run(directory: Path) -> dict[str, object]:
    if (directory / "classification").read_text().strip() != "COMPLETE":
        raise ValueError(f"{directory}: not COMPLETE")

    stderr = (directory / "stderr").read_text(errors="replace")
    report = one_file(directory, "report.log.").read_text(errors="replace")
    runtime = one_file(directory, "runtime.log.").read_text(errors="replace")
    records = parse_gclog(stderr)
    phases = {record.name: record.ns / 1000.0 for record in records.phases if record.seq == 1}
    stw = {record.reason: record.held_ns for record in records.stw if record.seq == 1}
    if len(records.stw) != 4:
        raise ValueError(f"{directory}: expected four STW records")
    if not all(name in phases for name in COLLECTION_PHASES):
        missing = sorted(set(COLLECTION_PHASES) - phases.keys())
        raise ValueError(f"{directory}: missing phases {missing}")

    report_lines = timed_report_lines(report)
    timer_end: dict[str, tuple[dt.datetime, int]] = {}
    for timestamp, body in report_lines:
        match = TIMER_RE.match(body)
        if match is not None:
            timer_end[match.group(1)] = (timestamp, int(match.group(2).replace(",", "")))
    mark_end, _ = timer_end["young.mark_from_remset"]
    pre_clear_end, pre_clear_us = timer_end["young.pre_evac_clear"]
    postmark_gap_us = (pre_clear_end - mark_end).total_seconds() * 1e6 - pre_clear_us

    counters: dict[str, int] = {}
    counters.update(numbers(
        r"\[GCV2Minor\] run=1 .*candidates=(?P<candidates>\d+) "
        r"candidateBytes=(?P<candidate_bytes>\d+) liveBytes=(?P<live_bytes>\d+) "
        r"remembered=(?P<remembered>\d+)",
        report, ("candidates", "candidate_bytes", "live_bytes", "remembered")))
    counters.update(numbers(
        r"\[GCV2\]\[markpar\].*reachable_n=(?P<reachable>\d+)",
        report, ("reachable",)))
    counters.update(numbers(
        r"\[GCV2Minor\] y2yDirtyHolders=(?P<y2y_dirty_holders>\d+)",
        report, ("y2y_dirty_holders",)))
    counters.update(numbers(
        r"\[GCV2\]\[remsetdrain\].*recorded=(?P<recorded>\d+) "
        r"live=(?P<remset_live>\d+) consumed=(?P<consumed>\d+).*interiors=(?P<interiors>\d+)",
        report, ("recorded", "remset_live", "consumed", "interiors")))
    counters.update(numbers(
        r"\[GCV2\]\[reffix\]\[conc_heap\].*nObj=(?P<n_obj>\d+) "
        r"nSlot=(?P<n_slot>\d+).*cas_ok=(?P<cas_ok>\d+) cas_fail=(?P<cas_fail>\d+)",
        report, ("n_obj", "n_slot", "cas_ok", "cas_fail")))
    counters.update(numbers(
        r"remembered-set promoteReplay=(?P<promote_replay>\d+) "
        r"residualPromote=(?P<residual_promote>\d+) youngRegionCount=(?P<young_after>\d+)",
        report, ("promote_replay", "residual_promote", "young_after")))
    counters.update(numbers(
        r"\[PROMODOMAIN\]\[DISCHARGE\] edges=(?P<domain_edges>\d+) .*"
        r"ns=(?P<domain_ns>\d+) registered=(?P<domain_regions>\d+) "
        r"tableBytes≈(?P<domain_table_bytes>\d+)",
        report, ("domain_edges", "domain_ns", "domain_regions", "domain_table_bytes")))
    counters.update(numbers(
        r"\[GCV2\]\[installdomain\] pregrant grant=(?P<grant>\d+) "
        r"already=(?P<already>\d+) tooLate=(?P<too_late>\d+) skip=(?P<skip>\d+)",
        runtime, ("grant", "already", "too_late", "skip")))
    counters.update(numbers(
        r"\[GCV2\]\[youngstatic\] pregrant_static young=(?P<static_young>\d+) "
        r"ensureCalls=(?P<static_ensure>\d+) missAfter=(?P<static_miss>\d+)",
        runtime, ("static_young", "static_ensure", "static_miss")))

    tail_start = timer_end["young.ref_fix_tail"][0]
    tail_end = timer_end["young.evac_prepare_next"][0]
    counters["evac_ghost_regions"] = sum(
        tail_start < timestamp <= tail_end and "[GCV2][ghost-dispel]" in body
        for timestamp, body in report_lines)

    collection_known_us = sum(phases[name] for name in COLLECTION_PHASES)
    collection_held_us = stw["young_collection"] / 1000.0
    collection_boundary_us = collection_held_us - collection_known_us - postmark_gap_us
    evac_tail_us = phases["young.evac_finish"] - phases["young.evac_prepare_next"]
    evac_other_us = evac_tail_us - counters["domain_ns"] / 1000.0
    post_held_us = stw["young_post-relocate"] / 1000.0
    post_boundary_us = post_held_us - (
        phases["young.ref_fix_bulk"] + phases["young.ref_fix_tail"] + phases["young.evac_finish"])

    return {
        "run": directory.name,
        "phases_us": phases,
        "stw_ns": stw,
        "postmark_gap_us": postmark_gap_us,
        "collection_boundary_us": collection_boundary_us,
        "evac_tail_us": evac_tail_us,
        "evac_other_us": evac_other_us,
        "post_boundary_us": post_boundary_us,
        "counters": counters,
    }


def stats(values: list[float]) -> dict[str, float]:
    return {"median": median(values), "p90": percentile(values, 0.9), "sum": sum(values)}


def summarize(root: Path) -> dict[str, object]:
    runs = [load_run(path) for path in sorted((root / "reasons").glob("r*-subject"))]
    if len(runs) < 10:
        raise ValueError(f"need N>=10, got {len(runs)}")

    collection_rows: list[tuple[str, list[float]]] = []
    for name in COLLECTION_PHASES[:7]:
        collection_rows.append((name, [run["phases_us"][name] for run in runs]))
    collection_rows.append(("derived.postmark_fixpoint_gap", [run["postmark_gap_us"] for run in runs]))
    for name in COLLECTION_PHASES[7:]:
        collection_rows.append((name, [run["phases_us"][name] for run in runs]))
    collection_rows.append(("derived.boundary_glue", [run["collection_boundary_us"] for run in runs]))

    post_rows = [
        ("young.ref_fix_bulk", [run["phases_us"]["young.ref_fix_bulk"] for run in runs]),
        ("young.ref_fix_tail", [run["phases_us"]["young.ref_fix_tail"] for run in runs]),
        ("counter.promodomain_discharge", [run["counters"]["domain_ns"] / 1000.0 for run in runs]),
        ("derived.evac_finish_other", [run["evac_other_us"] for run in runs]),
        ("young.evac_prepare_next", [run["phases_us"]["young.evac_prepare_next"] for run in runs]),
        ("derived.boundary_glue", [run["post_boundary_us"] for run in runs]),
    ]

    collection_total_us = sum(run["stw_ns"]["young_collection"] for run in runs) / 1000.0
    post_total_us = sum(run["stw_ns"]["young_post-relocate"] for run in runs) / 1000.0

    def rows_json(rows: list[tuple[str, list[float]]], total_us: float) -> list[dict[str, object]]:
        output = []
        for name, values in rows:
            row = {"name": name, "n": len(values), **stats(values)}
            row["pooled_share_pct"] = row["sum"] * 100.0 / total_us
            output.append(row)
        return output

    counter_names = sorted(runs[0]["counters"].keys())
    counter_stats = {
        name: stats([run["counters"][name] for run in runs])
        for name in counter_names
    }
    held = {
        reason: stats([run["stw_ns"][reason] / 1000.0 for run in runs])
        for reason in ("young_collection", "young_post-relocate")
    }
    return {
        "root": str(root),
        "n": len(runs),
        "held_us": held,
        "young_collection": rows_json(collection_rows, collection_total_us),
        "young_post_relocate": rows_json(post_rows, post_total_us),
        "counters": counter_stats,
        "runs": runs,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--compact", action="store_true")
    args = parser.parse_args()
    print(json.dumps(summarize(args.root), ensure_ascii=False,
                     indent=None if args.compact else 2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
