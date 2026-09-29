#!/usr/bin/env python3
"""Exact readers for the current GCLOG producer; completed-run validation is explicit.

Streaming/entry observers can parse an unfinished ledger. Measurement consumers must
call validate_complete() before using its values.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

U64_MAX = (1 << 64) - 1
TOKEN = r"[A-Za-z0-9._-]+"
RECORD_TOKENS = re.compile(r"(?:^| )rec=([^ ]+)")
GC_CYCLE = re.compile(
    rf"\[GCLOG\] v=(\S+) rec=cycle seq=(\S+) gc_tag=(-) name=({TOKEN}) cause=({TOKEN}) "
    r"event=(start|abort|end)(?: start_ns=(\S+) dur_ns=(\S+) used_at_start=(\S+) used_at_end=(\S+))?"
)
GC_GENERATION = re.compile(
    rf"\[GCLOG\] v=(\S+) rec=generation seq=(\S+) gc_tag=([yYO]) name=({TOKEN}) "
    r"event=(start|abort|end)(?: start_ns=(\S+) dur_ns=(\S+) "
    r"used_at_collection_start=(\S+) used_at_collection_end=(\S+))?"
)
GC_PHASE = re.compile(
    rf"\[GCLOG\] v=(\S+) rec=phase seq=(\S+) gc_tag=([yYO-]) name=({TOKEN}) "
    r"kind=(pause|conc|subphase|critical) start_ns=(\S+) ns=(\S+)"
)
GC_STW = re.compile(
    rf"\[GCLOG\] v=(\S+) rec=stw seq=(\S+) gc_tag=([yYO-]) reason=({TOKEN}) "
    r"start_ns=(\S+) wait_ns=(\S+) held_ns=(\S+)"
)
PILLARS = (
    ("ref_fix", re.compile(r"ref.?fix|fix.?ref|FixRef|ref_fix", re.I)),
    ("mark", re.compile(r"mark", re.I)),
    ("evac_finish", re.compile(r"evac_finish|evac.?finish", re.I)),
    ("drain", re.compile(r"drain|remset", re.I)),
    ("copy", re.compile(r"copy|reloc|evac(?!_finish)", re.I)),
)

@dataclass(frozen=True)
class CycleRecord:
    seq: int
    gc_tag: str
    name: str
    cause: str
    event: str
    start_ns: int | None = None
    dur_ns: int | None = None
    used_at_start: int | None = None
    used_at_end: int | None = None

    @property
    def kind(self):
        return {"Minor_Collection": "minor", "Major_Collection": "major"}[self.name]

@dataclass(frozen=True)
class GenerationRecord:
    seq: int
    gc_tag: str
    name: str
    event: str
    start_ns: int | None = None
    dur_ns: int | None = None
    used_at_collection_start: int | None = None
    used_at_collection_end: int | None = None

@dataclass(frozen=True)
class PhaseRecord:
    seq: int
    gc_tag: str
    name: str
    kind: str
    start_ns: int
    ns: int

@dataclass(frozen=True)
class StwRecord:
    seq: int
    gc_tag: str
    reason: str
    start_ns: int
    wait_ns: int
    held_ns: int

# ZGC zGeneration.cpp:538-581,1015-1070: only unconditional collect phases.
# Continue is conditional on mark-end retries; verify is configuration-dependent.
COMMON_PHASES = {
    "Concurrent_Mark": "conc", "Pause_Mark_End": "pause",
    "Concurrent_Mark_Free": "conc", "Concurrent_Reset_Relocation_Set": "conc",
    "Concurrent_Select_Relocation_Set": "conc", "Pause_Relocate_Start": "pause",
    "Concurrent_Relocate": "conc",
}
YOUNG_NAMES = {"Young_Generation", "Young_Generation__Promote_All_", "Young_Generation__Collect_Roots_"}

@dataclass
class GcLogRecords:
    cycles: list[CycleRecord] = field(default_factory=list)
    generations: list[GenerationRecord] = field(default_factory=list)
    phases: list[PhaseRecord] = field(default_factory=list)
    stw: list[StwRecord] = field(default_factory=list)
    events: list = field(default_factory=list, repr=False)
    aborted: int = 0
    truncated: int = 0

    def any(self):
        return bool(self.events)

    def validate_complete(self):
        """Validate a process-exit ledger, retaining abort/truncation separately."""
        self.aborted = self.truncated = 0
        starts = [r.seq for r in self.cycles if r.event == "start"]
        last = starts[-1] if starts else None
        collections = {}
        active_generations = {}
        ended_generations = {}
        for record in self.events:
            seq = record.seq
            if isinstance(record, CycleRecord):
                if record.event == "start":
                    if seq in collections:
                        raise ValueError(f"seq={seq} duplicate collection start")
                    collections[seq] = [record, None]
                    continue
                if seq not in collections:
                    raise ValueError(f"seq={seq} missing collection start")
                start, terminal = collections[seq]
                if terminal is not None or (start.name, start.cause) != (record.name, record.cause):
                    raise ValueError(f"seq={seq} mismatched/duplicate collection {record.event}")
                if record.event == "end" and any(k[0] == seq for k in active_generations):
                    raise ValueError(f"seq={seq} missing generation end")
                if record.event == "end":
                    required_tags = {"y"} if start.kind == "minor" else {"Y", "O"}
                    missing_tags = required_tags - ended_generations.get(seq, set())
                    if missing_tags:
                        raise ValueError(f"seq={seq} missing generation end tags={','.join(sorted(missing_tags))}")
                collections[seq][1] = record
                self.aborted += record.event == "abort"
                continue
            # Critical waits can run on a mutator without an assigned GC id.
            if isinstance(record, PhaseRecord) and record.kind == "critical" and seq == 0:
                continue
            if seq not in collections:
                raise ValueError(f"seq={seq} missing collection start")
            if collections[seq][1] is not None:
                raise ValueError(f"seq={seq} record after collection terminal")
            key = (seq, record.gc_tag)
            if isinstance(record, GenerationRecord):
                if record.event == "start":
                    if (record.gc_tag == "O") != (record.name == "Old_Generation"):
                        raise ValueError(f"seq={seq} generation={record.name} mismatched generation tag {record.gc_tag}")
                    if key in active_generations:
                        raise ValueError(f"seq={seq} generation={record.name} missing generation end")
                    active_generations[key] = (record, [])
                    continue
                if key not in active_generations:
                    raise ValueError(f"seq={seq} generation={record.name} missing generation start")
                start, phases = active_generations.pop(key)
                if start.name != record.name:
                    raise ValueError(f"seq={seq} generation={record.name} mismatched generation start")
                if record.event == "abort":
                    continue
                ended_generations.setdefault(seq, set()).add(record.gc_tag)
                required = dict(COMMON_PHASES)
                if record.gc_tag == "O":
                    required.update(Concurrent_Process_Non_Strong="conc", Concurrent_Remap_Roots="conc")
                else:
                    name = "Pause_Mark_Start__Major_" if record.gc_tag == "Y" and record.name != "Young_Generation__Promote_All_" else "Pause_Mark_Start"
                    required[name] = "pause"
                observed = {(p.name, p.kind) for p in phases
                            if record.start_ns <= p.start_ns and p.start_ns + p.ns <= record.start_ns + record.dur_ns}
                for name, kind in required.items():
                    if (name, kind) not in observed:
                        raise ValueError(f"seq={seq} generation={record.name} missing phase {name}")
            elif isinstance(record, PhaseRecord) and key in active_generations:
                active_generations[key][1].append(record)
        for seq, (_, terminal) in collections.items():
            if terminal is None:
                if seq != last:
                    raise ValueError(f"seq={seq} missing collection end")
                self.truncated += 1
        return self


def _u64(value, field_name, line):
    if re.fullmatch(r"[0-9]+", value) is None:
        raise ValueError(f"non-numeric {field_name} in structured record: {line}")
    number = int(value)
    if number > U64_MAX:
        raise ValueError(f"{field_name} outside uint64 range: {number}")
    return number


def parse_gclog(text: str) -> GcLogRecords:
    records = GcLogRecords()
    patterns = {"cycle": GC_CYCLE, "generation": GC_GENERATION, "phase": GC_PHASE, "stw": GC_STW}
    for line in text.splitlines():
        if not line.startswith("[GCLOG]"):
            continue
        tokens = RECORD_TOKENS.findall(line)
        if tokens == ["crash"]:  # independently versioned crash signature, not a GC ledger event
            continue
        if len(tokens) != 1 or tokens[0] not in patterns:
            raise ValueError(f"unknown or malformed GCLOG dispatch: {line}")
        family = tokens[0]
        match = patterns[family].fullmatch(line)
        if match is None:
            raise ValueError(f"malformed GCLOG {family} record: {line}")
        fields = list(match.groups())
        version = _u64(fields.pop(0), "v", line)
        expected = 6 if family in ("cycle", "generation") else 5
        if version != expected:
            raise ValueError(f"unsupported GCLOG {family} schema v={version}; expected v={expected}")
        fields[0] = _u64(fields[0], "seq", line)
        if family in ("cycle", "generation"):
            if fields[0] == 0:
                raise ValueError(f"GCLOG {family} seq must be greater than zero")
            event_index = 4 if family == "cycle" else 3
            event = fields[event_index]
            values = fields[event_index + 1:]
            if (event == "end") != (values[0] is not None):
                raise ValueError(f"malformed GCLOG {family} {event} fields: {line}")
            if event == "end":
                fields[event_index + 1:] = [_u64(v, "duration/used", line) for v in values]
            if family == "cycle":
                if fields[2] not in ("Minor_Collection", "Major_Collection"):
                    raise ValueError(f"unknown collection name: {fields[2]}")
                record = CycleRecord(*fields)
                records.cycles.append(record)
            else:
                if fields[2] not in YOUNG_NAMES | {"Old_Generation"}:
                    raise ValueError(f"unknown generation name/tag: {fields[1:3]}")
                record = GenerationRecord(*fields)
                records.generations.append(record)
        elif family == "phase":
            fields[4:] = [_u64(v, "phase duration", line) for v in fields[4:]]
            record = PhaseRecord(*fields)
            records.phases.append(record)
        else:
            fields[3:] = [_u64(v, "stw duration", line) for v in fields[3:]]
            record = StwRecord(*fields)
            records.stw.append(record)
        records.events.append(record)
    return records


def phase_ns_records(text):
    return [(r.seq, r.name, r.ns) for r in parse_gclog(text).phases]


def pillar_for(path):
    for pillar, pattern in PILLARS:
        if pattern.search(path):
            return pillar
    return None
