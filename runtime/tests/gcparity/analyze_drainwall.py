#!/usr/bin/env python3
"""Summarize paired drainwall runs without third-party modules."""

from __future__ import annotations

import csv
import math
import re
import statistics
import sys
from pathlib import Path


MINOR_CYCLE_RE = re.compile(r"rec=cycle .*kind=minor")


def cycle_durations_ms(text: str, kind: str) -> list[float]:
    return [
        float(value) / 1e6
        for value in re.findall(rf"rec=cycle .*kind={kind} .*dur_ns=([0-9]+)", text)
    ]


def p90_nearest(values: list[float]) -> float:
    ordered = sorted(values)
    return ordered[math.ceil(0.90 * len(ordered)) - 1]


def student_pdf(x: float, df: int) -> float:
    return (
        math.gamma((df + 1.0) / 2.0)
        / (math.sqrt(df * math.pi) * math.gamma(df / 2.0))
        * (1.0 + x * x / df) ** (-(df + 1.0) / 2.0)
    )


def simpson(function, low: float, high: float) -> float:
    middle = (low + high) / 2.0
    return (high - low) * (function(low) + 4.0 * function(middle) + function(high)) / 6.0


def adaptive_simpson(function, low: float, high: float, epsilon: float = 1e-12, depth: int = 20) -> float:
    whole = simpson(function, low, high)

    def integrate(begin: float, end: float, estimate: float, remaining: int) -> float:
        middle = (begin + end) / 2.0
        left = simpson(function, begin, middle)
        right = simpson(function, middle, end)
        delta = left + right - estimate
        if remaining == 0 or abs(delta) <= 15.0 * epsilon:
            return left + right + delta / 15.0
        return integrate(begin, middle, left, remaining - 1) + integrate(
            middle, end, right, remaining - 1
        )

    return integrate(low, high, whole, depth)


def paired_test(base: list[float], fix: list[float]) -> tuple[list[float], float, float, float, float, int, float]:
    differences = [candidate - control for control, candidate in zip(base, fix)]
    count = len(differences)
    mean = statistics.mean(differences)
    deviation = statistics.stdev(differences)
    error = deviation / math.sqrt(count)
    statistic = mean / error if error else math.inf
    degrees = count - 1
    area = adaptive_simpson(lambda value: student_pdf(value, degrees), 0.0, abs(statistic))
    probability = max(0.0, min(1.0, 1.0 - 2.0 * area))
    return differences, mean, deviation, error, statistic, degrees, probability


def signs(differences: list[float]) -> tuple[int, int, int]:
    return (
        sum(value < 0 for value in differences),
        sum(value > 0 for value in differences),
        sum(value == 0 for value in differences),
    )


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(f"usage: {sys.argv[0]} SUMMARY.tsv [RUNS_DIR]", file=sys.stderr)
        return 2
    with open(sys.argv[1], newline="", encoding="utf-8") as source:
        rows = list(csv.DictReader(source, delimiter="\t"))

    by_arm: dict[str, dict[int, dict[str, str]]] = {}
    for row in rows:
        by_arm.setdefault(row["arm"], {})[int(row["round"])] = row
    rounds = sorted(set(by_arm["base"]) & set(by_arm["fix"]))
    if len(rounds) < 2:
        raise ValueError(f"need at least two complete pairs, got {len(rounds)}")
    if not all(by_arm[arm][round_number]["class"] == "GOLD" for arm in ("base", "fix") for round_number in rounds):
        raise ValueError("non-GOLD row in paired input")

    def series(arm: str, field: str) -> list[float]:
        return [float(by_arm[arm][round_number][field]) for round_number in rounds]

    walls = {arm: series(arm, "wall_ms") for arm in ("base", "fix")}
    for arm in ("base", "fix"):
        values = walls[arm]
        print(
            "WALL", arm, f"n={len(values)}", f"mean={statistics.mean(values):.6f}",
            f"median={statistics.median(values):.6f}", f"p90_nearest={p90_nearest(values):.6f}",
            f"stdev={statistics.stdev(values):.6f}",
            f"cv_pct={statistics.stdev(values) / statistics.mean(values) * 100.0:.6f}",
        )

    wall_test = paired_test(walls["base"], walls["fix"])
    print(
        "WALL_PAIRED", f"mean_diff={wall_test[1]:.6f}", f"stdev_diff={wall_test[2]:.6f}",
        f"se={wall_test[3]:.6f}", f"t={wall_test[4]:.9f}", f"df={wall_test[5]}",
        f"p_two_sided={wall_test[6]:.9f}", f"fix_wins_base_wins_ties={signs(wall_test[0])}",
        f"mean_pct={(statistics.mean(walls['fix']) / statistics.mean(walls['base']) - 1.0) * 100.0:.6f}",
    )
    print("WALL_DIFFS", ",".join(f"{value:.0f}" for value in wall_test[0]))

    # A major invokes DoYoungGarbageCollection before tracing old.  The phase
    # timers and young STW records therefore normalize by all young-phase
    # invocations, not only top-level rec=cycle kind=minor records.
    minor = {arm: series(arm, "young_n") for arm in ("base", "fix")}
    if len(sys.argv) == 3:
        runs = Path(sys.argv[2])
        logs = {
            arm: {
                round_number: (runs / f"r{round_number:02d}-{arm}.log").read_text(errors="replace")
                for round_number in rounds
            }
            for arm in ("base", "fix")
        }
        top_minor = {
            arm: [
                float(len(MINOR_CYCLE_RE.findall(logs[arm][round_number])))
                for round_number in rounds
            ]
            for arm in ("base", "fix")
        }
        top_major = {
            arm: [minor_count - top_minor_count for minor_count, top_minor_count in zip(minor[arm], top_minor[arm])]
            for arm in ("base", "fix")
        }
    minor_test = paired_test(minor["base"], minor["fix"])
    print(
        "MINOR", "normalizer=young_phase_invocations", f"base_total={sum(minor['base']):.0f}",
        f"fix_total={sum(minor['fix']):.0f}",
        f"base_mean={statistics.mean(minor['base']):.6f}",
        f"fix_mean={statistics.mean(minor['fix']):.6f}", f"mean_diff={minor_test[1]:.6f}",
        f"t={minor_test[4]:.9f}", f"df={minor_test[5]}", f"p_two_sided={minor_test[6]:.9f}",
        f"fix_less_more_equal={signs(minor_test[0])}",
    )
    if len(sys.argv) == 3:
        print(
            "TOP_LEVEL_CYCLES", f"base_minor={sum(top_minor['base']):.0f}",
            f"fix_minor={sum(top_minor['fix']):.0f}", f"base_major={sum(top_major['base']):.0f}",
            f"fix_major={sum(top_major['fix']):.0f}",
        )
        for kind, counts in (("minor", top_minor), ("major", top_major)):
            test = paired_test(counts["base"], counts["fix"])
            print(
                "CYCLE_COUNT", kind, f"mean_diff={test[1]:.6f}", f"t={test[4]:.9f}",
                f"df={test[5]}", f"p_two_sided={test[6]:.9f}",
                f"fix_less_more_equal={signs(test[0])}",
            )
        for kind in ("minor", "major"):
            durations = {
                arm: [
                    sum(cycle_durations_ms(logs[arm][round_number], kind))
                    for round_number in rounds
                ]
                for arm in ("base", "fix")
            }
            test = paired_test(durations["base"], durations["fix"])
            print(
                "CYCLE_DURATION", kind,
                f"base_per_run_ms={statistics.mean(durations['base']):.6f}",
                f"fix_per_run_ms={statistics.mean(durations['fix']):.6f}",
                f"mean_diff_ms={test[1]:.6f}", f"t={test[4]:.9f}", f"df={test[5]}",
                f"p_two_sided={test[6]:.9f}", f"fix_less_more_equal={signs(test[0])}",
            )

    for field, label, scale in (
        ("pinned_scan_us", "young.pinned_scan", 1.0),
        ("remset_drain_us", "young.remset_drain", 1.0),
        ("remset_rescan_us", "young.remset_rescan", 1.0),
        ("young_sum_ns", "young.STW_held", 1e-6),
    ):
        values = {arm: [value * scale for value in series(arm, field)] for arm in ("base", "fix")}
        run_mean = {arm: statistics.mean(values[arm]) for arm in ("base", "fix")}
        aggregate_minor = {arm: sum(values[arm]) / sum(minor[arm]) for arm in ("base", "fix")}
        run_minor = {
            arm: [value / count for value, count in zip(values[arm], minor[arm])]
            for arm in ("base", "fix")
        }
        test = paired_test(run_minor["base"], run_minor["fix"])
        print(
            "PHASE", label, f"base_per_run={run_mean['base']:.6f}",
            f"fix_per_run={run_mean['fix']:.6f}",
            f"per_run_pct={(run_mean['fix'] / run_mean['base'] - 1.0) * 100.0:.6f}",
            f"base_per_minor={aggregate_minor['base']:.6f}",
            f"fix_per_minor={aggregate_minor['fix']:.6f}",
            f"per_minor_pct={(aggregate_minor['fix'] / aggregate_minor['base'] - 1.0) * 100.0:.6f}",
            f"paired_per_minor_t={test[4]:.9f}", f"paired_per_minor_p={test[6]:.9f}",
            f"fix_less_more_equal={signs(test[0])}",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
