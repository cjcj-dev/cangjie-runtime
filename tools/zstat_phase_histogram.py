#!/usr/bin/env python3
"""Validate ZStat phase records from a completed, unmodified managed workload.

Use the same ELF with candidate/cut/restored product libraries. Every assertion
is evaluated independently, so an earlier failure cannot hide the target check.
Durations are observations, not performance admission thresholds.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re

PAUSES = {"Pause_Mark_Start", "Pause_Mark_Start__Major_", "Pause_Mark_End", "Pause_Relocate_Start"}
CONCURRENT = {
    "Concurrent_Mark", "Concurrent_Mark_Continue", "Concurrent_Mark_Free",
    "Concurrent_Reset_Relocation_Set", "Concurrent_Select_Relocation_Set",
    "Concurrent_Relocate", "Concurrent_Process_Non-Strong", "Concurrent_Remap_Roots",
}
START_CHILDREN = {"young.flush_alloc", "young.prepare_candidates", "young.remset_drain"}


def analyze(stderr):
    rows = []
    for line in stderr.splitlines():
        if "[GCLOG]" in line:
            row = dict(re.findall(r"(\w+)=([^\s]+)", line))
            if row.get("rec") in {"stw", "phase", "cycle", "generation"}:
                rows.append(row)
    stws = [r for r in rows if r["rec"] == "stw" and r.get("reason") != "Verify_Old"]
    phases = [r for r in rows if r["rec"] == "phase"]
    generations = [r for r in rows if r["rec"] == "generation"]
    checks = {}
    def check(name, observed, failures):
        checks[name] = {"observed": observed, "failures": failures,
                        "pass": observed > 0 and not failures}
    check("VerifyDisabled", len([r for r in rows if r["rec"] == "stw"]),
          [r for r in rows if r["rec"] == "stw" and r.get("reason") == "Verify_Old"])
    check("ThreePhaseNames", len(stws), [r for r in stws if r.get("reason") not in PAUSES])
    named_pauses = [r for r in phases if r.get("name") in PAUSES]
    check("PauseKind", len(named_pauses), [r for r in named_pauses if r.get("kind") != "pause"])
    # Keep named phases in pairing even when misclassified (PauseKind diagnoses
    # those), and include every actual pause regardless of its name. Otherwise
    # an extra pause with a subphase name can escape the reverse pairing check.
    pause_rows = [r for r in phases
                  if r.get("name") in PAUSES or r.get("kind") == "pause"]
    pair_failures = []
    matched = Counter()
    for stw in stws:
        start = int(stw["start_ns"])
        end = start + int(stw["wait_ns"]) + int(stw["held_ns"])
        found = [i for i, p in enumerate(pause_rows)
                 if p.get("seq") == stw.get("seq") and p.get("gc_tag") == stw.get("gc_tag")
                 and p.get("name") == stw.get("reason")
                 and start <= int(p["start_ns"]) <= int(p["start_ns"]) + int(p["ns"]) <= end]
        if len(found) != 1:
            pair_failures.append({"stw": stw, "matches": len(found)})
        matched.update(found)
    pair_failures.extend({"phase": p, "matches": matched[i]} for i, p in enumerate(pause_rows)
                         if matched[i] != 1)
    check("PausePairing", len(stws), pair_failures)
    children = [r for r in phases if r.get("name") in START_CHILDREN]
    check("SubphaseKind", len(children), [r for r in children if r.get("kind") != "subphase"])
    concurrent = [r for r in phases if r.get("name") in CONCURRENT]
    check("ConcurrentKind", len(concurrent), [r for r in concurrent if r.get("kind") != "conc"])
    check("GenerationTag", len(generations), [r for r in generations
          if r.get("gc_tag") not in ({"y", "Y"} if r.get("name") == "Young_Generation" else {"O"})])
    joins = []
    for generation in generations:
        start = int(generation["start_ns"])
        end = start + int(generation["dur_ns"])
        linked = [r for r in stws if r["seq"] == generation["seq"] and r["gc_tag"] == generation["gc_tag"]
                  and start <= int(r["start_ns"]) < end]
        if not linked:
            joins.append({"generation": generation, "linked_stws": 0})
    check("GenerationJoin", len(generations), joins)
    groups = defaultdict(list)
    for stw in stws:
        groups[(stw["seq"], stw["gc_tag"])].append(stw)
    # A major can contain several young collections under one seq/Y. Delimit
    # complete young cycles by relocate-start rather than grouping by seq alone.
    complete = []
    incomplete = []
    order_failures = []
    for (seq, tag), group in groups.items():
        if tag not in {"y", "Y"}:
            continue
        current = []
        for stw in sorted(group, key=lambda r: int(r["start_ns"])):
            current.append(stw)
            if stw.get("reason") == "Pause_Relocate_Start":
                names = [r["reason"] for r in current]
                good = (len(names) >= 3 and names[0] in {"Pause_Mark_Start", "Pause_Mark_Start__Major_"}
                        and all(n == "Pause_Mark_End" for n in names[1:-1]))
                if not good:
                    order_failures.append({"seq": seq, "tag": tag, "names": names})
                complete.append(current)
                current = []
        if current:
            incomplete.append({"seq": seq, "tag": tag, "names": [r["reason"] for r in current]})
    check("YoungPhaseOrder", len(complete), order_failures)
    return {"checks": checks, "records": dict(Counter(r["rec"] for r in rows)),
            "stw_names": dict(Counter(r.get("reason") for r in stws)),
            "young_complete_stw_histogram": dict(Counter(len(c) for c in complete)),
            "young_incomplete": incomplete,
            "held_ns_by_phase": {name: sorted(int(r["held_ns"]) for r in stws if r["reason"] == name)
                                 for name in sorted({r["reason"] for r in stws})}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stderr", type=Path)
    parser.add_argument("--rc-file", required=True, type=Path)
    parser.add_argument("--stdout", required=True, type=Path)
    parser.add_argument("--json", required=True, type=Path)
    args = parser.parse_args()
    result = analyze(args.stderr.read_text(errors="replace"))
    rc = int(args.rc_file.read_text().strip())
    stdout = args.stdout.read_text(errors="replace")
    result["workload"] = {"rc": rc, "completed": rc == 0 and
        ("NATURAL_WAVE_OK" in stdout or "SURVIVAL_DENSE_OK" in stdout)}
    for name, check in result["checks"].items():
        print(f"ASSERT {name} executed=1 observed={check['observed']} "
              f"failures={len(check['failures'])} result={'PASS' if check['pass'] else 'FAIL'}")
    result["pass"] = result["workload"]["completed"] and all(c["pass"] for c in result["checks"].values())
    args.json.write_text(json.dumps(result, indent=2) + "\n")
    print(f"WORKLOAD rc={rc} completed={result['workload']['completed']}")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
