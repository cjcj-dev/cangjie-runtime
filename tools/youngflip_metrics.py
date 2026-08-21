#!/usr/bin/env python3
"""Parse one run's stderr into metrics.tsv (youngflip)."""
import sys

path, out, wall, rc = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
held = []
reasons = {}
cycles_minor = 0
gctrigger_atexit = 0
gctrigger_armed = 0
gctrigger_turned = 0
stw2 = 0
si = ""
try:
    lines = open(path, errors="replace")
except FileNotFoundError:
    lines = []
for line in lines:
    if "[GCLOG]" in line and "rec=stw" in line and "held_ns=" in line:
        rec = {}
        for tok in line.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                rec[k] = v
        reason = rec.get("reason", "")
        reasons[reason] = reasons.get(reason, 0) + 1
        if reason == "young_collection":
            try:
                held.append(int(rec["held_ns"]))
            except Exception:
                pass
    if "[GCLOG]" in line and "rec=cycle" in line and "kind=minor" in line:
        cycles_minor += 1
    if "[GCV2][gctrigger] atexit" in line:
        gctrigger_atexit += 1
        rec = {}
        for tok in line.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                rec[k] = v
        try:
            gctrigger_armed += int(rec.get("armed", 0))
            gctrigger_turned += int(rec.get("turned", 0))
        except ValueError:
            pass
    if "stw2_fixpoint" in line:
        stw2 += 1
    if "si_addr=" in line and not si:
        parts = [
            t
            for t in line.split()
            if t.startswith("si_addr=") or t.startswith("si_code=")
        ]
        if parts:
            si = " ".join(parts[:4])
held.sort()
med = 0
p99 = 0
if held:
    if len(held) % 2:
        med = held[len(held) // 2]
    else:
        med = (held[len(held) // 2 - 1] + held[len(held) // 2]) // 2
    p99 = held[int(0.99 * (len(held) - 1))]
tot = sum(held) if held else 0
rs = ",".join(f"{k}:{v}" for k, v in sorted(reasons.items()))
held_values = ",".join(str(v) for v in held) or "-"
open(out, "w").write(
    f"wall={wall}\trc={rc}\tyoung_n={len(held)}\tyoung_med_ns={int(med)}\t"
    f"young_p99_ns={int(p99)}\tyoung_sum_ns={tot}\tminor_cycles={cycles_minor}\t"
    f"stw2={stw2}\tgctrigger_atexit={gctrigger_atexit}\t"
    f"gctrigger_armed={gctrigger_armed}\tgctrigger_turned={gctrigger_turned}\t"
    f"young_values_ns={held_values}\tsi={si or '-'}\treasons={rs}\n"
)
