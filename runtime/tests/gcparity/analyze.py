#!/usr/bin/env python3
"""Summarize a gcparity run_matrix.sh result tree without third-party modules."""

from __future__ import annotations

import csv
import hashlib
import json
import math
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path


TIME_RE = re.compile(r"wall_s=([0-9.]+) maxrss_kb=([0-9]+) exit=([0-9]+)")
REASON_RE = re.compile(r"Begin GC log\. GCReason: ([^,]+),")
PAUSE_RE = re.compile(r"(?:stw time|light sync time) ([0-9]+) us")
MINOR_RE = re.compile(r"\[GCV2Minor\] run=")
RUN_RE = re.compile(r"r([0-9]+)-(young|minoroff)$")


def median(values: list[float | int]) -> float:
    return float(statistics.median(values)) if values else math.nan


def nearest_rank(values: list[int], percentile: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    rank = max(1, math.ceil(percentile * len(ordered)))
    return float(ordered[rank - 1])


def fmt(value: float) -> str:
    return "NA" if math.isnan(value) else f"{value:.6f}"


def read_glob(directory: Path, pattern: str) -> str:
    return "\n".join(
        path.read_text(errors="replace") for path in sorted(directory.glob(pattern)) if path.is_file()
    )


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_run(directory: Path, root: Path) -> dict[str, object]:
    relative = directory.relative_to(root)
    if len(relative.parts) != 4:
        raise ValueError(f"unexpected run path: {relative}")
    workload, heap, fys_name, run_name = relative.parts
    match = RUN_RE.fullmatch(run_name)
    if match is None or fys_name not in {"fys0", "fys1"}:
        raise ValueError(f"unexpected run name: {relative}")
    round_number = int(match.group(1))
    arm = match.group(2)
    rc = int((directory / "rc").read_text().strip())
    time_match = TIME_RE.fullmatch((directory / "time.txt").read_text().strip())
    if time_match is None:
        raise ValueError(f"bad time.txt: {relative}")
    wall = float(time_match.group(1))
    rss = int(time_match.group(2))
    time_exit = int(time_match.group(3))
    stdout = (directory / "stdout").read_text(errors="replace")
    expected_marker = "ALLOCATION_DENSE_OK" if workload == "allocation_dense" else "SURVIVAL_DENSE_OK"
    stdout_hash = sha256(directory / "stdout")
    report = read_glob(directory, "report.log*")
    runtime_log = read_glob(directory, "runtime.log*")
    reasons = REASON_RE.findall(report)
    pauses = [int(value) for value in PAUSE_RE.findall(runtime_log)]
    minor_log_count = len(MINOR_RE.findall(report))
    minor_cycles = sum(reason == "young" for reason in reasons)
    major_cycles = len(reasons) - minor_cycles
    return {
        "workload": workload,
        "heap": heap,
        "fys": int(fys_name[-1]),
        "round": round_number,
        "arm": arm,
        "rc": rc,
        "time_exit": time_exit,
        "correct": rc == 0 and time_exit == 0 and expected_marker in stdout,
        "stdout_sha256": stdout_hash,
        "wall_s": wall,
        "maxrss_kb": rss,
        "cycles": len(reasons),
        "minor_cycles": minor_cycles,
        "major_cycles": major_cycles,
        "minor_log_count": minor_log_count,
        "pause_count": len(pauses),
        "pause_p99_us_run": nearest_rank(pauses, 0.99),
        "pauses": pauses,
        "path": str(relative),
    }


def arm_stats(runs: list[dict[str, object]]) -> dict[str, object]:
    correct = [run for run in runs if run["correct"]]
    pauses = [pause for run in correct for pause in run["pauses"]]
    hashes = sorted({str(run["stdout_sha256"]) for run in correct})
    return {
        "attempts": len(runs),
        "ok": len(correct),
        "stdout_sha_count": len(hashes),
        "stdout_sha256": ",".join(hashes),
        "wall_median_s": median([float(run["wall_s"]) for run in correct]),
        "rss_median_kb": median([int(run["maxrss_kb"]) for run in correct]),
        "pause_events": len(pauses),
        "pause_p99_us": nearest_rank(pauses, 0.99),
        "cycles_median": median([int(run["cycles"]) for run in correct]),
        "cycles_min": min((int(run["cycles"]) for run in correct), default=-1),
        "cycles_max": max((int(run["cycles"]) for run in correct), default=-1),
        "minor_median": median([int(run["minor_cycles"]) for run in correct]),
        "major_median": median([int(run["major_cycles"]) for run in correct]),
        "minor_log_median": median([int(run["minor_log_count"]) for run in correct]),
    }


def ratio(numerator: float, denominator: float) -> float:
    if math.isnan(numerator) or math.isnan(denominator) or denominator == 0:
        return math.nan
    return numerator / denominator


def write_tsv(path: Path, rows: list[dict[str, object]], fields: list[str]) -> None:
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, dialect="excel-tab", extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} RUN_ROOT OUTPUT_DIR", file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    output.mkdir(parents=True, exist_ok=True)

    run_dirs = sorted(path.parent for path in root.glob("*/*/fys*/r*-*/rc"))
    runs = [parse_run(path, root) for path in run_dirs]
    raw_fields = [
        "workload", "heap", "fys", "round", "arm", "rc", "time_exit", "correct",
        "stdout_sha256", "wall_s", "maxrss_kb", "cycles", "minor_cycles", "major_cycles",
        "minor_log_count", "pause_count", "pause_p99_us_run", "path",
    ]
    write_tsv(output / "raw.tsv", runs, raw_fields)

    groups: dict[tuple[str, str, int, str], list[dict[str, object]]] = defaultdict(list)
    for run in runs:
        groups[(str(run["workload"]), str(run["heap"]), int(run["fys"]), str(run["arm"]))].append(run)

    arm_rows: list[dict[str, object]] = []
    stats_by_group: dict[tuple[str, str, int, str], dict[str, object]] = {}
    for key in sorted(groups):
        stats = arm_stats(groups[key])
        stats_by_group[key] = stats
        workload, heap, fys, arm = key
        arm_rows.append({"workload": workload, "heap": heap, "fys": fys, "arm": arm, **stats})
    arm_fields = ["workload", "heap", "fys", "arm"] + list(arm_rows[0].keys())[4:]
    write_tsv(output / "arms.tsv", arm_rows, arm_fields)

    cell_rows: list[dict[str, object]] = []
    matched_rows: list[dict[str, object]] = []
    max_excess = 0.0
    all_cells_pass = True
    for workload, heap, fys in sorted({key[:3] for key in groups}):
        young = stats_by_group[(workload, heap, fys, "young")]
        control = stats_by_group[(workload, heap, fys, "minoroff")]
        wall_ratio = ratio(float(young["wall_median_s"]), float(control["wall_median_s"]))
        pause_ratio = ratio(float(young["pause_p99_us"]), float(control["pause_p99_us"]))
        rss_ratio = ratio(float(young["rss_median_kb"]), float(control["rss_median_kb"]))
        same_output = (
            young["stdout_sha_count"] == 1
            and control["stdout_sha_count"] == 1
            and young["stdout_sha256"] == control["stdout_sha256"]
        )
        correct = young["attempts"] == young["ok"] == 20 and control["attempts"] == control["ok"] == 20
        strict_win = min(wall_ratio, pause_ratio) <= 0.95
        passed = correct and same_output and wall_ratio <= 1.0 and pause_ratio <= 1.0 and rss_ratio <= 1.0 and strict_win
        all_cells_pass = all_cells_pass and passed
        if not any(math.isnan(value) for value in (wall_ratio, pause_ratio, rss_ratio)):
            max_excess = max(max_excess, wall_ratio - 1.0, pause_ratio - 1.0, rss_ratio - 1.0)
        cell_rows.append({
            "workload": workload,
            "heap": heap,
            "fys": fys,
            "correct": correct,
            "same_output": same_output,
            "wall_ratio": fmt(wall_ratio),
            "pause_p99_ratio": fmt(pause_ratio),
            "rss_ratio": fmt(rss_ratio),
            "strict_win": strict_win,
            "pass": passed,
        })

        young_by_round = {int(run["round"]): run for run in groups[(workload, heap, fys, "young")] if run["correct"]}
        control_by_round = {
            int(run["round"]): run for run in groups[(workload, heap, fys, "minoroff")] if run["correct"]
        }
        matched = [
            (young_by_round[number], control_by_round[number])
            for number in sorted(young_by_round.keys() & control_by_round.keys())
            if young_by_round[number]["cycles"] == control_by_round[number]["cycles"]
        ]
        matched_young_pauses = [pause for pair in matched for pause in pair[0]["pauses"]]
        matched_control_pauses = [pause for pair in matched for pause in pair[1]["pauses"]]
        matched_rows.append({
            "workload": workload,
            "heap": heap,
            "fys": fys,
            "matched_pairs": len(matched),
            "wall_ratio_median_of_pairs": fmt(median([
                float(pair[0]["wall_s"]) / float(pair[1]["wall_s"]) for pair in matched
            ])),
            "pause_p99_ratio": fmt(ratio(
                nearest_rank(matched_young_pauses, 0.99), nearest_rank(matched_control_pauses, 0.99)
            )),
        })

    write_tsv(
        output / "cells.tsv", cell_rows,
        ["workload", "heap", "fys", "correct", "same_output", "wall_ratio", "pause_p99_ratio",
         "rss_ratio", "strict_win", "pass"],
    )
    write_tsv(
        output / "cycle_matched.tsv", matched_rows,
        ["workload", "heap", "fys", "matched_pairs", "wall_ratio_median_of_pairs", "pause_p99_ratio"],
    )
    result = {
        "run_count": len(runs),
        "all_rc_zero": all(int(run["rc"]) == 0 for run in runs),
        "all_correct": all(bool(run["correct"]) for run in runs),
        "all_cells_pass": all_cells_pass,
        "distance_to_nonregression_percent": max(0.0, max_excess) * 100.0,
        "cells": cell_rows,
        "cycle_matched": matched_rows,
    }
    (output / "summary.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
