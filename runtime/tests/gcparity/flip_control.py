#!/usr/bin/env python3
"""Positive control for MRT_GCV2_MINOR_YOUNG_FLIP.

`set_good_masks()` has exactly four callers, all of them flips (WCollector.h:166,172,
180,191): young/old relocate-start and young/old mark-start. MRT_GCV2_MASKEQUIV=1
increments `checked` on every one of them and prints the total at exit, so `checked`
is a direct count of colour flips.

A minor cycle flips young mark-start once, and — only when the flag is on — young
relocate-start once more. A major cycle flips all four. That gives an exact integer
prediction per run:

    checked == minors * (1 + flip) + majors * 4

so `extra = checked - minors - 4*majors` must be `minors` in the ON arm and `0` in
the OFF arm. This is stronger than "some counter moved": it pins the exact number of
extra flips, and it is immune to the arms drawing different numbers of major cycles.

The control named in the task brief -- MRT_GCV2_WAITFWD `call>0` vs `call==0` -- is
recorded separately as falsified: WCollector::Preforward flips young unconditionally,
so `relocate_or_remap_object` is reached in both arms.
"""
import csv
import json
import re
import sys
from pathlib import Path

BASE = Path("/root/minorflip-run")
STW_RE = re.compile(r"young collection stw time ([0-9,]+) us")
CHECKED_RE = re.compile(r"\[GCV2\]\[maskequiv\] checked=(\d+) mismatch=(\d+) inject=(\d+)")
PREFORWARD_RE = re.compile(r"^.*\bPreforward\b.*$", re.MULTILINE)

tag = sys.argv[1] if len(sys.argv) > 1 else "control"
rows = []
with (BASE / f"evidence/{tag}-progress.tsv").open(newline="") as handle:
    rows = list(csv.DictReader(
        (line for line in handle if line.strip() != "DONE"), delimiter="\t"))

results = []
violations = []
for row in rows:
    run_dir = (BASE / f"runs/{tag}" / row["workload"] / row["heap"] /
               row["mode"] / row["arm"] / row["round"])
    report = sorted(run_dir.glob("report.log.*"))[0].read_text(errors="replace")
    runtime = sorted(run_dir.glob("runtime.log.*"))[0].read_text(errors="replace")
    combined = report + "\n" + runtime
    minors = len(STW_RE.findall(combined))
    majors = len(PREFORWARD_RE.findall(report))
    found = CHECKED_RE.findall(combined)
    if not found:
        violations.append(f"{run_dir}: no maskequiv atexit line")
        continue
    checked, mismatch, injected = (int(v) for v in found[-1])
    flip = 1 if row["arm"] == "ON" else 0
    extra = checked - minors - 4 * majors
    predicted = minors * flip
    ok = extra == predicted and mismatch == 0
    if not ok:
        violations.append(
            f"{row['workload']}/{row['mode']}/{row['arm']}/{row['round']}: "
            f"checked={checked} minors={minors} majors={majors} "
            f"extra={extra} predicted={predicted} mismatch={mismatch}")
    results.append({
        "workload": row["workload"], "mode": row["mode"], "arm": row["arm"],
        "round": row["round"], "rc": int(row["rc"]), "minors": minors,
        "majors": majors, "checked": checked, "extra_flips": extra,
        "predicted_extra_flips": predicted, "mismatch": mismatch,
        "injected": injected, "model_ok": ok,
    })

by_arm = {}
for arm in ("OFF", "ON"):
    subset = [r for r in results if r["arm"] == arm]
    by_arm[arm] = {
        "runs": len(subset),
        "extra_flips_total": sum(r["extra_flips"] for r in subset),
        "minors_total": sum(r["minors"] for r in subset),
        "majors_total": sum(r["majors"] for r in subset),
        "checked_total": sum(r["checked"] for r in subset),
        "extra_flips_distinct": sorted({r["extra_flips"] for r in subset}),
        "model_ok": all(r["model_ok"] for r in subset),
    }

out = {
    "tag": tag,
    "model": "checked == minors*(1+flip) + majors*4",
    "runs": len(results),
    "violations": violations,
    "by_arm": by_arm,
    "verdict": ("PASS" if not violations and
                by_arm["OFF"]["extra_flips_total"] == 0 and
                by_arm["ON"]["extra_flips_total"] == by_arm["ON"]["minors_total"] and
                by_arm["ON"]["minors_total"] > 0
                else "FAIL"),
    "per_run": results,
}
dest = BASE / f"evidence/control-{tag}.json"
dest.write_text(json.dumps(out, indent=2, sort_keys=True))
print(json.dumps({k: out[k] for k in ("model", "runs", "violations", "by_arm", "verdict")},
                 indent=2, sort_keys=True))
print(f"wrote {dest}")
