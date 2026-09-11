#!/usr/bin/env python3
"""Validate the phase-entry runner logs, including per-cycle young entry evidence."""

import sys
from pathlib import Path

mode, log_path = sys.argv[1], Path(sys.argv[2])
from gclog_schema import phase_leaf_ledger, parse_gclog

text = log_path.read_text(encoding="utf-8", errors="replace")
records = parse_gclog(text)
ledger = phase_leaf_ledger(text)
errors = []
if not records.cycles:
    errors.append("cycle=0")
if not records.stw:
    errors.append("stw=0")
if not records.phases:
    errors.append("phase=0")
if not records.phase_leaves:
    errors.append("phase_leaf=0")
if records.phases and not any(0 < record.ns < 1000 for record in records.phases):
    errors.append("sub_microsecond_phase=0")

if mode in ("minor", "major"):
    marker = "PHASE_ENTRY_MINOR_OK checksum=" if mode == "minor" else "PHASE_ENTRY_MAJOR_OK checksum="
    if marker not in text:
        errors.append("completion=0")
    if not any(record.kind == mode for record in records.cycles):
        errors.append(f"{mode}_cycle=0")
    # Cycle kind is emitted by the outer collector even when its young call
    # is absent. Require an internal phase for EACH minor sequence, including
    # the owned young prelude in major mode. This timer precedes the empty
    # candidate early return (Generation.cpp, DoYoungGarbageCollection).
    young_entries = {record.seq for record in records.phases
                     if record.name == "young.flush_alloc"}
    for cycle in records.cycles:
        if cycle.kind == "minor" and cycle.seq not in young_entries:
            errors.append(f"minor_entry_missing_seq={cycle.seq}")
    if mode == "major":
        # The source stays below the automatic-minor waterline and makes one
        # explicit heavy request. ZGC's major shape is one owned young prelude
        # followed immediately by old collection.
        cycle_kinds = [record.kind for record in records.cycles]
        if cycle_kinds != ["minor", "major"]:
            errors.append("major_explicit_shape=" + ",".join(cycle_kinds))
    if records.phase_leaves and not any(">" in record.path for record in records.phase_leaves):
        errors.append("nested_leaf_path=0")
else:
    if "TIMER_LEDGER_CONTRACT_OK" not in text:
        errors.append("completion=0")
    leaves = {record.name: record for record in records.phase_leaves}
    if "contract.root" in leaves or "contract.middle" in leaves:
        errors.append("parent_emitted_as_leaf")
    deep = leaves.get("contract.deep")
    if deep is None or deep.path != "contract.deep>contract.middle>contract.root":
        errors.append("deep_leaf_path")
    captured = leaves.get("cycle.captured")
    if captured is None or captured.seq == 0 or captured.seq not in {cycle.seq for cycle in records.cycles}:
        errors.append("cycle_captured_at_construction")
    finalizer = leaves.get("Finalizer")
    if finalizer is None or finalizer.seq != 0:
        errors.append("unowned_finalizer_seq")
    if "Finalizer" not in ledger["unowned_nonpillar_names"]:
        errors.append("unowned_nonpillar_exclusion")
    external = leaves.get("contract.external")
    if external is None or external.seq != 0:
        errors.append("active_cycle_external_seq")
    internal = leaves.get("young.flush_alloc")
    if internal is None or internal.seq == 0 or internal.seq not in {cycle.seq for cycle in records.cycles}:
        errors.append("active_cycle_internal_seq")
    if "contract.external" not in ledger["unowned_nonpillar_names"]:
        errors.append("active_cycle_external_unowned")
    owned_row = next((row for row in ledger["cycles"] if internal is not None and
                      row["seq"] == internal.seq), None)
    if owned_row is None or owned_row["structural_leaf_ns"] != internal.ns:
        errors.append("active_cycle_internal_bound")

print(
    f"SCHEMA_LEDGER_GUARD mode={mode} cycles={len(records.cycles)} stw={len(records.stw)} "
    f"phase={len(records.phases)} phase_leaf={len(records.phase_leaves)} "
    f"ledger_cycles={len(ledger['cycles'])} errors={','.join(errors) if errors else 'none'}"
)
raise SystemExit(1 if errors else 0)
