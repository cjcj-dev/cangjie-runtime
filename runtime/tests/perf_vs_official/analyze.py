#!/usr/bin/env python3
"""Analyze a perf-vs-official campaign without third-party packages."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import random
import re
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Iterable


TIME_RE = re.compile(r"wall_s=([0-9.]+)\tmaxrss_kb=([0-9]+)\ttime_exit=([0-9]+)")
PAUSE_RE = re.compile(r"(?:stw time|light sync time) ([0-9]+) us")
REASON_RE = re.compile(r"Begin GC log\. GCReason: ([^,]+),")
RUN_RE = re.compile(r"r([0-9]+)-(subject|official)")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_logs(directory: Path, pattern: str) -> str:
    return "\n".join(
        path.read_text(errors="replace") for path in sorted(directory.glob(pattern)) if path.is_file()
    )


def nearest_rank(values: Iterable[int], quantile: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan
    rank = max(1, math.ceil(quantile * len(ordered)))
    return float(ordered[rank - 1])


def percentile(values: list[float], quantile: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def median(values: Iterable[float | int]) -> float:
    materialized = list(values)
    return float(statistics.median(materialized)) if materialized else math.nan


def ratio(numerator: float, denominator: float) -> float:
    if math.isnan(numerator) or math.isnan(denominator) or denominator == 0:
        return math.nan
    return numerator / denominator


def fmt(value: float) -> str:
    return "NA" if math.isnan(value) else f"{value:.6f}"


def write_tsv(path: Path, rows: list[dict[str, object]], fields: list[str]) -> None:
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, dialect="excel-tab", extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def load_manifest(path: Path) -> dict[str, dict[str, str]]:
    with path.open(newline="") as stream:
        return {row["workload"]: row for row in csv.DictReader(stream, dialect="excel-tab")}


def parse_attempt(directory: Path, root: Path, manifest: dict[str, dict[str, str]]) -> dict[str, object]:
    relative = directory.relative_to(root / "runs")
    workload, heap, run_name = relative.parts
    match = RUN_RE.fullmatch(run_name)
    if match is None or workload not in manifest:
        raise ValueError(f"unexpected attempt path: {relative}")
    round_number = int(match.group(1))
    arm = match.group(2)
    rc = int((directory / "rc").read_text().strip())
    time_match = TIME_RE.search((directory / "time.tsv").read_text().strip())
    if time_match is None:
        raise ValueError(f"bad time record: {relative}")
    wall = float(time_match.group(1))
    rss = int(time_match.group(2))
    time_exit = int(time_match.group(3))
    stdout = (directory / "stdout").read_text(errors="replace")
    stdout_sha = sha256(directory / "stdout")
    runtime_log = read_logs(directory, "runtime.log*")
    report_log = read_logs(directory, "report.log*")
    pauses = [int(value) for value in PAUSE_RE.findall(runtime_log)]
    reasons = REASON_RE.findall(report_log)
    marker = manifest[workload]["marker"]
    correct = rc == 0 and time_exit == 0 and marker in stdout
    return {
        "workload": workload,
        "heap": heap,
        "round": round_number,
        "arm": arm,
        "rc": rc,
        "time_exit": time_exit,
        "correct": correct,
        "classification": (directory / "classification").read_text().strip(),
        "stdout_sha256": stdout_sha,
        "wall_s": wall,
        "throughput": float(manifest[workload]["work_units"]) / wall,
        "maxrss_kb": rss,
        "pause_events": len(pauses),
        "pauses": pauses,
        "cycles": len(reasons),
        "minor_cycles": sum(reason == "young" for reason in reasons),
        "major_cycles": sum(reason != "young" for reason in reasons),
        "path": str(relative),
    }


def aggregate(runs: list[dict[str, object]], metric: str) -> float:
    if metric == "wall":
        return median(float(run["wall_s"]) for run in runs)
    if metric == "throughput":
        return median(float(run["throughput"]) for run in runs)
    if metric == "peak_memory":
        return median(int(run["maxrss_kb"]) for run in runs)
    quantiles = {"pause_p50": 0.5, "pause_p99": 0.99, "pause_p999": 0.999}
    if metric in quantiles:
        return nearest_rank(
            (pause for run in runs for pause in run["pauses"]),
            quantiles[metric],
        )
    raise ValueError(metric)


def bootstrap_interval(
    pairs: list[tuple[dict[str, object], dict[str, object]]],
    metric: str,
    samples: int,
    seed: int,
) -> tuple[float, float]:
    if not pairs:
        return math.nan, math.nan
    generator = random.Random(seed)
    estimates: list[float] = []
    for _ in range(samples):
        chosen = [pairs[generator.randrange(len(pairs))] for _ in pairs]
        subject = [pair[0] for pair in chosen]
        official = [pair[1] for pair in chosen]
        estimate = ratio(aggregate(subject, metric), aggregate(official, metric))
        if not math.isnan(estimate):
            estimates.append(estimate)
    return percentile(estimates, 0.025), percentile(estimates, 0.975)


def metric_status(direction: str, low: float, high: float) -> str:
    if math.isnan(low) or math.isnan(high):
        return "INSUFFICIENT"
    if direction == "lower":
        if high < 1.0:
            return "SUPERIOR"
        if low > 1.0:
            return "INFERIOR"
    else:
        if low > 1.0:
            return "SUPERIOR"
        if high < 1.0:
            return "INFERIOR"
    return "INCONCLUSIVE"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("run_root", type=Path)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--min-pairs", type=int, default=5)
    parser.add_argument("--bootstrap-samples", type=int, default=10_000)
    parser.add_argument("--seed", type=int, default=20260821)
    args = parser.parse_args()

    root = args.run_root.resolve()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest = load_manifest(root / "manifest.tsv")
    directories = sorted(path.parent for path in (root / "runs").glob("*/*/r*-*/rc"))
    attempts = [parse_attempt(path, root, manifest) for path in directories]

    attempt_fields = [
        "workload", "heap", "round", "arm", "rc", "time_exit", "correct", "classification",
        "stdout_sha256", "wall_s", "throughput", "maxrss_kb", "pause_events", "cycles",
        "minor_cycles", "major_cycles", "path",
    ]
    write_tsv(output / "attempts.tsv", attempts, attempt_fields)

    groups: dict[tuple[str, str, str], list[dict[str, object]]] = defaultdict(list)
    for attempt in attempts:
        groups[(str(attempt["workload"]), str(attempt["heap"]), str(attempt["arm"]))].append(attempt)

    metrics_rows: list[dict[str, object]] = []
    matrix_rows: list[dict[str, object]] = []
    cell_rows: list[dict[str, object]] = []
    metric_specs = [
        ("wall", "lower", False, "s"),
        ("throughput", "higher", True, "work_units_per_s"),
        ("pause_p50", "lower", False, "us"),
        ("pause_p99", "lower", False, "us"),
        ("pause_p999", "lower", False, "us"),
        ("peak_memory", "lower", False, "KiB"),
    ]

    for workload, heap in sorted({key[:2] for key in groups}):
        subject_all = sorted(groups.get((workload, heap, "subject"), []), key=lambda row: row["round"])
        official_all = sorted(groups.get((workload, heap, "official"), []), key=lambda row: row["round"])
        subject_by_round = {int(run["round"]): run for run in subject_all}
        official_by_round = {int(run["round"]): run for run in official_all}
        common_rounds = sorted(subject_by_round.keys() & official_by_round.keys())
        all_correct = (
            bool(subject_all) and bool(official_all)
            and all(bool(run["correct"]) for run in subject_all + official_all)
        )
        hashes = {str(run["stdout_sha256"]) for run in subject_all + official_all if run["correct"]}
        same_output = len(hashes) == 1
        pairs = [
            (subject_by_round[number], official_by_round[number])
            for number in common_rounds
            if subject_by_round[number]["correct"] and official_by_round[number]["correct"]
        ]
        valid_cell = all_correct and same_output and len(pairs) >= args.min_pairs

        for arm, runs in (("subject", subject_all), ("official", official_all)):
            correct = [run for run in runs if run["correct"]]
            metrics_rows.append({
                "workload": workload,
                "heap": heap,
                "arm": arm,
                "attempts": len(runs),
                "correct": len(correct),
                "stdout_sha256": ",".join(sorted({str(run["stdout_sha256"]) for run in correct})),
                "wall_median_s": fmt(aggregate(correct, "wall")),
                "throughput_median": fmt(aggregate(correct, "throughput")),
                "pause_events": sum(int(run["pause_events"]) for run in correct),
                "pause_p50_us": fmt(aggregate(correct, "pause_p50")),
                "pause_p99_us": fmt(aggregate(correct, "pause_p99")),
                "pause_p999_us": fmt(aggregate(correct, "pause_p999")),
                "maxrss_median_kb": fmt(aggregate(correct, "peak_memory")),
                "cycles_median": fmt(median(int(run["cycles"]) for run in correct)),
            })

        statuses: dict[str, str] = {}
        for index, (metric, direction, derived, unit) in enumerate(metric_specs):
            subject_value = aggregate([pair[0] for pair in pairs], metric)
            official_value = aggregate([pair[1] for pair in pairs], metric)
            point_ratio = ratio(subject_value, official_value)
            cell_seed = args.seed + int(hashlib.sha256(f"{workload}/{heap}/{metric}".encode()).hexdigest()[:8], 16)
            low, high = bootstrap_interval(pairs, metric, args.bootstrap_samples, cell_seed)
            if not valid_cell:
                status = "INVALID" if not all_correct or not same_output else "INSUFFICIENT"
            else:
                status = metric_status(direction, low, high)
            statuses[metric] = status
            quantile = {"pause_p50": 0.5, "pause_p99": 0.99, "pause_p999": 0.999}.get(metric)
            subject_events = sum(int(pair[0]["pause_events"]) for pair in pairs)
            official_events = sum(int(pair[1]["pause_events"]) for pair in pairs)
            if quantile is None:
                tail_subject = tail_official = "NA"
                tail_resolution = "NA"
            else:
                tail_subject = str(math.ceil((1.0 - quantile) * subject_events))
                tail_official = str(math.ceil((1.0 - quantile) * official_events))
                tail_resolution = (
                    "LOW_TAIL_RESOLUTION"
                    if min(int(tail_subject), int(tail_official)) < 5
                    else "ADEQUATE"
                )
            matrix_rows.append({
                "workload": workload,
                "heap": heap,
                "metric": metric,
                "unit": unit,
                "direction": direction,
                "derived_from_wall": str(derived).lower(),
                "valid_pairs": len(pairs),
                "subject": fmt(subject_value),
                "official": fmt(official_value),
                "ratio_subject_official": fmt(point_ratio),
                "ci95_low": fmt(low),
                "ci95_high": fmt(high),
                "status": status,
                "subject_pause_events": subject_events if quantile is not None else "NA",
                "official_pause_events": official_events if quantile is not None else "NA",
                "tail_observations_subject": tail_subject,
                "tail_observations_official": tail_official,
                "tail_resolution": tail_resolution,
            })

        independent = ["wall", "pause_p50", "pause_p99", "pause_p999", "peak_memory"]
        if valid_cell and all(statuses[name] == "SUPERIOR" for name in independent):
            cell_status = "UNIFORMLY_SUPERIOR"
        elif valid_cell and all(statuses[name] == "INFERIOR" for name in independent):
            cell_status = "UNIFORMLY_INFERIOR"
        elif valid_cell:
            cell_status = "MIXED"
        else:
            cell_status = "INVALID" if not all_correct or not same_output else "INSUFFICIENT"
        cell_rows.append({
            "workload": workload,
            "heap": heap,
            "attempts_subject": len(subject_all),
            "attempts_official": len(official_all),
            "valid_pairs": len(pairs),
            "all_correct": all_correct,
            "same_output": same_output,
            "cell_status": cell_status,
        })

    metric_fields = [
        "workload", "heap", "arm", "attempts", "correct", "stdout_sha256", "wall_median_s",
        "throughput_median", "pause_events", "pause_p50_us", "pause_p99_us", "pause_p999_us",
        "maxrss_median_kb", "cycles_median",
    ]
    matrix_fields = [
        "workload", "heap", "metric", "unit", "direction", "derived_from_wall", "valid_pairs",
        "subject", "official", "ratio_subject_official", "ci95_low", "ci95_high", "status",
        "subject_pause_events", "official_pause_events", "tail_observations_subject",
        "tail_observations_official", "tail_resolution",
    ]
    cell_fields = [
        "workload", "heap", "attempts_subject", "attempts_official", "valid_pairs", "all_correct",
        "same_output", "cell_status",
    ]
    write_tsv(output / "metrics.tsv", metrics_rows, metric_fields)
    write_tsv(output / "matrix.tsv", matrix_rows, matrix_fields)
    write_tsv(output / "cells.tsv", cell_rows, cell_fields)

    summary = [
        "# GC performance versus official: matrix summary",
        "",
        f"Paired-cluster bootstrap: N={args.bootstrap_samples}, seed={args.seed}; minimum pairs={args.min_pairs}.",
        "",
        "| workload | heap | metric | subject | official | S/O | 95% CI | status | tail |",
        "|---|---:|---|---:|---:|---:|---|---|---|",
    ]
    for row in matrix_rows:
        summary.append(
            f"| {row['workload']} | {row['heap']} | {row['metric']} | {row['subject']} | "
            f"{row['official']} | {row['ratio_subject_official']} | "
            f"[{row['ci95_low']}, {row['ci95_high']}] | {row['status']} | {row['tail_resolution']} |"
        )
    summary.extend(["", "No weighted aggregate or single benchmark score is produced.", ""])
    (output / "summary.md").write_text("\n".join(summary))
    result = {
        "attempts": len(attempts),
        "bootstrap_samples": args.bootstrap_samples,
        "seed": args.seed,
        "min_pairs": args.min_pairs,
        "cells": cell_rows,
        "matrix": matrix_rows,
    }
    (output / "summary.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n")
    print(
        f"PERF_MATRIX attempts={len(attempts)} cells={len(cell_rows)} metrics={len(matrix_rows)} "
        f"uniform={sum(row['cell_status'] == 'UNIFORMLY_SUPERIOR' for row in cell_rows)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
