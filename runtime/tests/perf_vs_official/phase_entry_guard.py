#!/usr/bin/env python3
"""Validate the phase-entry runner logs, including per-cycle young entry evidence.

P15: rec=phase_leaf and the Timer leaf-path tree were deleted (no ZGC
counterpart); the guard now reads only rec=cycle / rec=phase / rec=stw.
The timer contract mode went away with the mechanism it validated.
"""

import sys
from pathlib import Path

mode, log_path = sys.argv[1], Path(sys.argv[2])
if mode not in ("minor", "major"):
    print(f"SCHEMA_LEDGER_GUARD mode={mode} errors=unknown_mode")
    raise SystemExit(1)
from gclog_schema import parse_gclog

text = log_path.read_text(encoding="utf-8", errors="replace")
records = parse_gclog(text)
errors = []
if not records.cycles:
    errors.append("cycle=0")
if not records.stw:
    errors.append("stw=0")
if not records.phases:
    errors.append("phase=0")
if records.phases and not any(0 < record.ns < 1000 for record in records.phases):
    errors.append("sub_microsecond_phase=0")

marker = "PHASE_ENTRY_MINOR_OK checksum=" if mode == "minor" else "PHASE_ENTRY_MAJOR_OK checksum="
if marker not in text:
    errors.append("completion=0")
if not any(record.kind == mode for record in records.cycles):
    errors.append(f"{mode}_cycle=0")
# Cycle kind alone cannot prove that the young entry ran. Major preludes
# now belong to the major request's id, not a separate minor cycle.
young_entries = {record.seq for record in records.phases
                 if record.name == "young.flush_alloc"}
for cycle in records.cycles:
    if cycle.kind == "minor" and cycle.seq not in young_entries:
        errors.append(f"minor_entry_missing_seq={cycle.seq}")
if mode == "major":
    # ZGC zDriver.cpp:384-451: a user/full request owns two young spans
    # (preclean, full roots), then old collection, under ONE request id.
    cycle_kinds = [record.kind for record in records.cycles]
    if cycle_kinds != ["major"]:
        errors.append("major_explicit_shape=" + ",".join(cycle_kinds))
    for cycle in records.cycles:
        if cycle.kind != "major":
            continue
        names = ("major.preclean", "major.full_roots", "major.old")
        spans = [record for record in records.phases
                 if record.seq == cycle.seq and record.name.startswith("major.")]
        if [record.name for record in spans] != list(names):
            errors.append(f"major_spans_seq={cycle.seq}")
            continue
        if [record.gc_tag for record in spans] != ["Y", "Y", "O"]:
            errors.append(f"major_span_tags_seq={cycle.seq}")
        if (spans[0].start_ns < cycle.start_ns or
                any(left.start_ns + left.ns > right.start_ns
                    for left, right in zip(spans, spans[1:])) or
                spans[-1].start_ns + spans[-1].ns > cycle.start_ns + cycle.dur_ns):
            errors.append(f"major_span_order_seq={cycle.seq}")
        for span in spans[:2]:
            if not any(record.seq == cycle.seq and record.gc_tag == "Y" and
                       record.name == "young.flush_alloc" and
                       span.start_ns <= record.start_ns and
                       record.start_ns + record.ns <= span.start_ns + span.ns
                       for record in records.phases):
                errors.append(f"major_entry_missing_seq={cycle.seq}_span={span.name}")

print(
    f"SCHEMA_LEDGER_GUARD mode={mode} cycles={len(records.cycles)} stw={len(records.stw)} "
    f"phase={len(records.phases)} errors={','.join(errors) if errors else 'none'}"
)
raise SystemExit(1 if errors else 0)
