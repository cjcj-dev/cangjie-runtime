#!/usr/bin/env python3
"""Re-derive the paritywall3 SD five pillars from [ZSTAT] records.

Consumer of the ZStatPhase port (runtime/src/Base/ZStat.h). Reads stderr of a run,
takes `rec=zphase` lines, applies the same PILLARS regexes as
kkk2:/root/paritywall-run/analyze.py:24-29, self-normalizes, and compares against the
paritywall3 reference (trust=0 M, aligned window):
    mark 65.0% · drain 19.0% · copy 6.3% · evac_finish 5.2% · ref_fix 4.5%.

A phase's pillar contribution is pause_ns + conc_ns (paritywall3 summed rec=phase
without a kind); the kind split is reported alongside as the new information the
device exists for.

Usage: zstat_pillars.py <stderr.log> [--tolerance PCT_POINTS]
Exits 1 (red) when a pillar differs from the reference by more than tolerance,
or when every ZSTAT total is zero (恒0 = device dead, not "no GC").
"""
from __future__ import annotations

import re
import sys
from collections import defaultdict

REFERENCE = {"mark": 65.0, "drain": 19.0, "copy": 6.3, "evac_finish": 5.2, "ref_fix": 4.5}

# Same five regexes as paritywall-run/analyze.py PILLARS (same iteration order:
# ref_fix first so "ref_fix" never falls into a broader bucket).
PILLARS = {
    "ref_fix": re.compile(r"ref.?fix|fix.?ref|FixRef|ref_fix", re.I),
    "mark": re.compile(r"mark", re.I),
    "evac_finish": re.compile(r"evac_finish|evac.?finish", re.I),
    "drain": re.compile(r"drain|remset", re.I),
    "copy": re.compile(r"copy|reloc|evac(?!_finish)", re.I),
}

ZPHASE = re.compile(r"\[ZSTAT\] v=1 rec=zphase seq=(\d+) name=(\S+) pause_ns=(\d+) conc_ns=(\d+) n=(\d+)")
ZCYCLE = re.compile(r"\[ZSTAT\] v=1 rec=zcycle seq=(\d+) pause_ns=(\d+) conc_ns=(\d+) max_pause_ns=(\d+)")


def main() -> int:
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    tolerance = 3.0
    if "--tolerance" in sys.argv:
        tolerance = float(sys.argv[sys.argv.index("--tolerance") + 1])
    text = open(sys.argv[1], encoding="utf-8", errors="replace").read()

    pillar_us = defaultdict(int)
    pillar_pause_us = defaultdict(int)
    total_us = 0
    n_phases = 0
    n_cycles = 0
    max_pause_ns = 0
    for line in text.splitlines():
        m = ZPHASE.search(line)
        if m:
            name, pause_ns, conc_ns = m.group(2), int(m.group(3)), int(m.group(4))
            n_phases += 1
            us = (pause_ns + conc_ns) / 1000.0
            total_us += us
            for key, rx in PILLARS.items():
                if rx.search(name):
                    pillar_us[key] += us
                    pillar_pause_us[key] += pause_ns / 1000.0
                    break
            continue
        c = ZCYCLE.search(line)
        if c:
            n_cycles += 1
            max_pause_ns = max(max_pause_ns, int(c.group(4)))

    if n_cycles == 0 or total_us == 0:
        print(f"RED: no ZSTAT cycles or all-zero totals (cycles={n_cycles} total_us={total_us}); "
              "恒0 是装置死了，不是『没有 GC』")
        return 1

    pillar_total = sum(pillar_us.values())
    if pillar_total == 0:
        print("RED: no zphase name matched any pillar regex")
        return 1

    red = False
    print(f"cycles={n_cycles} zphase_lines={n_phases} all_phase_total={total_us/1e6:.2f}s "
          f"max_pause={max_pause_ns/1e6:.3f}ms")
    print(f"{'pillar':<12} {'self-norm%':>10} {'ref%':>6} {'Δ':>6}  pause-share%")
    for key in ("mark", "drain", "copy", "evac_finish", "ref_fix"):
        pct = 100.0 * pillar_us[key] / pillar_total
        ref = REFERENCE[key]
        delta = pct - ref
        pause_share = 100.0 * pillar_pause_us[key] / pillar_us[key] if pillar_us[key] else 0.0
        flag = ""
        if abs(delta) > tolerance:
            flag = "  <-- RED"
            red = True
        print(f"{key:<12} {pct:>10.1f} {ref:>6.1f} {delta:>+6.1f}  {pause_share:>8.1f}{flag}")
    return 1 if red else 0


if __name__ == "__main__":
    sys.exit(main())
