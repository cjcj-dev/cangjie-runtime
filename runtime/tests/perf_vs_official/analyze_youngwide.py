#!/usr/bin/env python3
"""Summarize the default-off youngwide holder/closure census."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from statistics import median


SEED_RE = re.compile(
    r"\[GCV2\]\[youngwide\]\[seeds\] raw=(?P<raw>\d+) unique=(?P<unique>\d+) "
    r"duplicate=(?P<duplicate>\d+) young=(?P<young>\d+) old=(?P<old>\d+) "
    r"unknown=(?P<unknown>\d+) target_young_only=(?P<target_young_only>\d+) "
    r"target_old_only=(?P<target_old_only>\d+) target_both=(?P<target_both>\d+) "
    r"target_unclassified=(?P<target_unclassified>\d+) age_objects=\[(?P<age_objects>[\d,]+)\]"
)
CLOSURE_RE = re.compile(
    r"\[GCV2\]\[youngwide\]\[closure\] entries=(?P<entries>\d+) unique=(?P<unique>\d+) "
    r"duplicate=(?P<duplicate>\d+) young=(?P<young>\d+) old=(?P<old>\d+) "
    r"unknown=(?P<unknown>\d+) walked_old=(?P<walked_old>\d+) "
    r"age_objects=\[(?P<age_objects>[\d,]+)\] "
    r"candidate_age_regions=\[(?P<candidate_age_regions>[\d,]+)\] "
    r"candidate_live_bytes=\[(?P<candidate_live_bytes>[\d,]+)\] threshold=(?P<threshold>\d+)"
)


def percentile(values: list[int], q: float) -> float:
    ordered = sorted(values)
    at = (len(ordered) - 1) * q
    lo = int(at)
    hi = min(lo + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (at - lo)


def one_report(run: Path) -> str:
    reports = list(run.glob("report.log*"))
    if len(reports) != 1:
        raise ValueError(f"{run}: expected one report.log*, got {len(reports)}")
    return reports[0].read_text(errors="replace")


def parse_match(pattern: re.Pattern[str], text: str, run: Path) -> dict[str, object]:
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        raise ValueError(f"{run}: expected one {pattern.pattern[:32]!r} match, got {len(matches)}")
    result: dict[str, object] = {}
    for name, value in matches[0].groupdict().items():
        result[name] = [int(item) for item in value.split(",")] if "," in value else int(value)
    return result


def load_run(run: Path) -> dict[str, object]:
    if (run / "classification").read_text().strip() != "COMPLETE":
        raise ValueError(f"{run}: not COMPLETE")
    text = one_report(run)
    seed = parse_match(SEED_RE, text, run)
    closure = parse_match(CLOSURE_RE, text, run)

    if seed["raw"] != seed["unique"] + seed["duplicate"]:
        raise ValueError(f"{run}: seed raw ledger does not close")
    if seed["unique"] != seed["young"] + seed["old"] + seed["unknown"]:
        raise ValueError(f"{run}: seed generation classes do not close")
    target_total = sum(seed[name] for name in (
        "target_young_only", "target_old_only", "target_both", "target_unclassified"))
    if seed["unique"] != target_total:
        raise ValueError(f"{run}: seed target cohorts do not close")
    if closure["entries"] != closure["unique"] + closure["duplicate"]:
        raise ValueError(f"{run}: closure entry ledger does not close")
    if closure["entries"] != closure["young"] + closure["old"] + closure["unknown"]:
        raise ValueError(f"{run}: closure generation classes do not close")
    if len(seed["age_objects"]) != 16 or len(closure["age_objects"]) != 16:
        raise ValueError(f"{run}: expected 16 page-age buckets")
    if len(closure["candidate_age_regions"]) != 16 or len(closure["candidate_live_bytes"]) != 16:
        raise ValueError(f"{run}: expected 16 candidate age buckets")
    return {"run": run.name, "seed": seed, "closure": closure}


def scalar_stats(values: list[int]) -> dict[str, float | int]:
    return {
        "n": len(values),
        "median": median(values),
        "p90": percentile(values, 0.9),
        "min": min(values),
        "max": max(values),
    }


def summarize(root: Path) -> dict[str, object]:
    runs = [load_run(path) for path in sorted(root.glob("r*-subject"))]
    if len(runs) < 10:
        raise ValueError(f"need N>=10 COMPLETE runs, got {len(runs)}")

    scalar: dict[str, dict[str, float | int]] = {}
    for section in ("seed", "closure"):
        for name, first in runs[0][section].items():
            if isinstance(first, int):
                scalar[f"{section}.{name}"] = scalar_stats([run[section][name] for run in runs])

    arrays: dict[str, list[dict[str, float | int]]] = {}
    for section, names in (("seed", ("age_objects",)),
                           ("closure", ("age_objects", "candidate_age_regions", "candidate_live_bytes"))):
        for name in names:
            arrays[f"{section}.{name}"] = [
                scalar_stats([run[section][name][age] for run in runs]) for age in range(16)
            ]
    return {"root": str(root), "n": len(runs), "scalar": scalar, "arrays": arrays, "runs": runs}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runs", type=Path, help="directory containing rNN-subject directories")
    parser.add_argument("--compact", action="store_true")
    args = parser.parse_args()
    print(json.dumps(summarize(args.runs), ensure_ascii=False,
                     indent=None if args.compact else 2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
