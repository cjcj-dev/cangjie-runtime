#!/usr/bin/env python3
"""Structural gate for the five verification faces, keyed by stable symbols."""

from pathlib import Path
import re
import sys


# A target is identified by its owning function, not by its current source line.
# Keeping the file in the identity also rejects an accidental same-named helper in
# another translation unit.
EXPECTED_TARGETS = (
    ("roots", "VerifyRoots.cpp", "void VerifyRoots::VerifyRootPayload("),
    ("objects", "VerifyHeap.cpp", "void VerifyHeapObjects("),
    ("marking", "MarkCompleteVerify.cpp", "void RunAtMarkEnd("),
    ("remembered", "VerifyRememberedSet.cpp", "void VerifyRememberedSetInvariant("),
    ("oops", "VerifyRegions.cpp", "void VerifyRegions::VerifyAfterPrepareYoung("),
    ("oops", "VerifyRegions.cpp", "void VerifyRegions::VerifyAfterYoungMark("),
)
FACES = ("roots", "objects", "marking", "remembered", "oops")
CALL_PATTERN = re.compile(
    r"VerifyPhaseEnter\(\s*VerifyFace::(Roots|Objects|Marking|Remembered|Oops)\s*,"
)


def function_body(text: str, anchor: str):
    """Return an anchored function body and its source span, or None."""
    if text.count(anchor) != 1:
        return None
    start = text.find(anchor)
    opening = text.find("{", start + len(anchor))
    if opening < 0:
        return None
    depth = 0
    for position in range(opening, len(text)):
        if text[position] == "{":
            depth += 1
        elif text[position] == "}":
            depth -= 1
            if depth == 0:
                return text[opening : position + 1], (opening, position + 1)
    return None


def check(root: Path):
    sources = {
        source.name: source.read_text(encoding="utf-8")
        for source in sorted(root.glob("*.cpp"))
    }
    result = {face: True for face in FACES}
    targets = []
    owned_ranges = {source_name: [] for source_name in sources}

    for face, source_name, anchor in EXPECTED_TARGETS:
        parsed = function_body(sources.get(source_name, ""), anchor)
        matches = [] if parsed is None else CALL_PATTERN.findall(parsed[0])
        ok = matches == [face.capitalize()]
        result[face] = result[face] and ok
        targets.append((face, source_name, anchor, ok, matches))
        if parsed is not None:
            owned_ranges[source_name].append((parsed[1], face))

    unexpected = []
    for source_name, source_text in sources.items():
        for match in CALL_PATTERN.finditer(source_text):
            face = match.group(1).lower()
            owners = [
                owner_face
                for (start, end), owner_face in owned_ranges[source_name]
                if start <= match.start() < end
            ]
            if owners != [face]:
                unexpected.append(f"{source_name}:{face}")
                result[face] = False

    return result, targets, unexpected


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_verify_phase_matrix.py <runtime/src/Heap/Verify>")
        return 2
    result, targets, unexpected = check(Path(sys.argv[1]))
    print(
        "VERIFY_PHASE_MATRIX "
        + " ".join(
            f"{face}={'GREEN' if result[face] else 'RED'}" for face in FACES
        )
    )
    for face, source, anchor, ok, matches in targets:
        print(
            f"VERIFY_PHASE_TARGET face={face} source={source} symbol={anchor} "
            f"calls={matches} status={'GREEN' if ok else 'RED'}"
        )
    print(f"VERIFY_PHASE_UNEXPECTED calls={unexpected}")
    return 0 if all(result.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
