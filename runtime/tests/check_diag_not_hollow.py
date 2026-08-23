#!/usr/bin/env python3
"""A diagnostic whose header documents a gate must not have an all-empty implementation.

34 of the subsystems under src/Heap/Verify/ were reduced to `return false;` and empty
bodies while three things were left completely intact: the header's arm-by-arm contract,
its gate name (MRT_GCV2_TOVERFAIL, MRT_GCV2_PERMWHO_ADMIT, ...), and every call site in
the product code.  ToverFailDiag.h even still carries the sentence "Positive controls sit
next to the signature counters so a zero cannot mean 'probe dead'".

That combination is worse than deleting them.  Reading the header tells you the instrument
exists and how to switch it on; switching it on produces zero lines; and zero lines read as
"that arm never fires".  Every number taken from a hollowed subsystem is a false negative,
and the ledgers are full of "turn on gate X and measure" plans that cannot work.  This was
found on 2026-08-18 one step before it was acted on: a run with MRT_GCV2_TOVERFAIL=1 was
already queued to decide whether the unmovable-skip arm hands out from-versions.

Deleting a diagnostic is fine.  Keeping a diagnostic is fine.  Keeping the *documentation*
of one whose body is gone is not, so this fails the build until the header says so.

Fix either way:
  - restore the sink (see PermWhoAdmit.cpp for the shape: compile-time constant gate, plus a
    line on the zero case so a zero cannot be read as a dead probe), or
  - put HOLLOWED in the header, which records that the contract below it is not running.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
VERIFY = ROOT / "src/Heap/Verify"

# A body that is empty or a bare `return <literal>;` carries no behaviour.
EMPTY_BODY = re.compile(r"\)\s*(?:const\s*)?\{\s*(?:return\s+(?:false|true|0|nullptr)\s*;\s*)?\}")
# Definitions only: a leading type at column 0, which is how these files are written.
DEFINITION = re.compile(r"^(?:void|bool|size_t|uint\d+_t|unsigned|int|const\s+char\*)\s+\w+\s*\(", re.M)
GATE = re.compile(r"MRT_[A-Z0-9_]+")
HOLLOW_MARK = "HOLLOWED"


def main() -> int:
    if not VERIFY.is_dir():
        print(f"DIAG_HOLLOW_GUARD SKIP no {VERIFY}")
        return 0

    offenders = []
    checked = 0
    for src in sorted(VERIFY.glob("*.cpp")):
        header = src.with_suffix(".h")
        if not header.exists():
            continue
        header_text = header.read_text(errors="replace")
        # Only subsystems that advertise a gate: those are the ones someone will try to switch on.
        if not GATE.search(header_text):
            continue
        checked += 1
        if HOLLOW_MARK in header_text:
            continue
        body = src.read_text(errors="replace")
        definitions = len(DEFINITION.findall(body))
        if definitions == 0:
            continue
        empties = len(EMPTY_BODY.findall(body))
        if empties >= definitions:
            offenders.append((src.name, header.name, definitions, empties))

    if offenders:
        print("DIAG_HOLLOW_GUARD FAIL: header documents a gate but the implementation is all no-ops")
        for name, hdr, definitions, empties in offenders:
            print(f"  {name}: {empties}/{definitions} bodies empty, and {hdr} still documents its gate")
        print("  Restore the sink, or write HOLLOWED in the header so the contract is not read as live.")
        return 1

    print(f"DIAG_HOLLOW_GUARD PASS gated_subsystems={checked}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
