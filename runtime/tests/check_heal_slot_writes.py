#!/usr/bin/env python3
"""Fail the build if HeapSlot CAS escapes HealSlot or loses site attribution."""

from __future__ import annotations

import re
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "runtime" / "src"
REF_FIELD = SRC / "ObjectModel" / "RefField.h"
SOURCE_SUFFIXES = {".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp"}


def mask_non_code(text: str) -> str:
    token = re.compile(
        r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
        re.DOTALL,
    )

    def spaces(match: re.Match[str]) -> str:
        return "".join("\n" if char == "\n" else " " for char in match.group(0))

    return token.sub(spaces, text)


def line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def balanced_call(text: str, open_paren: int) -> str | None:
    depth = 0
    for pos in range(open_paren, len(text)):
        if text[pos] == "(":
            depth += 1
        elif text[pos] == ")":
            depth -= 1
            if depth == 0:
                return text[open_paren + 1 : pos]
    return None


def main() -> int:
    files = sorted(path for path in SRC.rglob("*") if path.suffix in SOURCE_SUFFIXES)
    masked = {path: mask_non_code(path.read_text(encoding="utf-8")) for path in files}
    failures: list[str] = []

    direct_calls: list[tuple[Path, int, str]] = []
    direct_pattern = re.compile(r"(?:\.|->)\s*CompareExchange\s*\(")
    for path, text in masked.items():
        original = path.read_text(encoding="utf-8")
        for match in direct_pattern.finditer(text):
            line = line_number(text, match.start())
            source_line = original.splitlines()[line - 1].strip()
            direct_calls.append((path, line, source_line))

    allowed_direct = [
        item
        for item in direct_calls
        if item[0] == REF_FIELD and "slot.CompareExchange(expected, desired" in item[2]
    ]
    escaped_direct = [item for item in direct_calls if item not in allowed_direct]
    if len(allowed_direct) != 1:
        failures.append(f"HealSlot body CompareExchange count is {len(allowed_direct)}, expected 1")
    for path, line, source_line in escaped_direct:
        failures.append(f"direct CompareExchange escaped HealSlot: {path.relative_to(REPO)}:{line}: {source_line}")

    if not re.search(
        r"allowNull\s*==\s*HealNull::Disallow\s*&&\s*!is_null\(expected\)\s*&&\s*is_null\(desired\)",
        masked[REF_FIELD],
    ):
        failures.append("HealSlot default non-null-to-null guard is missing")
    if not re.search(
        r"allowNull\s*==\s*HealNull::Disallow\s*&&\s*!is_null\(observed\)\s*&&\s*is_null\(good\)",
        masked[REF_FIELD],
    ):
        failures.append("HealRoot default non-null-to-null guard is missing")

    ref_text = masked[REF_FIELD]
    enum_match = re.search(r"enum\s+class\s+HealSite\s*:[^{]+\{(?P<body>.*?)\};", ref_text, re.DOTALL)
    if enum_match is None:
        failures.append("HealSite enum not found")
        enum_sites: list[str] = []
    else:
        enum_sites = re.findall(r"\b([A-Za-z][A-Za-z0-9_]*)\s*,", enum_match.group("body"))
    if any(site.upper() == "OTHER" for site in enum_sites):
        failures.append("HealSite::OTHER is forbidden")

    explicit_uses = 0
    for site in enum_sites:
        uses = sum(len(re.findall(rf"\bHealSite::{re.escape(site)}\b", text)) for text in masked.values())
        explicit_uses += uses
        if uses == 0:
            failures.append(f"HealSite::{site} is not assigned to a call site")

    for function in ("HealSlot", "HealRoot"):
        pattern = re.compile(rf"\b{function}\s*\(")
        for path, text in masked.items():
            for match in pattern.finditer(text):
                prefix = text[max(0, match.start() - 80) : match.start()]
                if re.search(r"\b(?:friend\s+)?(?:inline\s+)?bool\s*$", prefix):
                    continue
                arguments = balanced_call(text, match.end() - 1)
                line = line_number(text, match.start())
                if arguments is None:
                    failures.append(f"unbalanced {function} call: {path.relative_to(REPO)}:{line}")
                    continue
                if "HealSite::" in arguments:
                    continue
                if re.search(r"\bsite\b", arguments) and path in {
                    REF_FIELD,
                    SRC / "Heap" / "WCollector" / "WCollector.cpp",
                }:
                    continue
                failures.append(f"{function} call lacks explicit HealSite: {path.relative_to(REPO)}:{line}")

    if failures:
        print(
            "HEAL_SLOT_GUARD FAIL "
            f"direct_calls={len(direct_calls)} escaped={len(escaped_direct)} "
            f"enum_sites={len(enum_sites)} explicit_uses={explicit_uses} failures={len(failures)}"
        )
        for failure in failures:
            print(failure)
        return 1

    print(
        "HEAL_SLOT_GUARD PASS "
        f"direct_calls={len(direct_calls)} escaped=0 "
        f"enum_sites={len(enum_sites)} explicit_uses={explicit_uses} other=0"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
