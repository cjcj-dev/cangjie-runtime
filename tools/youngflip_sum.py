#!/usr/bin/env python3
"""Summarize a paired mega.tsv into ARM/ECON lines."""
import collections
import math
import os
import re
import statistics
import sys

out, name = sys.argv[1], sys.argv[2]
rows = []
with open(os.path.join(out, "mega.tsv")) as f:
    for line in f:
        p = line.rstrip("\n").split("\t")
        if len(p) >= 7:
            rows.append(p)


def med(xs):
    return statistics.median(xs) if xs else float("nan")


def mean(xs):
    return statistics.mean(xs) if xs else float("nan")


def p99(xs):
    if not xs:
        return float("nan")
    ordered = sorted(xs)
    return ordered[math.ceil(0.99 * len(ordered)) - 1]


def dump(arm):
    cls = collections.Counter()
    rc = collections.Counter()
    si = collections.Counter()
    abort = collections.Counter()
    ok = 0
    n = 0
    walls = []
    held_all = []
    young_med = []
    young_p99 = []
    young_sum = []
    young_n = []
    minor = []
    stw2 = []
    gctrigger_atexit = []
    gctrigger_armed = []
    gctrigger_turned = []
    for p in rows:
        if p[0] != arm:
            continue
        n += 1
        rci, okf, c = p[4], p[5], p[6]
        rc[rci] += 1
        if okf == "y":
            ok += 1
            cls["GOLD"] += 1
        else:
            cls[c] += 1
        fn = os.path.join(out, f"FAIL_{arm}_{p[1]}_{p[2]}_{p[3]}.txt")
        t = ""
        if os.path.exists(fn):
            t = open(fn, errors="replace").read()
        if rci == "139":
            m = re.findall(r"si_code=\S+|si_addr=\S+", t)
            si[" ".join(m[:4]) if m else "no_si"] += 1
        if rci == "134":
            if "IsLockedState" in t:
                abort["IsLockedState"] += 1
            elif "PREFORWARD" in t:
                abort["phase"] += 1
            elif "toRegion2Idx" in t:
                abort["toRegion2Idx"] += 1
            else:
                abort["other134"] += 1
        if rci == "1":
            if "Cannot allocate" in t or "Out of memory" in t:
                abort["oom"] += 1
            else:
                abort["rc1_other"] += 1
        met = os.path.join(out, f"met_{arm}_{p[1]}_{p[2]}_{p[3]}.tsv")
        if os.path.exists(met):
            rec = {}
            for tok in open(met).read().split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    rec[k] = v

            def f(k):
                try:
                    return float(rec.get(k, 0))
                except Exception:
                    return 0.0

            walls.append(f("wall"))
            vals = rec.get("young_values_ns", "-")
            if vals != "-":
                for value in vals.split(","):
                    try:
                        held_all.append(float(value))
                    except ValueError:
                        pass
            young_med.append(f("young_med_ns"))
            young_p99.append(f("young_p99_ns"))
            young_sum.append(f("young_sum_ns"))
            young_n.append(f("young_n"))
            minor.append(f("minor_cycles"))
            stw2.append(f("stw2"))
            gctrigger_atexit.append(f("gctrigger_atexit"))
            gctrigger_armed.append(f("gctrigger_armed"))
            gctrigger_turned.append(f("gctrigger_turned"))
    line = (
        f"ARM {arm} n={n} golden={ok} GOLD={cls['GOLD']} "
        f"A_locked={cls['A_locked']} A2_phase={cls['A2_phase']} "
        f"A3_toreg={cls['A3_toreg']} A4_other134={cls['A4_other134']} "
        f"B_segv={cls['B_segv']} C_drift={cls['C_drift']} "
        f"D_rc1={cls['D_rc1']} TO={cls['TO']} "
        f"other={sum(v for k, v in cls.items() if k not in ('GOLD','A_locked','A2_phase','A3_toreg','A4_other134','B_segv','C_drift','D_rc1','TO'))} "
        f"rc={dict(rc)} abort={dict(abort)} si_top={si.most_common(5)}"
    )
    econ = (
        f"ECON {arm} n={n} wall_med={med(walls):.3f} wall_p99={p99(walls):.3f} "
        f"wall_mean={mean(walls):.3f} young_held_n={len(held_all)} "
        f"young_held_med_ms={med(held_all)/1e6:.3f} young_held_p99_ms={p99(held_all)/1e6:.3f} "
        f"young_run_sum_med_s={med(young_sum)/1e9:.3f} young_n_med={med(young_n)} "
        f"minor_cyc_med={med(minor)} minor_cyc_sum={int(sum(minor))} "
        f"gctrigger_atexit_pos={sum(1 for x in gctrigger_atexit if x>0)}/{len(gctrigger_atexit)} "
        f"gctrigger_armed_med={med(gctrigger_armed)} gctrigger_armed_sum={int(sum(gctrigger_armed))} "
        f"gctrigger_turned_med={med(gctrigger_turned)} gctrigger_turned_sum={int(sum(gctrigger_turned))} "
        f"stw2_med={med(stw2)} "
        f"stw2_pos={sum(1 for x in stw2 if x>0)}/{len(stw2)}"
    )
    open(os.path.join(out, f"summary_{arm}.txt"), "w").write(line + "\n" + econ + "\n")
    print(line)
    print(econ)


dump("A")
dump("B")
