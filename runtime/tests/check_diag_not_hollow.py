#!/usr/bin/env python3
"""Reject hollow diagnostics and retired phase hooks that reappear in product code.

A documented gate plus an empty implementation produces false-negative measurements.
Delete that subsystem and its call sites, or keep at least one live sink with a positive
control. A retired phase hook must stay absent from declarations, definitions, calls,
and export ledgers. No header marker or historical comment waives either check.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
VERIFY = ROOT / "src/Heap/Verify"
PRODUCT = ROOT / "src"

# These names used to look like verification checkpoints while performing no
# verification. Reintroducing even a declaration or export recreates that false
# contract, so scan the whole product tree rather than a remembered file list.
RETIRED_PHASE_HOOKS = ("ValidateMinorReferences",)

# Definitions only: a leading return type at column 0. The complete signature is
# captured through its opening brace so empty lambdas inside a live function cannot
# be mistaken for additional empty top-level definitions.
DEFINITION = re.compile(
    r"^(?:void|bool|size_t|uint\d+_t|unsigned|int|const\s+char\*)\s+"
    r"[A-Za-z_][A-Za-z0-9_:~]*\s*\([^;{}]*\)\s*(?:const\s*)?\{",
    re.M,
)
EMPTY_CONTENT = re.compile(r"\s*(?:return\s+(?:false|true|0|nullptr)\s*;\s*)?\Z")
GATE = re.compile(r"MRT_[A-Z0-9_]+")
def function_bodies(text: str):
    """Yield top-level definition bodies; braces in comments/strings are masked."""
    masked = list(text)
    for match in re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', text, re.S):
        for i in range(match.start(), match.end()):
            if masked[i] != "\n":
                masked[i] = " "
    masked_text = "".join(masked)
    for definition in DEFINITION.finditer(masked_text):
        open_brace = definition.end() - 1
        depth = 1
        cursor = open_brace + 1
        while cursor < len(masked_text) and depth != 0:
            if masked_text[cursor] == "{":
                depth += 1
            elif masked_text[cursor] == "}":
                depth -= 1
            cursor += 1
        if depth == 0:
            yield masked_text[open_brace + 1 : cursor - 1]


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
        body = src.read_text(errors="replace")
        bodies = list(function_bodies(body))
        definitions = len(bodies)
        if definitions == 0:
            continue
        empties = sum(1 for function_body in bodies if EMPTY_CONTENT.fullmatch(function_body))
        if empties >= definitions:
            offenders.append((src.name, header.name, definitions, empties))

    if offenders:
        print("DIAG_HOLLOW_GUARD FAIL: header documents a gate but the implementation is all no-ops")
        for name, hdr, definitions, empties in offenders:
            print(f"  {name}: {empties}/{definitions} bodies empty, and {hdr} still documents its gate")
        print("  Restore a live sink, or delete the subsystem and all product call sites.")
        return 1

    retired_hits = []
    for src in sorted(path for path in PRODUCT.rglob("*") if path.suffix in {".cpp", ".h", ".def"}):
        text = src.read_text(errors="replace")
        for hook in RETIRED_PHASE_HOOKS:
            for line_no, line in enumerate(text.splitlines(), 1):
                if hook in line:
                    retired_hits.append((src.relative_to(ROOT), line_no, hook))

    if retired_hits:
        print("DIAG_HOLLOW_GUARD FAIL: retired empty phase hook reappeared in product code")
        for src, line_no, hook in retired_hits:
            print(f"  {src}:{line_no}: {hook}")
        return 1

    print(f"DIAG_HOLLOW_GUARD PASS gated_subsystems={checked} retired_phase_hook_hits=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
