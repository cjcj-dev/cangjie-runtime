#!/usr/bin/env python3
"""Every GCPhase must map to a name built from its own enumerator.

The positional table this guard replaced kept names from an older phase enum, so
GC_PHASE_POST_TRACE printed "forward phase", GC_PHASE_PREFORWARD printed "enum
fix phase" and GC_PHASE_FORWARD printed "trace fix phase". Nothing failed: the
lookup was in bounds and the names were plausible phase names, so crash reports
carried the wrong phase and reports could not be compared against each other.

A positional table cannot state which value a name belongs to. This checks the
property the table could not: for every enumerator, the returned name is that
enumerator's own words.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/Heap/Collector/Collector.h"
SOURCE = ROOT / "src/Heap/Collector/Collector.cpp"


def enumerators() -> dict[str, int]:
    text = HEADER.read_text(encoding="utf-8")
    block = re.search(r"enum GCPhase\s*:\s*uint8_t\s*\{(.*?)\}", text, re.S)
    if not block:
        sys.exit("GC_PHASE_NAMES FAIL: enum GCPhase not found in Collector.h")
    found = re.findall(r"(GC_PHASE_[A-Z_]+)\s*=\s*(\d+)", block.group(1))
    if not found:
        sys.exit("GC_PHASE_NAMES FAIL: enum GCPhase has no enumerators")
    return {name: int(value) for name, value in found}


def arms() -> dict[str, str]:
    text = SOURCE.read_text(encoding="utf-8")
    body = re.search(r"const char\* Collector::GetGCPhaseName\(GCPhase phase\)\s*\{(.*?)\n\}",
                     text, re.S)
    if not body:
        sys.exit("GC_PHASE_NAMES FAIL: GetGCPhaseName not found in Collector.cpp")
    if "phaseNames[" in body.group(1):
        sys.exit("GC_PHASE_NAMES FAIL: GetGCPhaseName went back to a positional table; "
                 "a table cannot say which value a name belongs to")
    return dict(re.findall(r"case\s+(GC_PHASE_[A-Z_]+)\s*:\s*return\s+\"([^\"]+)\"", body.group(1)))


def main() -> None:
    values = enumerators()
    mapping = arms()
    failures = []

    # Check the containment that catches the real defect, which was a name
    # belonging to a *different* value, not a name that abbreviates its own.
    # "reclaim satb phase" for GC_PHASE_RECLAIM_SATB_NODE drops a word and is
    # fine; "trace fix phase" for GC_PHASE_FORWARD shares none and is not.
    for name in values:
        if name not in mapping:
            failures.append(f"{name} has no case arm")
            continue
        own = {w for w in name.removeprefix("GC_PHASE_").lower().split("_") if w}
        said = {w for w in mapping[name].lower().split() if w and w != "phase"}
        # "undefined" spells out UNDEF, so allow either to extend the other.
        # "forward" and "preforward" extend neither, which is the pair that
        # went wrong, so this stays strict where it matters.
        foreign = sorted(w for w in said
                         if not any(w.startswith(o) or o.startswith(w) for o in own))
        if foreign or not said:
            failures.append(f"{name} (={values[name]}) returns {mapping[name]!r}; "
                            f"{'says ' + str(foreign) + ' which is not its own' if foreign else 'says nothing'}")

    seen: dict[str, str] = {}
    for name, text in mapping.items():
        if text in seen:
            failures.append(f"{name} and {seen[text]} both return {text!r}")
        seen[text] = name

    for name in mapping:
        if name not in values:
            failures.append(f"{name} has a case arm but is not an enumerator")

    if failures:
        print("GC_PHASE_NAMES FAIL")
        for line in failures:
            print(f"  {line}")
        sys.exit(1)

    print(f"GC_PHASE_NAMES PASS enumerators={len(values)} arms={len(mapping)}")


if __name__ == "__main__":
    main()
