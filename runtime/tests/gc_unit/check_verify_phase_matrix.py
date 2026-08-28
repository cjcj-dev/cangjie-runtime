#!/usr/bin/env python3
"""Structural five-face gate with a precise per-call-site manifest.

The manifest is intentionally keyed by the product source ``file:line`` of
every VerifyPhaseEnter call.  A face is not considered covered merely because
one marker remains in its translation unit: removing either Oops entry must
change the extracted set and fail closed.
"""
from pathlib import Path
import sys

# Frozen product call-site manifest.  Keep this set in lock-step with the
# product source; the checker below extracts a fresh set rather than trusting
# marker strings supplied by tests.
EXPECTED_CALLS = {
    "roots": {"VerifyRoots.cpp:104"},
    "objects": {"VerifyHeap.cpp:385"},
    "marking": {"MarkCompleteVerify.cpp:763"},
    "remembered": {"VerifyRememberedSet.cpp:173"},
    "oops": {"VerifyRegions.cpp:222", "VerifyRegions.cpp:426"},
}


def check(root: Path):
    actual = {face: set() for face in EXPECTED_CALLS}
    for source in sorted(root.glob("*.cpp")):
        for line_no, line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
            marker = "VerifyPhaseEnter(VerifyFace::"
            if marker not in line:
                continue
            face = line.split(marker, 1)[1].split(",", 1)[0].strip()
            face = face.lower()
            if face in actual:
                actual[face].add(f"{source.name}:{line_no}")
    result = {face: actual[face] == EXPECTED_CALLS[face] for face in EXPECTED_CALLS}
    return result, actual


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_verify_phase_matrix.py <runtime/src/Heap/Verify>")
        return 2
    result, actual = check(Path(sys.argv[1]))
    bad = [face for face, ok in result.items() if not ok]
    print("VERIFY_PHASE_MATRIX " + " ".join(f"{face}={'GREEN' if ok else 'RED'}" for face, ok in result.items()))
    for face in EXPECTED_CALLS:
        print(f"VERIFY_PHASE_CALLS {face}=actual:{sorted(actual[face])} expected:{sorted(EXPECTED_CALLS[face])}")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
