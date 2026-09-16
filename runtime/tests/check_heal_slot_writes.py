#!/usr/bin/env python3
"""Fail the build if HealSlot/ZgcSelfHeal remain in product sources."""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "runtime" / "src"
SOURCE_SUFFIXES = {".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp"}
FORBIDDEN = re.compile(r"\b(HealSlot|ZgcSelfHeal)\b")


def mask_non_code(text: str) -> str:
    token = re.compile(
        r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
        re.DOTALL,
    )

    def spaces(match: re.Match[str]) -> str:
        return "".join("\n" if char == "\n" else " " for char in match.group(0))

    return token.sub(spaces, text)


def main() -> int:
    failures: list[str] = []
    for path in sorted(p for p in SRC.rglob("*") if p.suffix in SOURCE_SUFFIXES):
        text = mask_non_code(path.read_text(encoding="utf-8"))
        for match in FORBIDDEN.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            failures.append(f"{path.relative_to(REPO)}:{line}: {match.group(0)}")
    if failures:
        print(f"HEAL_SLOT_GUARD FAIL leftover={len(failures)}")
        for item in failures:
            print(item)
        return 1
    print("HEAL_SLOT_GUARD PASS leftover=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
