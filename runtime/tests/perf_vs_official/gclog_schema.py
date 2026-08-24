#!/usr/bin/env python3
"""Strict, fail-closed readers for the GCLOG v3 and ZSTAT v1 ledgers."""

from __future__ import annotations

import re
from dataclasses import dataclass, field


U64_MAX = (1 << 64) - 1
TOKEN = r"[A-Za-z0-9._-]+"
PATH = r"[-A-Za-z0-9._>]+"

# Candidate discovery deliberately does not require a numeric version.  A malformed
# ``v=x rec=phase`` line must reach dispatch and fail the exact record fullmatch.
RECORD_TOKENS = re.compile(r"(?:^| )rec=([^ ]+)")

GC_CYCLE = re.compile(
    rf"^\[GCLOG\] v=(\S+) rec=cycle seq=(\S+) kind=({TOKEN}) reason=({TOKEN}) "
    r"start_ns=(\S+) dur_ns=(\S+) live_before=(\S+) live_after=(\S+) collected=(\S+) "
    r"heap_used=(\S+) threshold=(\S+) rss_kb=(\S+)$"
)
GC_PHASE = re.compile(
    rf"^\[GCLOG\] v=(\S+) rec=phase seq=(\S+) name=({TOKEN}) ns=(\S+)$"
)
GC_PHASE_LEAF = re.compile(
    rf"^\[GCLOG\] v=(\S+) rec=phase_leaf seq=(\S+) name=({TOKEN}) ns=(\S+) "
    rf"kind=(pause|conc|unknown) depth=(\S+) path_ok=(\S+) path=({PATH})$"
)
GC_STW = re.compile(
    rf"^\[GCLOG\] v=(\S+) rec=stw seq=(\S+) reason=({TOKEN}) wait_ns=(\S+) held_ns=(\S+)$"
)
ZSTAT_PHASE = re.compile(
    rf"^\[ZSTAT\] v=(\S+) rec=zphase seq=(\S+) name=({TOKEN}) pause_ns=(\S+) "
    r"conc_ns=(\S+) n=(\S+)$"
)
ZSTAT_CYCLE = re.compile(
    r"^\[ZSTAT\] v=(\S+) rec=zcycle seq=(\S+) pause_ns=(\S+) conc_ns=(\S+) "
    r"max_pause_ns=(\S+) phases=(\S+)$"
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
    kind: str
    reason: str
    start_ns: int
    dur_ns: int
    live_before: int
    live_after: int
    collected: int
    heap_used: int
    threshold: int
    rss_kb: int


@dataclass(frozen=True)
class PhaseRecord:
    seq: int
    name: str
    ns: int


@dataclass(frozen=True)
class PhaseLeafRecord:
    seq: int
    name: str
    ns: int
    kind: str
    depth: int
    path_ok: int
    path: str
    components: tuple[str, ...]


@dataclass(frozen=True)
class StwRecord:
    seq: int
    reason: str
    wait_ns: int
    held_ns: int


@dataclass
class GcLogRecords:
    cycles: list[CycleRecord] = field(default_factory=list)
    phases: list[PhaseRecord] = field(default_factory=list)
    phase_leaves: list[PhaseLeafRecord] = field(default_factory=list)
    stw: list[StwRecord] = field(default_factory=list)

    def any(self) -> bool:
        return bool(self.cycles or self.phases or self.phase_leaves or self.stw)


@dataclass(frozen=True)
class ZPhaseRecord:
    seq: int
    name: str
    pause_ns: int
    conc_ns: int
    n: int


@dataclass(frozen=True)
class ZCycleRecord:
    seq: int
    pause_ns: int
    conc_ns: int
    max_pause_ns: int
    phases: int


@dataclass
class ZStatRecords:
    phases: list[ZPhaseRecord] = field(default_factory=list)
    cycles: list[ZCycleRecord] = field(default_factory=list)


def _u64(value: str, field_name: str, line: str) -> int:
    try:
        number = int(value, 10)
    except ValueError as exc:
        raise ValueError(f"non-numeric {field_name} in structured record: {line}") from exc
    if not 0 <= number <= U64_MAX:
        raise ValueError(f"{field_name} outside uint64 range: {number}")
    return number


def _version(value: str, expected: int, family: str, line: str) -> None:
    version = _u64(value, "v", line)
    if version != expected:
        raise ValueError(f"unsupported {family} schema v={version}; expected v={expected}")


def _exact(pattern: re.Pattern[str], line: str, family: str) -> re.Match[str]:
    match = pattern.fullmatch(line)
    if match is None:
        raise ValueError(f"malformed {family} record: {line}")
    return match


def parse_gclog(text: str) -> GcLogRecords:
    """Parse every supported GCLOG record, rejecting any malformed candidate."""
    records = GcLogRecords()
    for line in text.splitlines():
        if not line.startswith("[GCLOG]"):
            continue
        rec_tokens = RECORD_TOKENS.findall(line)
        phase_candidates = [token for token in rec_tokens if token.startswith("phase")]
        if phase_candidates:
            if len(rec_tokens) != 1 or len(phase_candidates) != 1:
                raise ValueError(f"malformed GCLOG phase-family dispatch: {line}")
            family = phase_candidates[0]
            if family == "phase":
                match = _exact(GC_PHASE, line, "GCLOG phase")
                _version(match.group(1), 3, "GCLOG phase", line)
                records.phases.append(PhaseRecord(
                    _u64(match.group(2), "seq", line), match.group(3),
                    _u64(match.group(4), "ns", line)))
                continue
            if family == "phase_leaf":
                match = _exact(GC_PHASE_LEAF, line, "GCLOG phase_leaf")
                _version(match.group(1), 3, "GCLOG phase_leaf", line)
                seq = _u64(match.group(2), "seq", line)
                ns = _u64(match.group(4), "ns", line)
                depth = _u64(match.group(6), "depth", line)
                path_ok = _u64(match.group(7), "path_ok", line)
                components = tuple(match.group(8).split(">"))
                if any(not component or re.fullmatch(TOKEN, component) is None for component in components):
                    raise ValueError(f"invalid phase_leaf path component: {line}")
                if match.group(3) != components[0]:
                    raise ValueError(
                        f"phase_leaf name/path mismatch: name={match.group(3)} path={match.group(8)}")
                if depth != len(components):
                    raise ValueError(
                        f"phase_leaf depth mismatch: depth={depth} components={len(components)}")
                if path_ok not in (0, 1):
                    raise ValueError(f"invalid phase_leaf path_ok={path_ok}")
                if path_ok == 0:
                    raise ValueError(f"phase_leaf path overflow marker: {line}")
                records.phase_leaves.append(PhaseLeafRecord(
                    seq, match.group(3), ns, match.group(5), depth, path_ok,
                    match.group(8), components))
                continue
            raise ValueError(f"unknown GCLOG phase-family record rec={family}")

        if "cycle" in rec_tokens:
            match = _exact(GC_CYCLE, line, "GCLOG cycle")
            _version(match.group(1), 3, "GCLOG cycle", line)
            seq = _u64(match.group(2), "seq", line)
            if seq == 0:
                raise ValueError("GCLOG cycle seq must be greater than zero")
            numbers = [_u64(match.group(index), name, line) for index, name in zip(
                range(5, 12),
                ("start_ns", "dur_ns", "live_before", "live_after", "collected", "heap_used", "threshold"))]
            rss_kb = _u64(match.group(12), "rss_kb", line)
            records.cycles.append(CycleRecord(seq, match.group(3), match.group(4), *numbers, rss_kb))
            continue
        if "stw" in rec_tokens:
            match = _exact(GC_STW, line, "GCLOG stw")
            _version(match.group(1), 3, "GCLOG stw", line)
            records.stw.append(StwRecord(
                _u64(match.group(2), "seq", line), match.group(3),
                _u64(match.group(4), "wait_ns", line), _u64(match.group(5), "held_ns", line)))
    return records


def parse_zstat(text: str) -> ZStatRecords:
    """Parse every ZSTAT v1 candidate with exact field order and uint64 values."""
    records = ZStatRecords()
    for line in text.splitlines():
        if not line.startswith("[ZSTAT]"):
            continue
        rec_tokens = RECORD_TOKENS.findall(line)
        candidates = [token for token in rec_tokens if token.startswith("zphase") or token.startswith("zcycle")]
        if not candidates:
            continue
        if len(rec_tokens) != 1 or len(candidates) != 1:
            raise ValueError(f"malformed ZSTAT dispatch: {line}")
        family = candidates[0]
        if family == "zphase":
            match = _exact(ZSTAT_PHASE, line, "ZSTAT zphase")
            _version(match.group(1), 1, "ZSTAT zphase", line)
            records.phases.append(ZPhaseRecord(
                _u64(match.group(2), "seq", line), match.group(3),
                _u64(match.group(4), "pause_ns", line), _u64(match.group(5), "conc_ns", line),
                _u64(match.group(6), "n", line)))
            continue
        if family == "zcycle":
            match = _exact(ZSTAT_CYCLE, line, "ZSTAT zcycle")
            _version(match.group(1), 1, "ZSTAT zcycle", line)
            records.cycles.append(ZCycleRecord(
                _u64(match.group(2), "seq", line), _u64(match.group(3), "pause_ns", line),
                _u64(match.group(4), "conc_ns", line), _u64(match.group(5), "max_pause_ns", line),
                _u64(match.group(6), "phases", line)))
            continue
        raise ValueError(f"unknown ZSTAT record rec={family}")
    return records


def phase_ns_records(text: str) -> list[tuple[int, str, int]]:
    return [(record.seq, record.name, record.ns) for record in parse_gclog(text).phases]


def pillar_for(path: str) -> str | None:
    """Apply fixed pillar priority to the complete leaf-to-root path."""
    for pillar, pattern in PILLARS:
        if pattern.search(path):
            return pillar
    return None


def build_phase_leaf_ledger(records: GcLogRecords) -> dict[str, object]:
    """Left-join all owned structural leaves to cycles and enforce bound ①."""
    cycles: dict[int, CycleRecord] = {}
    for cycle in records.cycles:
        if cycle.seq in cycles:
            raise ValueError(f"duplicate cycle seq={cycle.seq}")
        cycles[cycle.seq] = cycle
    if not cycles:
        raise ValueError("no rec=cycle records")

    for family, sequence in (("phase", records.phases), ("stw", records.stw)):
        for record in sequence:
            if record.seq > 0 and record.seq not in cycles:
                raise ValueError(f"positive seq orphan: rec={family} seq={record.seq}")

    values = {seq: {name: 0 for name, _pattern in PILLARS} for seq in cycles}
    structural_values = {seq: 0 for seq in cycles}
    owned_structural_records = 0
    matched = 0
    excluded_count = 0
    excluded_ns = 0
    unowned_count = 0
    unowned_ns = 0
    unowned_names: list[str] = []
    for leaf in records.phase_leaves:
        if leaf.seq > 0 and leaf.seq not in cycles:
            raise ValueError(f"positive seq orphan: rec=phase_leaf seq={leaf.seq}")
        pillar = pillar_for(leaf.path)
        if leaf.seq == 0:
            if pillar is not None:
                raise ValueError(f"unowned five-pillar leaf: seq=0 name={leaf.name} pillar={pillar}")
            unowned_count += 1
            unowned_ns += leaf.ns
            unowned_names.append(leaf.name)
            excluded_count += 1
            excluded_ns += leaf.ns
            continue
        structural_values[leaf.seq] += leaf.ns
        owned_structural_records += 1
        if pillar is None:
            excluded_count += 1
            excluded_ns += leaf.ns
            continue
        values[leaf.seq][pillar] += leaf.ns
        matched += 1

    rows: list[dict[str, object]] = []
    for seq, cycle in sorted(cycles.items()):
        pillar_values = values[seq]
        pillar_sum = sum(pillar_values.values())
        structural_sum = structural_values[seq]
        if structural_sum > cycle.dur_ns:
            raise ValueError(
                f"inequality_1 structural leaf sum exceeds cycle: "
                f"seq={seq} sum_ns={structural_sum} dur_ns={cycle.dur_ns}")
        rows.append({
            "seq": seq,
            "pillars_ns": pillar_values,
            "leaf_pillar_ns": pillar_sum,
            "structural_leaf_ns": structural_sum,
            "cycle_dur_ns": cycle.dur_ns,
            "bound_ok": True,
        })
    owned_kinds = {"pause": 0, "conc": 0, "unknown": 0}
    for leaf in records.phase_leaves:
        if leaf.seq > 0:
            owned_kinds[leaf.kind] += 1
    if owned_kinds["unknown"]:
        kind_classification = "unavailable"
    elif owned_structural_records:
        kind_classification = "available"
    else:
        kind_classification = "not_applicable"
    return {
        "matched_leaf_records": matched,
        "structural_leaf_records": len(records.phase_leaves),
        "owned_structural_leaf_records": owned_structural_records,
        "owned_phase_leaf_kind_counts": owned_kinds,
        "phase_leaf_kind_classification": kind_classification,
        "excluded_nonpillar_leaf_records": excluded_count,
        "excluded_nonpillar_leaf_ns": excluded_ns,
        "unowned_nonpillar_leaf_records": unowned_count,
        "unowned_nonpillar_leaf_ns": unowned_ns,
        "unowned_nonpillar_names": sorted(unowned_names),
        "cycles": rows,
    }


def phase_leaf_ledger(text: str) -> dict[str, object]:
    return build_phase_leaf_ledger(parse_gclog(text))
