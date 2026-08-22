#!/usr/bin/env python3
"""Analyze paritywall campaign: wall, per-cycle STW held, cycle counts, 5-pillar phases."""
from __future__ import annotations
import json, math, os, re, statistics, sys
from collections import defaultdict
from pathlib import Path

STW_HELD = re.compile(r"rec=stw\b.*\bheld_ns=(\d+)")
STW_SEQ = re.compile(r"rec=stw\b.*\bseq=(\d+).*?\bheld_ns=(\d+)")
STW_OFFICIAL = re.compile(r"(?:stw time|light sync time) (\d+) us")
CYCLE = re.compile(r"rec=cycle\b.*\bseq=(\d+)\s+kind=(\S+)")
BEGIN = re.compile(r"Begin GC log\. GCReason: ([^,]+),")
PHASE = re.compile(r"rec=phase\b.*\bname=(\S+)\s+us=(\d+)")
WALL = re.compile(r"wall_s=([0-9.]+)")

PILLARS = {
    "ref_fix": re.compile(r"ref.?fix|fix.?ref|FixRef|ref_fix", re.I),
    "mark": re.compile(r"mark", re.I),
    "evac_finish": re.compile(r"evac_finish|evac.?finish", re.I),
    "drain": re.compile(r"drain|remset", re.I),
    "copy": re.compile(r"copy|reloc|evac(?!_finish)", re.I),
}


def pctile(vals, q):
    if not vals:
        return math.nan
    s = sorted(vals)
    if len(s) == 1:
        return float(s[0])
    pos = (len(s) - 1) * q
    lo, hi = math.floor(pos), math.ceil(pos)
    if lo == hi:
        return float(s[lo])
    f = pos - lo
    return s[lo] * (1 - f) + s[hi] * f


def parse_logs(d: Path):
    text = ""
    for p in sorted(d.glob("*")):
        if p.is_file() and p.suffix in {".log", ".txt", ""} or p.name in {
            "stderr", "stdout", "runtime.log", "report.log", "time.tsv"
        }:
            try:
                text += p.read_text(errors="replace") + "\n"
            except Exception:
                pass
    for extra in ("stderr", "stdout", "runtime.log", "report.log", "time.tsv"):
        fp = d / extra
        if fp.is_file():
            text += fp.read_text(errors="replace") + "\n"
    return text


def cycle_pauses(text: str):
    """Per-cycle sum of held_ns. Prefer rec=stw grouped by seq; else official us."""
    by_seq = defaultdict(int)
    for m in STW_SEQ.finditer(text):
        by_seq[int(m.group(1))] += int(m.group(2))
    if by_seq:
        return list(by_seq.values())
    held = [int(x) for x in STW_HELD.findall(text)]
    if held:
        return held
    us = [int(x) * 1000 for x in STW_OFFICIAL.findall(text)]
    return us


def cycles(text: str):
    kinds = [k for _, k in CYCLE.findall(text)]
    if kinds:
        minor = sum(1 for k in kinds if "young" in k.lower() or "minor" in k.lower())
        major = sum(1 for k in kinds if "old" in k.lower() or "full" in k.lower() or "major" in k.lower())
        other = len(kinds) - minor - major
        return len(kinds), minor, major, other, kinds
    reasons = BEGIN.findall(text)
    if reasons:
        minor = sum(1 for r in reasons if "YOUNG" in r.upper() or "MINOR" in r.upper())
        major = len(reasons) - minor
        return len(reasons), minor, major, 0, reasons
    return 0, 0, 0, 0, []


def pillars(text: str):
    acc = defaultdict(int)
    other = 0
    total = 0
    for name, us in PHASE.findall(text):
        us = int(us)
        total += us
        hit = None
        for key, rx in PILLARS.items():
            if rx.search(name):
                hit = key
                break
        if hit:
            acc[hit] += us
        else:
            other += us
            acc["_other_names"]  # placeholder
            acc.setdefault("_names", {})
    names = defaultdict(int)
    for name, us in PHASE.findall(text):
        names[name] += int(us)
    return dict(acc), other, total, dict(names)


def wall_of(d: Path):
    t = d / "time.tsv"
    if t.is_file():
        m = WALL.search(t.read_text())
        if m:
            return float(m.group(1))
    return math.nan


def main(root: Path):
    runs = root / "runs"
    cells = defaultdict(list)
    for run_dir in sorted(runs.rglob("r*-*")):
        if not run_dir.is_dir():
            continue
        parts = run_dir.relative_to(runs).parts
        # workload/heap/rNN-arm
        if len(parts) < 3:
            continue
        wl, heap, leaf = parts[0], parts[1], parts[-1]
        arm = "subject" if leaf.endswith("subject") else "official"
        text = parse_logs(run_dir)
        cls = (run_dir / "classification").read_text().strip() if (run_dir / "classification").exists() else "?"
        w = wall_of(run_dir)
        pauses = cycle_pauses(text)
        n, minor, major, other, _ = cycles(text)
        pil, oth, tot, names = pillars(text)
        cells[(wl, heap, arm)].append({
            "dir": str(run_dir),
            "class": cls,
            "wall": w,
            "pauses_ns": pauses,
            "pause_sum_ns": sum(pauses),
            "n_pause_cycles": len(pauses),
            "cycles": n,
            "minor": minor,
            "major": major,
            "pillars": pil,
            "phase_other_us": oth,
            "phase_total_us": tot,
            "phase_names": names,
        })

    summary = {}
    for key, rows in sorted(cells.items()):
        ok = [r for r in rows if r["class"] == "COMPLETE" and not math.isnan(r["wall"])]
        walls = [r["wall"] for r in ok]
        all_pauses = [p for r in ok for p in r["pauses_ns"]]
        cyc = [r["cycles"] for r in ok]
        minors = [r["minor"] for r in ok]
        majors = [r["major"] for r in ok]
        per_run_p50 = []
        per_run_p99 = []
        for r in ok:
            if r["pauses_ns"]:
                per_run_p50.append(pctile(r["pauses_ns"], 0.50))
                per_run_p99.append(pctile(r["pauses_ns"], 0.99))
        pil_sum = defaultdict(int)
        names_sum = defaultdict(int)
        tot = 0
        for r in ok:
            tot += r["phase_total_us"]
            for k, v in r["pillars"].items():
                if not k.startswith("_"):
                    pil_sum[k] += v
            for k, v in r["phase_names"].items():
                names_sum[k] += v
        summary["/".join(key)] = {
            "n_ok": len(ok),
            "n_try": len(rows),
            "wall_med": pctile(walls, 0.5),
            "wall_p90": pctile(walls, 0.9),
            "wall_p99": pctile(walls, 0.99),
            "pause_pooled_p50_ns": pctile(all_pauses, 0.5),
            "pause_pooled_p90_ns": pctile(all_pauses, 0.9),
            "pause_pooled_p99_ns": pctile(all_pauses, 0.99),
            "pause_run_p50_med_ns": pctile(per_run_p50, 0.5),
            "pause_run_p99_med_ns": pctile(per_run_p99, 0.5),
            "n_pause_events": len(all_pauses),
            "cycles_med": pctile(cyc, 0.5),
            "minor_med": pctile(minors, 0.5),
            "major_med": pctile(majors, 0.5),
            "pillars_us": dict(pil_sum),
            "phase_total_us": tot,
            "top_phases": sorted(names_sum.items(), key=lambda x: -x[1])[:15],
        }
    out = root / "analysis"
    out.mkdir(exist_ok=True)
    (out / "summary.json").write_text(json.dumps(summary, indent=2, default=str))
    print(json.dumps(summary, indent=2, default=str))


if __name__ == "__main__":
    main(Path(sys.argv[1]))
