#!/usr/bin/env python3
"""Analyze paritywall campaign: wall, per-cycle STW held, cycle counts, 5-pillar phases.

GCLOG is read once from stderr only (subject). Official uses report.log+runtime.log once.
Never concatenate the same file twice — a prior extra loop doubled rec=cycle and held_ns.
wall is always taken from time.tsv once and is unaffected by that bug.
"""
from __future__ import annotations
import json, math, re, sys
from collections import defaultdict
from pathlib import Path

STW_SEQ = re.compile(r"rec=stw\b.*\bseq=(\d+)\s+reason=(\S+)\s+wait_ns=(\d+)\s+held_ns=(\d+)")
STW_OFFICIAL = re.compile(r"(?:stw time|light sync time) (\d+) us")
CYCLE = re.compile(
    r"rec=cycle\b.*\bseq=(\d+)\s+kind=(\S+)\s+reason=(\S+)\s+start_ns=(\d+)\s+dur_ns=(\d+)"
)
BEGIN = re.compile(r"Begin GC log\. GCReason: ([^,]+),")
PHASE = re.compile(r"rec=phase\b.*\bname=(\S+)\s+us=(\d+)")
WALL = re.compile(r"wall_s=([0-9.]+)")
CONC_PHASE = re.compile(r"concurrent|conc_|satb|re-marking|remark", re.I)

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


def read_once(paths):
    seen = set()
    text = ""
    for p in paths:
        if p is None or not p.is_file():
            continue
        key = p.resolve()
        if key in seen:
            continue
        seen.add(key)
        text += p.read_text(errors="replace") + "\n"
    return text


def parse_logs(d: Path, arm: str):
    """Subject GCLOG lives on stderr; official pause/cycle live on report+runtime."""
    if arm == "subject":
        return read_once([d / "stderr"])
    return read_once([d / "report.log", d / "runtime.log", d / "stderr"])


def cycle_pauses(text: str):
    by_seq = defaultdict(int)
    for m in STW_SEQ.finditer(text):
        by_seq[int(m.group(1))] += int(m.group(4))
    if by_seq:
        return dict(by_seq)
    us = [int(x) * 1000 for x in STW_OFFICIAL.findall(text)]
    return {i: v for i, v in enumerate(us, 1)}


def cycle_durs(text: str):
    out = {}
    kinds = {}
    for m in CYCLE.finditer(text):
        seq = int(m.group(1))
        out[seq] = int(m.group(5))
        kinds[seq] = m.group(2)
    if out:
        return out, kinds
    reasons = BEGIN.findall(text)
    return {i: 0 for i, _ in enumerate(reasons, 1)}, {
        i: ("minor" if "YOUNG" in r.upper() or "MINOR" in r.upper() else "major")
        for i, r in enumerate(reasons, 1)
    }


def pillars(text: str):
    acc = defaultdict(int)
    names = defaultdict(int)
    conc = 0
    total = 0
    wait = 0
    for name, us in PHASE.findall(text):
        us = int(us)
        names[name] += us
        total += us
        if name == "finalizerProcessor_waitting_time":
            wait += us
            continue
        if CONC_PHASE.search(name):
            conc += us
        hit = None
        for key, rx in PILLARS.items():
            if rx.search(name):
                hit = key
                break
        if hit:
            acc[hit] += us
    return dict(acc), dict(names), total, conc, wait


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
        if len(parts) < 3:
            continue
        wl, heap, leaf = parts[0], parts[1], parts[-1]
        arm = "subject" if leaf.endswith("subject") else "official"
        text = parse_logs(run_dir, arm)
        cls = (run_dir / "classification").read_text().strip() if (run_dir / "classification").exists() else "?"
        w = wall_of(run_dir)
        pauses = cycle_pauses(text)
        durs, kinds = cycle_durs(text)
        n = len(durs)
        minor = sum(1 for k in kinds.values() if "young" in k.lower() or "minor" in k.lower())
        major = sum(1 for k in kinds.values() if "old" in k.lower() or "full" in k.lower() or "major" in k.lower())
        pil, names, tot, conc, wait = pillars(text)
        pause_sum = sum(pauses.values())
        gc_dur = sum(durs.values())
        cells[(wl, heap, arm)].append({
            "dir": str(run_dir),
            "class": cls,
            "wall": w,
            "pauses_ns": list(pauses.values()),
            "pause_sum_ns": pause_sum,
            "gc_dur_ns": gc_dur,
            "conc_gc_ns": max(0, gc_dur - pause_sum),
            "mutator_ns": (w * 1e9 - gc_dur) if not math.isnan(w) else math.nan,
            "cycles": n,
            "minor": minor,
            "major": n - minor if major == 0 and n else major,
            "pillars": pil,
            "phase_total_us": tot,
            "phase_conc_us": conc,
            "phase_wait_us": wait,
            "phase_names": names,
        })

    summary = {}
    for key, rows in sorted(cells.items()):
        ok = [r for r in rows if r["class"] == "COMPLETE" and not math.isnan(r["wall"])]
        walls = [r["wall"] for r in ok]
        all_pauses = [p for r in ok for p in r["pauses_ns"]]
        run_pause = [r["pause_sum_ns"] for r in ok]
        run_gcdur = [r["gc_dur_ns"] for r in ok]
        run_conc = [r["conc_gc_ns"] for r in ok]
        run_mut = [r["mutator_ns"] for r in ok]
        shares = [
            r["pause_sum_ns"] / (r["wall"] * 1e9) for r in ok if r["wall"] and r["wall"] > 0
        ]
        conc_shares = [
            r["conc_gc_ns"] / (r["wall"] * 1e9) for r in ok if r["wall"] and r["wall"] > 0
        ]
        mut_shares = [
            r["mutator_ns"] / (r["wall"] * 1e9) for r in ok if r["wall"] and r["wall"] > 0
        ]
        cyc = [r["cycles"] for r in ok]
        minors = [r["minor"] for r in ok]
        majors = [r["major"] for r in ok]
        pil_sum = defaultdict(int)
        names_sum = defaultdict(int)
        tot = conc_us = wait_us = 0
        for r in ok:
            tot += r["phase_total_us"]
            conc_us += r["phase_conc_us"]
            wait_us += r["phase_wait_us"]
            for k, v in r["pillars"].items():
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
            "pause_sum_per_run_med_ns": pctile(run_pause, 0.5),
            "gc_dur_per_run_med_ns": pctile(run_gcdur, 0.5),
            "conc_gc_per_run_med_ns": pctile(run_conc, 0.5),
            "mutator_per_run_med_ns": pctile(run_mut, 0.5),
            "pause_share_of_wall_med": pctile(shares, 0.5),
            "conc_gc_share_of_wall_med": pctile(conc_shares, 0.5),
            "mutator_share_of_wall_med": pctile(mut_shares, 0.5),
            "n_pause_events": len(all_pauses),
            "cycles_med": pctile(cyc, 0.5),
            "minor_med": pctile(minors, 0.5),
            "major_med": pctile(majors, 0.5),
            "pillars_us": dict(pil_sum),
            "phase_total_us": tot,
            "phase_conc_us": conc_us,
            "phase_wait_us": wait_us,
            "top_phases": sorted(names_sum.items(), key=lambda x: -x[1])[:20],
        }
    out = root / "analysis2"
    out.mkdir(exist_ok=True)
    (out / "summary.json").write_text(json.dumps(summary, indent=2, default=str))
    print(json.dumps(summary, indent=2, default=str))


if __name__ == "__main__":
    main(Path(sys.argv[1]))
