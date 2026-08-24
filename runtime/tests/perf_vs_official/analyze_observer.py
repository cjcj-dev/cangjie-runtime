#!/usr/bin/env python3
"""Analyze the paired subject fair/ledger attempts."""

from __future__ import annotations

import argparse
import csv
import math
import random
import re
import statistics
from collections import defaultdict
from pathlib import Path


TIME_RE = re.compile(r"wall_s=([0-9.]+)\tmaxrss_kb=([0-9]+)\ttime_exit=([0-9]+)")
RUN_RE = re.compile(r"r([0-9]+)-subject")
IDENTITY_KEYS = (
    "heap", "binary", "binary_sha256", "marker", "work_units", "work_unit_name",
    "runtime_lib", "runtime_sha256", "runtime_stamp", "boundscheck_sha256", "cores",
    "timeout_seconds",
)
FAIR_OBSERVER_ENV = {
    "env.MRT_LOG_LEVEL": "UNSET",
    "env.MRT_LOG_PATH": "UNSET",
    "env.MRT_REPORT": "UNSET",
    "env.MRT_GC_LOG": "UNSET",
    "env.MRT_GCV2_*": "UNSET",
}
LEDGER_OBSERVER_ENV = {**FAIR_OBSERVER_ENV, "env.MRT_GC_LOG": "1"}


def read_meta(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line_number, line in enumerate(path.read_text().splitlines(), start=1):
        key, separator, value = line.partition("=")
        if not separator or not key:
            raise ValueError(f"bad metadata line {path}:{line_number}")
        if key in values:
            raise ValueError(f"duplicate metadata key {key}: {path}")
        values[key] = value
    return values


def parse_attempt(directory: Path, expected_observation: str) -> dict[str, object]:
    match = RUN_RE.fullmatch(directory.name)
    if match is None:
        raise ValueError(f"unexpected subject attempt path: {directory}")
    meta = read_meta(directory / "meta.txt")
    if meta.get("arm") != "subject" or meta.get("observation") != expected_observation:
        raise ValueError(f"bad arm/observation metadata: {directory}")
    time_match = TIME_RE.fullmatch((directory / "time.tsv").read_text().strip())
    if time_match is None:
        raise ValueError(f"bad time record: {directory}")
    rc = int((directory / "rc").read_text().strip())
    time_exit = int(time_match.group(3))
    classification = (directory / "classification").read_text().strip()
    return {
        "round": int(match.group(1)),
        "meta": meta,
        "rc": rc,
        "time_exit": time_exit,
        "classification": classification,
        "wall_s": float(time_match.group(1)),
        "maxrss_kb": int(time_match.group(2)),
        "valid": rc == 0 and time_exit == 0 and classification == "COMPLETE",
        "path": str(directory),
    }


def environment(meta: dict[str, str]) -> dict[str, str]:
    return {key: value for key, value in meta.items() if key.startswith("env.")}


def observer_environment(meta: dict[str, str]) -> dict[str, str]:
    return {key: value for key, value in meta.items() if key.startswith("env.MRT_")}


def validate_pair(fair: dict[str, object], ledger: dict[str, object]) -> None:
    fair_meta = fair["meta"]
    ledger_meta = ledger["meta"]
    assert isinstance(fair_meta, dict) and isinstance(ledger_meta, dict)
    drift = [key for key in IDENTITY_KEYS if fair_meta.get(key) != ledger_meta.get(key)]
    if drift:
        raise ValueError(f"identity drift in round {fair['round']}: {','.join(drift)}")
    fair_observer_env = observer_environment(fair_meta)
    ledger_observer_env = observer_environment(ledger_meta)
    if fair_observer_env != FAIR_OBSERVER_ENV:
        raise ValueError(
            f"fair observer recipe in round {fair['round']} must have all MRT_* UNSET; "
            f"got {fair_observer_env}"
        )
    if ledger_observer_env != LEDGER_OBSERVER_ENV:
        raise ValueError(
            f"ledger observer recipe in round {fair['round']} must differ only by "
            f"env.MRT_GC_LOG=1; got {ledger_observer_env}"
        )
    fair_env = environment(fair_meta)
    ledger_env = environment(ledger_meta)
    changed = sorted(
        key for key in fair_env.keys() | ledger_env.keys()
        if fair_env.get(key) != ledger_env.get(key)
    )
    if changed != ["env.MRT_GC_LOG"]:
        raise ValueError(
            f"observer A/B in round {fair['round']} changed {changed}, expected only env.MRT_GC_LOG"
        )
    if fair_env.get("env.MRT_GC_LOG") != "UNSET" or ledger_env.get("env.MRT_GC_LOG") != "1":
        raise ValueError(f"bad MRT_GC_LOG arms in round {fair['round']}")


def percentile(values: list[float], quantile: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def bootstrap(
    pairs: list[tuple[float, float]], samples: int, seed: int
) -> tuple[float, float, float, float]:
    generator = random.Random(seed)
    differences: list[float] = []
    ratios: list[float] = []
    for _ in range(samples):
        chosen = [pairs[generator.randrange(len(pairs))] for _ in pairs]
        fair_median = statistics.median(pair[0] for pair in chosen)
        ledger_median = statistics.median(pair[1] for pair in chosen)
        differences.append(ledger_median - fair_median)
        if fair_median != 0:
            ratios.append(ledger_median / fair_median)
    return (
        percentile(differences, 0.025), percentile(differences, 0.975),
        percentile(ratios, 0.025), percentile(ratios, 0.975),
    )


def write_tsv(path: Path, rows: list[dict[str, object]], fields: list[str]) -> None:
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, dialect="excel-tab", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def fmt(value: float) -> str:
    return "NA" if math.isnan(value) else f"{value:.6f}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("run_root", type=Path)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--min-pairs", type=int, required=True)
    parser.add_argument("--bootstrap-samples", type=int, default=10_000)
    parser.add_argument("--seed", type=int, default=20260824)
    args = parser.parse_args()
    if args.min_pairs < 1 or args.bootstrap_samples < 1:
        parser.error("pair and bootstrap counts must be positive")

    root = args.run_root.resolve()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    fair_dirs = sorted(path.parent for path in (root / "runs").glob("*/*/r*-subject/rc"))
    ledger_dirs = sorted(path.parent for path in (root / "ledger/runs").glob("*/*/r*-subject/rc"))
    attempts: dict[tuple[str, str, int, str], dict[str, object]] = {}
    for observation, directories in (("fair", fair_dirs), ("ledger", ledger_dirs)):
        for directory in directories:
            subtree = "runs" if observation == "fair" else "ledger/runs"
            relative = directory.relative_to(root / subtree)
            workload, heap, _name = relative.parts
            attempt = parse_attempt(directory, observation)
            attempts[(workload, heap, int(attempt["round"]), observation)] = attempt

    pair_rows: list[dict[str, object]] = []
    grouped: dict[tuple[str, str], list[tuple[float, float]]] = defaultdict(list)
    cells = sorted({key[:2] for key in attempts})
    for workload, heap in cells:
        rounds = sorted({key[2] for key in attempts if key[:2] == (workload, heap)})
        for round_number in rounds:
            fair = attempts.get((workload, heap, round_number, "fair"))
            ledger = attempts.get((workload, heap, round_number, "ledger"))
            if fair is None or ledger is None:
                raise ValueError(f"unpaired observer attempt: {workload}/{heap}/r{round_number:02d}")
            validate_pair(fair, ledger)
            valid = bool(fair["valid"]) and bool(ledger["valid"])
            fair_wall = float(fair["wall_s"])
            ledger_wall = float(ledger["wall_s"])
            if valid:
                grouped[(workload, heap)].append((fair_wall, ledger_wall))
            pair_rows.append({
                "workload": workload, "heap": heap, "round": round_number,
                "fair_rc": fair["rc"], "ledger_rc": ledger["rc"], "valid": valid,
                "fair_wall_s": fmt(fair_wall), "ledger_wall_s": fmt(ledger_wall),
                "delta_s": fmt(ledger_wall - fair_wall),
                "ratio": fmt(ledger_wall / fair_wall) if fair_wall else "NA",
                "fair_path": fair["path"], "ledger_path": ledger["path"],
            })

    pair_fields = [
        "workload", "heap", "round", "fair_rc", "ledger_rc", "valid", "fair_wall_s",
        "ledger_wall_s", "delta_s", "ratio", "fair_path", "ledger_path",
    ]
    write_tsv(output / "pairs.tsv", pair_rows, pair_fields)

    summary_rows: list[dict[str, object]] = []
    for workload, heap in cells:
        valid_pairs = grouped.get((workload, heap), [])
        pair_total = sum(row["workload"] == workload and row["heap"] == heap for row in pair_rows)
        if valid_pairs:
            fair_values = [pair[0] for pair in valid_pairs]
            ledger_values = [pair[1] for pair in valid_pairs]
            fair_median = statistics.median(fair_values)
            ledger_median = statistics.median(ledger_values)
            delta_median = statistics.median(pair[1] - pair[0] for pair in valid_pairs)
            ratio_medians = ledger_median / fair_median if fair_median else math.nan
            diff_low, diff_high, ratio_low, ratio_high = bootstrap(
                valid_pairs, args.bootstrap_samples, args.seed
            )
        else:
            fair_values = []
            ledger_values = []
            fair_median = ledger_median = delta_median = ratio_medians = math.nan
            diff_low = diff_high = ratio_low = ratio_high = math.nan
        if len(valid_pairs) < args.min_pairs:
            status = "INSUFFICIENT"
        elif diff_low > 0:
            status = "OBSERVED_INCREASE"
        elif diff_high < 0:
            status = "OBSERVED_DECREASE"
        else:
            status = "INCONCLUSIVE"
        summary_rows.append({
            "workload": workload, "heap": heap, "pairs_total": pair_total,
            "valid_pairs": len(valid_pairs), "min_pairs": args.min_pairs, "status": status,
            "fair_median_s": fmt(fair_median), "ledger_median_s": fmt(ledger_median),
            "paired_delta_median_s": fmt(delta_median), "ratio_of_medians": fmt(ratio_medians),
            "delta_ci_low_s": fmt(diff_low), "delta_ci_high_s": fmt(diff_high),
            "ratio_ci_low": fmt(ratio_low), "ratio_ci_high": fmt(ratio_high),
            "fair_range_s": f"{fmt(min(fair_values))}..{fmt(max(fair_values))}" if fair_values else "NA",
            "ledger_range_s": f"{fmt(min(ledger_values))}..{fmt(max(ledger_values))}" if ledger_values else "NA",
        })
    summary_fields = [
        "workload", "heap", "pairs_total", "valid_pairs", "min_pairs", "status",
        "fair_median_s", "ledger_median_s", "paired_delta_median_s", "ratio_of_medians",
        "delta_ci_low_s", "delta_ci_high_s", "ratio_ci_low", "ratio_ci_high",
        "fair_range_s", "ledger_range_s",
    ]
    write_tsv(output / "summary.tsv", summary_rows, summary_fields)
    print(f"OBSERVER_ANALYSIS_DONE pairs={len(pair_rows)} out={output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
