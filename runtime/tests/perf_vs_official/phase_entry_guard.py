#!/usr/bin/env python3
"""Validate generation spans emitted at ZGC-shaped collection scope exits.

#954 removes the legacy driver cycle and major.* records. The generation
scope still reports each completed span (ZGC zStat.cpp:711-759); the same
request id, ordered preclean/roots/old spans and enclosed young entries are
checked here without depending on the removed driver logging wrapper.
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
if not records.generations:
    errors.append("generation=0")
if not records.stw:
    errors.append("stw=0")
if not records.phases:
    errors.append("phase=0")
if records.phases and not any(0 < record.ns < 1000 for record in records.phases):
    errors.append("sub_microsecond_phase=0")

marker = "PHASE_ENTRY_MINOR_OK checksum=" if mode == "minor" else "PHASE_ENTRY_MAJOR_OK checksum="
if marker not in text:
    errors.append("completion=0")
minor_spans = [r for r in records.generations if r.gc_tag == "y"]
if mode == "minor":
    if not minor_spans:
        errors.append("minor_generation=0")
    for span in minor_spans:
        if not any(record.seq == span.seq and record.gc_tag == "y" and
                   record.name == "Pause_Mark_Start" and
                   span.start_ns <= record.start_ns and
                   record.start_ns + record.ns <= span.start_ns + span.dur_ns
                   for record in records.phases):
            errors.append(f"minor_entry_missing_seq={span.seq}")
else:
    # ZGC zDriver.cpp:416-449: preclean, roots, old share one driver GC id.
    spans = records.generations
    ids = {span.seq for span in spans}
    if len(ids) != 1:
        errors.append("major_request_ids=" + ",".join(map(str, sorted(ids))))
    seq = spans[0].seq if spans else 0
    names = ["Young_Generation__Promote_All_", "Young_Generation__Collect_Roots_", "Old_Generation"]
    if [span.name for span in spans] != names:
        errors.append(f"major_spans_seq={seq}")
    elif [span.gc_tag for span in spans] != ["Y", "Y", "O"]:
        errors.append(f"major_span_tags_seq={seq}")
    else:
        if any(left.start_ns + left.dur_ns > right.start_ns
               for left, right in zip(spans, spans[1:])):
            errors.append(f"major_span_order_seq={seq}")
        # ZGC zGeneration.cpp:78-79: preclean enters the young pause;
        # full roots enters the combined young/old mark-start pause.
        for span, label, entry in zip(spans[:2], ("major.preclean", "major.full_roots"),
                                      ("Pause_Mark_Start", "Pause_Mark_Start__Major_")):
            if not any(record.seq == span.seq and record.gc_tag == "Y" and
                       record.name == entry and
                       span.start_ns <= record.start_ns and
                       record.start_ns + record.ns <= span.start_ns + span.dur_ns
                       for record in records.phases):
                errors.append(f"major_entry_missing_seq={span.seq}_span={label}")

print(
    f"SCHEMA_LEDGER_GUARD mode={mode} generations={len(records.generations)} stw={len(records.stw)} "
    f"phase={len(records.phases)} errors={','.join(errors) if errors else 'none'}"
)
raise SystemExit(1 if errors else 0)
