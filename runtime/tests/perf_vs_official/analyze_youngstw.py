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


def report_timer_ends(
    report_lines: list[tuple[dt.datetime, str]],
) -> dict[str, tuple[dt.datetime, int]]:
    result: dict[str, tuple[dt.datetime, int]] = {}
    for timestamp, body in report_lines:
        match = TIMER_RE.match(body)
        if match is not None:
            result[match.group(1)] = (timestamp, int(match.group(2).replace(",", "")))
    return result


def evac_ghost_regions(
    report_lines: list[tuple[dt.datetime, str]],
    timer_end: dict[str, tuple[dt.datetime, int]],
) -> int:
    """Count current-cycle ghost retirement during the evacuation tail.

    ``ExpireKeptFromPreviousCycle`` runs after ``young.ref_fix_bulk`` closes and
    before ``young.evac_finish`` starts.  Deriving the latter's start from its
    end timestamp and duration keeps those previous-cycle retirements out while
    retaining the current evacuation tail through ``young.evac_prepare_next``.
    """
    evac_finish_end, evac_finish_us = timer_end["young.evac_finish"]
    tail_start = evac_finish_end - dt.timedelta(microseconds=evac_finish_us)
    tail_end = timer_end["young.evac_prepare_next"][0]
    if tail_end < tail_start:
        raise ValueError("young.evac_prepare_next ends before young.evac_finish starts")
    return sum(
        tail_start < timestamp <= tail_end and "[GCV2][ghost-dispel]" in body
        for timestamp, body in report_lines)


def validate_run(
    directory: Path,
    phases: dict[str, float],
    stw: dict[str, int],
    timer_end: dict[str, tuple[dt.datetime, int]],
    report_lines: list[tuple[dt.datetime, str]],
    counters: dict[str, int],
    postmark_gap_us: float,
) -> None:
    """Reject a structurally plausible log whose derived ledger is impossible.

    These are semantic contracts of the r4 boundary change, rather than fixed
    campaign values: the independently reported evacuation-tail population
    agrees with its timestamped records, every derived interval has
    non-negative elapsed time, and counters which partition a visited
    population cannot exceed it.
    """
    required_timers = (
        "young.mark_from_remset",
        "young.pre_evac_clear",
        "young.ref_fix_bulk",
        "young.evac_finish",
        "young.evac_prepare_next",
    )
    missing_timers = [name for name in required_timers if name not in timer_end]
    if missing_timers:
        raise ValueError(f"{directory}: missing timer ledger entries {missing_timers}")

    evac_finish_end, evac_finish_us = timer_end["young.evac_finish"]
    tail_start = evac_finish_end - dt.timedelta(microseconds=evac_finish_us)
    bulk_end = timer_end["young.ref_fix_bulk"][0]
    tail_end = timer_end["young.evac_prepare_next"][0]
    if bulk_end > tail_start:
        raise ValueError(
            f"{directory}: boundary order violated: bulk_end={bulk_end.isoformat()} "
            f"tail_start={tail_start.isoformat()}"
        )
    if tail_start > tail_end:
        raise ValueError(
            f"{directory}: evacuation tail ends before it starts: "
            f"start={tail_start.isoformat()} end={tail_end.isoformat()}"
        )

    measured_ghost_regions = evac_ghost_regions(report_lines, timer_end)
    if counters["candidates"] != measured_ghost_regions:
        raise ValueError(
            f"{directory}: invariant evac_ghost_regions == GCV2Minor candidates "
            f"violated: counter={counters['candidates']} "
            f"measured={measured_ghost_regions}"
        )
    counters["evac_ghost_regions"] = measured_ghost_regions

    collection_boundary_us = (
        stw["young_collection"] / 1000.0
        - sum(phases[name] for name in COLLECTION_PHASES)
        - postmark_gap_us
    )
    evac_tail_us = phases["young.evac_finish"] - phases["young.evac_prepare_next"]
    evac_other_us = evac_tail_us - counters["domain_ns"] / 1000.0
    post_boundary_us = stw["young_post-relocate"] / 1000.0 - (
        phases["young.ref_fix_bulk"] + phases["young.evac_finish"])
    derived = {
        "postmark_gap_us": postmark_gap_us,
        "collection_boundary_us": collection_boundary_us,
        "evac_tail_us": evac_tail_us,
        "evac_other_us": evac_other_us,
        "post_boundary_us": post_boundary_us,
    }
    negative = {name: value for name, value in derived.items() if value < 0}
    if negative:
        raise ValueError(f"{directory}: negative derived ledger values {negative}")

    bounded = (
        ("remset_live", "recorded"),
        ("consumed", "recorded"),
        ("interiors", "recorded"),
        ("static_miss", "static_ensure"),
    )
    for child, parent in bounded:
        if counters[child] > counters[parent]:
            raise ValueError(
                f"{directory}: counter invariant {child} <= {parent} violated: "
                f"{counters[child]} > {counters[parent]}"
            )
    if counters["cas_ok"] + counters["cas_fail"] > counters["n_slot"]:
        raise ValueError(
            f"{directory}: counter invariant cas_ok + cas_fail <= n_slot violated: "
            f"{counters['cas_ok']} + {counters['cas_fail']} > {counters['n_slot']}"
        )


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
    timer_end = report_timer_ends(report_lines)
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

    collection_known_us = sum(phases[name] for name in COLLECTION_PHASES)
    collection_held_us = stw["young_collection"] / 1000.0
    collection_boundary_us = collection_held_us - collection_known_us - postmark_gap_us
    evac_tail_us = phases["young.evac_finish"] - phases["young.evac_prepare_next"]
    evac_other_us = evac_tail_us - counters["domain_ns"] / 1000.0
    post_held_us = stw["young_post-relocate"] / 1000.0
    post_boundary_us = post_held_us - (
        phases["young.ref_fix_bulk"] + phases["young.evac_finish"])

    validate_run(
        directory, phases, stw, timer_end, report_lines, counters, postmark_gap_us)

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
    try:
        result = summarize(args.root)
    except (OSError, KeyError, ValueError) as exc:
        print(f"ANALYZER_REJECT: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=False,
                     indent=None if args.compact else 2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
