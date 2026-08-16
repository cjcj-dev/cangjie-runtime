#!/usr/bin/env python3
"""Analyse the single-arm MRT_GCV2_MINOR_YOUNG_FLIP campaign.

Ratios are ON/OFF. Confidence intervals are paired-round percentile bootstrap: a
round index is drawn once per resample and used for BOTH arms, so run-to-run drift
that hits both arms cancels instead of inflating the interval.

Young pause is the runtime's own `young collection stw time <n> us`, pooled over
all rounds of an arm; p99 is nearest-rank. Absolute milliseconds are reported next
to every ratio because a ratio alone hides the size of the denominator.
"""
import csv
import hashlib
import json
import math
import random
import re
import sys
from collections import defaultdict
from pathlib import Path

BASE = Path("/root/minorflip-run")
ARMS = ("OFF", "ON")
STW_RE = re.compile(r"young collection stw time ([0-9,]+) us")
CHECKED_RE = re.compile(r"\[GCV2\]\[maskequiv\] checked=(\d+) mismatch=(\d+) inject=(\d+)")
PAR_RE = re.compile(r"\[GCV2\]\[markpar\]\[parallel\].*?parallel=([01])")
EXPECTED_RUNTIME = "c513622a4b085d692a6ab80d10605bf0c9d020c20be3407e5108caccb241a149"
EXPECTED_BOUNDS = "1d7100b624cee27f12bddff59d432e39a17fb7b59e728f75f468a79f6ebc6db7"
EXPECTED_BIN = {
    "allocation_dense": "02ce713eab78b85ef26f293341ac2490cf9d01261781a3517c0377007adaf8da",
    "survival_dense": "2603386ff5ae8da94ec12281a67ced07163205677597a645cf06d67878f30489",
}


def mean(values):
    return sum(values) / len(values)


def nearest_rank(values, q):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(q * len(ordered)) - 1)]


def percentile(sorted_values, q):
    position = (len(sorted_values) - 1) * q
    lower, upper = math.floor(position), math.ceil(position)
    if lower == upper:
        return sorted_values[lower]
    fraction = position - lower
    return sorted_values[lower] * (1.0 - fraction) + sorted_values[upper] * fraction


def ci(values):
    ordered = sorted(values)
    return [percentile(ordered, 0.025), percentile(ordered, 0.975)]


def parse_time(path):
    text = path.read_text(errors="replace")
    wall = re.search(r"wall_s=([0-9.]+)", text)
    rss = re.search(r"maxrss_kb=(\d+)", text)
    code = re.search(r"exit=(\d+)", text)
    if not (wall and rss and code):
        raise ValueError(f"bad time file: {path}")
    return float(wall.group(1)), int(rss.group(1)), int(code.group(1))


def load(tag, rounds):
    progress = BASE / f"evidence/{tag}-progress.tsv"
    with progress.open(newline="") as handle:
        rows = list(csv.DictReader(
            (line for line in handle if line.strip() != "DONE"), delimiter="\t"))
    errors = []
    runs = defaultdict(dict)
    censored = completed = forensic = 0
    stdout_by_workload = defaultdict(set)
    so_shas, bounds_shas = set(), set()

    for row in rows:
        cell = (row["workload"], row["mode"])
        arm, rnd = row["arm"], int(row["round"][1:])
        run_dir = (BASE / f"runs/{tag}" / row["workload"] / row["heap"] /
                   row["mode"] / arm / row["round"])
        wall, rss, time_exit = parse_time(run_dir / "time.txt")
        rc = int((run_dir / "rc").read_text().strip())
        if rc == 0:
            completed += 1
        if rc in (124, 137, 134):
            censored += 1
        if rc != 0 or time_exit != 0:
            errors.append(f"nonzero: {cell}/{arm}/r{rnd:02d} rc={rc} time_exit={time_exit}")

        reports = sorted(run_dir.glob("report.log.*"))
        runtimes = sorted(run_dir.glob("runtime.log.*"))
        if len(reports) != 1 or len(runtimes) != 1:
            errors.append(f"log count: {run_dir}")
            continue
        report_text = reports[0].read_text(errors="replace")
        combined = report_text + "\n" + runtimes[0].read_text(errors="replace")
        stw = [int(v.replace(",", "")) / 1000.0 for v in STW_RE.findall(combined)]
        if stw:
            forensic += 1
        else:
            errors.append(f"zero minor: {run_dir}")

        checked = CHECKED_RE.findall(combined)
        checked_value = int(checked[-1][0]) if checked else None
        mismatch = int(checked[-1][1]) if checked else 0
        if checked and mismatch != 0:
            errors.append(f"maskequiv mismatch: {run_dir} mismatch={mismatch}")

        parallel = PAR_RE.findall(report_text)
        p1, p0 = parallel.count("1"), parallel.count("0")
        if row["mode"] == "parallel" and (p1 != len(stw) or p0 != 0):
            errors.append(f"parallel route: {run_dir} minor={len(stw)} p1={p1} p0={p0}")
        if row["mode"] == "serial" and (p0 != len(stw) or p1 != 0):
            errors.append(f"serial route: {run_dir} minor={len(stw)} p1={p1} p0={p0}")

        meta_text = (run_dir / "meta.txt").read_text(errors="replace")
        shas = re.findall(r"^([0-9a-f]{64})  (.+)$", meta_text, re.MULTILINE)
        if len(shas) != 3:
            errors.append(f"meta SHA count: {run_dir}")
        else:
            if shas[0][0] != EXPECTED_BIN[row["workload"]]:
                errors.append(f"workload SHA: {run_dir}")
            so_shas.add(shas[1][0])
            bounds_shas.add(shas[2][0])
        expected_flip = "1" if arm == "ON" else "0"
        if f"minor_young_flip={expected_flip}" not in meta_text:
            errors.append(f"arm env: {run_dir}")
        if "minor_conc_ref_fix=unset" not in meta_text:
            errors.append(f"conc_ref_fix leaked into arm: {run_dir}")
        if "cores=80-95" not in meta_text or "timeout_signal=ABRT" not in meta_text:
            errors.append(f"meta recipe: {run_dir}")
        expected_position = (1 if arm == "OFF" else 2) if rnd % 2 else (1 if arm == "ON" else 2)
        if int(row["position"]) != expected_position:
            errors.append(f"order: {run_dir}")

        sha = (run_dir / "stdout.sha256").read_text().split()[0]
        stdout_by_workload[row["workload"]].add(sha)
        runs[(cell, arm)][rnd] = {
            "wall_s": wall, "rss_kib": rss, "stw_ms": stw, "minor": len(stw),
            "checked": checked_value, "stdout_sha": sha,
        }

    if so_shas != {EXPECTED_RUNTIME}:
        errors.append(f"runtime SHAs={sorted(so_shas)}")
    if bounds_shas != {EXPECTED_BOUNDS}:
        errors.append(f"bounds SHAs={sorted(bounds_shas)}")

    ordered = {}
    for key, by_round in runs.items():
        if sorted(by_round) != list(range(1, rounds + 1)):
            errors.append(f"rounds: {key} {sorted(by_round)}")
            continue
        ordered[key] = [by_round[i] for i in range(1, rounds + 1)]
    return ordered, errors, {
        "rows": len(rows), "completed": completed, "censored": censored,
        "forensic": forensic,
        "stdout_by_workload": {k: sorted(v) for k, v in stdout_by_workload.items()},
    }


def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else "formal"
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    reps = int(sys.argv[3]) if len(sys.argv) > 3 else 100000
    ordered, errors, audit = load(tag, rounds)
    cells = sorted({cell for cell, _ in ordered})

    summary = {}
    for cell in cells:
        for arm in ARMS:
            values = ordered[(cell, arm)]
            stw = [v for run in values for v in run["stw_ms"]]
            summary["|".join(cell + (arm,))] = {
                "runs": len(values),
                "minor_events": len(stw),
                "wall_mean_s": mean([r["wall_s"] for r in values]),
                "rss_mean_kib": mean([r["rss_kib"] for r in values]),
                "stw_p99_ms": nearest_rank(stw, 0.99),
                "stw_max_ms": max(stw),
                "stw_mean_ms": mean(stw),
                "checked_total": sum(r["checked"] or 0 for r in values),
                "minor_total": sum(r["minor"] for r in values),
            }

    point = {}
    for cell in cells:
        on = summary["|".join(cell + ("ON",))]
        off = summary["|".join(cell + ("OFF",))]
        point["|".join(cell)] = {
            "wall_ratio": on["wall_mean_s"] / off["wall_mean_s"],
            "wall_off_s": off["wall_mean_s"], "wall_on_s": on["wall_mean_s"],
            "stw_p99_ratio": on["stw_p99_ms"] / off["stw_p99_ms"],
            "stw_p99_off_ms": off["stw_p99_ms"], "stw_p99_on_ms": on["stw_p99_ms"],
            "stw_p99_delta_ms": on["stw_p99_ms"] - off["stw_p99_ms"],
            "rss_ratio": on["rss_mean_kib"] / off["rss_mean_kib"],
            "rss_delta_kib": on["rss_mean_kib"] - off["rss_mean_kib"],
        }

    rng = random.Random(20260816)
    boot = {"|".join(cell): defaultdict(list) for cell in cells}
    agg = defaultdict(list)
    for _ in range(reps):
        wall_product = 1.0
        for cell in cells:
            indices = [rng.randrange(rounds) for _ in range(rounds)]
            on_runs, off_runs = ordered[(cell, "ON")], ordered[(cell, "OFF")]
            on_wall = mean([on_runs[i]["wall_s"] for i in indices])
            off_wall = mean([off_runs[i]["wall_s"] for i in indices])
            on_rss = mean([on_runs[i]["rss_kib"] for i in indices])
            off_rss = mean([off_runs[i]["rss_kib"] for i in indices])
            on_stw = [v for i in indices for v in on_runs[i]["stw_ms"]]
            off_stw = [v for i in indices for v in off_runs[i]["stw_ms"]]
            on_p99, off_p99 = nearest_rank(on_stw, 0.99), nearest_rank(off_stw, 0.99)
            target = boot["|".join(cell)]
            target["wall_ratio"].append(on_wall / off_wall)
            target["stw_p99_ratio"].append(on_p99 / off_p99)
            target["stw_p99_delta_ms"].append(on_p99 - off_p99)
            target["rss_ratio"].append(on_rss / off_rss)
            target["rss_delta_kib"].append(on_rss - off_rss)
            wall_product *= on_wall / off_wall
        agg["wall_geomean_ratio"].append(wall_product ** (1.0 / len(cells)))

    comparisons = {}
    for cell in cells:
        key = "|".join(cell)
        item = dict(point[key])
        for metric, values in boot[key].items():
            item[metric + "_ci95"] = ci(values)
        item["wall_pass"] = item["wall_ratio"] <= 1.00 or (
            item["wall_ratio_ci95"][0] <= 1.0 <= item["wall_ratio_ci95"][1])
        item["stw_p99_pass"] = item["stw_p99_ratio"] <= 1.00 or (
            item["stw_p99_ratio_ci95"][0] <= 1.0 <= item["stw_p99_ratio_ci95"][1])
        comparisons[key] = item

    wall_geomean = math.prod(i["wall_ratio"] for i in comparisons.values()) ** (1.0 / len(cells))
    out = {
        "tag": tag, "rounds": rounds, "bootstrap_reps": reps, "seed": 20260816,
        "errors": errors, "audit": audit, "arms": summary, "cells": comparisons,
        "aggregate": {
            "wall_geomean_ratio": wall_geomean,
            "wall_geomean_ratio_ci95": ci(agg["wall_geomean_ratio"]),
        },
        "gates": {
            "wall_all_cells_pass": all(i["wall_pass"] for i in comparisons.values()),
            "stw_p99_all_cells_pass": all(i["stw_p99_pass"] for i in comparisons.values()),
            "stdout_identical_per_workload": all(
                len(v) == 1 for v in audit["stdout_by_workload"].values()),
        },
    }
    dest = BASE / f"evidence/analysis-{tag}.json"
    dest.write_text(json.dumps(out, indent=2, sort_keys=True))
    print(json.dumps({k: out[k] for k in ("errors", "audit", "aggregate", "gates")},
                     indent=2, sort_keys=True))
    print(f"\nwrote {dest} sha256={hashlib.sha256(dest.read_bytes()).hexdigest()}")


main()
