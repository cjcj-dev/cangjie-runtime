#!/usr/bin/env python3
"""Check pause/concurrent ownership using the common structured-log parser."""

from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "runtime/tests/perf_vs_official"))

from gclog_schema import PILLARS, parse_gclog, parse_zstat, pillar_for  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    text = args.log.read_text(encoding="utf-8", errors="replace")
    try:
        gclog = parse_gclog(text)
        zstat = parse_zstat(text)
    except ValueError as exc:
        print(f"PHASE_KIND_ACCOUNTING_FAIL schema={exc}")
        return 1

    totals: dict[str, list[int]] = defaultdict(lambda: [0, 0, 0])
    for record in zstat.phases:
        phase = totals[record.name]
        phase[0] += record.pause_ns
        phase[1] += record.conc_ns
        phase[2] += record.n

    failures: list[str] = []

    def require(name: str, condition: bool, detail: str) -> None:
        state = "PASS" if condition else "FAIL"
        print(f"CHECK {name} {state}: {detail}")
        if not condition:
            failures.append(name)

    require(
        "positive_control",
        bool(zstat.phases and gclog.stw and gclog.cycles and gclog.phases),
        f"zphase_lines={len(zstat.phases)} stw_lines={len(gclog.stw)} "
        f"cycle_lines={len(gclog.cycles)} phase_lines={len(gclog.phases)}",
    )

    relocate = totals["young.concurrent_relocate"]
    require(
        "concurrent_relocate_kind",
        relocate[0] == 0 and relocate[1] > 0 and relocate[2] > 0,
        f"pause_ns={relocate[0]} conc_ns={relocate[1]} n={relocate[2]}",
    )

    copy = totals["young.copy"]
    require(
        "copy_kind_unchanged",
        copy[0] == 0 and copy[1] > 0 and copy[2] > 0,
        f"pause_ns={copy[0]} conc_ns={copy[1]} n={copy[2]}",
    )

    bucket = {name: [0, 0, 0] for name, _pattern in PILLARS}
    for name, values in totals.items():
        key = pillar_for(name)
        if key is not None:
            for index, value in enumerate(values):
                bucket[key][index] += value

    ref_fix = bucket["ref_fix"]
    drain = bucket["drain"]
    require(
        "ref_fix_pause_share",
        ref_fix[0] > 0 and ref_fix[1] == 0,
        f"pause_ns={ref_fix[0]} conc_ns={ref_fix[1]} n={ref_fix[2]}",
    )
    require(
        "drain_pause_share",
        drain[0] > 0 and drain[1] == 0,
        f"pause_ns={drain[0]} conc_ns={drain[1]} n={drain[2]}",
    )

    print(
        "ACCOUNTING "
        f"phase_ns={sum(record.ns for record in gclog.phases)} "
        f"zpause_ns={sum(values[0] for values in totals.values())} "
        f"zconc_ns={sum(values[1] for values in totals.values())} "
        f"held_ns={sum(record.held_ns for record in gclog.stw)} "
        f"cycle_ns={sum(record.dur_ns for record in gclog.cycles)}"
    )
    print(f"PHASE_KIND_ACCOUNTING_RESULT checks=5 failed={len(failures)}")
    if failures:
        print("PHASE_KIND_ACCOUNTING_FAIL " + ",".join(failures))
        return 1
    print("PHASE_KIND_ACCOUNTING_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
