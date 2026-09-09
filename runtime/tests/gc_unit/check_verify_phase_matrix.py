#!/usr/bin/env python3
"""Structural gate for the five verification faces, keyed by C++ symbols."""

from pathlib import Path
import os
import re
import shlex
import subprocess
import sys


# A target is identified by its owning function's C++ tokens, not by a line number
# or a verbatim declaration. Keeping the file in the identity also rejects an
# accidental same-named helper in another translation unit.
EXPECTED_TARGETS = (
    ("roots", "VerifyRoots.cpp", ("VerifyRoots", "VerifyRootPayload")),
    ("objects", "VerifyHeap.cpp", ("VerifyHeapObjects",)),
    ("marking", "MarkCompleteVerify.cpp", ("RunAtMarkEnd",)),
    ("remembered", "VerifyRememberedSet.cpp", ("VerifyRememberedSetInvariant",)),
    ("oops", "VerifyRegions.cpp", ("VerifyRegions", "VerifyAfterPrepareYoung")),
    ("oops", "VerifyRegions.cpp", ("VerifyRegions", "VerifyAfterYoungMark")),
)
FACES = ("roots", "objects", "marking", "remembered", "oops")
TOKEN_PATTERN = re.compile(r"[A-Za-z_]\w*|::|->|&&|\|\||[^\s]")
FACE_SPELLINGS = {face.capitalize(): face for face in FACES}


def mask_comments_and_literals(text: str) -> str:
    """Blank comments and literals while preserving offsets and newlines."""
    chars = list(text)
    index = 0
    state = "code"
    raw_end = ""
    while index < len(chars):
        pair = text[index : index + 2]
        if state == "code":
            raw = re.match(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(', text[index:])
            if raw:
                raw_end = ")" + raw.group(1) + '"'
                state = "raw"
                end = index + raw.end()
                for position in range(index, end):
                    if chars[position] != "\n":
                        chars[position] = " "
                index = end
                continue
            if pair == "//":
                state = "line-comment"
            elif pair == "/*":
                state = "block-comment"
            elif text[index] == '"':
                state = "string"
            elif text[index] == "'":
                state = "char"
            else:
                index += 1
                continue
            chars[index] = " "
            index += 1
            continue
        if state == "raw" and text.startswith(raw_end, index):
            for position in range(index, index + len(raw_end)):
                if chars[position] != "\n":
                    chars[position] = " "
            index += len(raw_end)
            state = "code"
            continue
        current = text[index]
        chars[index] = "\n" if current == "\n" else " "
        if state == "line-comment" and current == "\n":
            state = "code"
        elif state == "block-comment" and pair == "*/":
            if index + 1 < len(chars):
                chars[index + 1] = " "
            index += 2
            state = "code"
            continue
        elif state in ("string", "char") and current == "\\":
            if index + 1 < len(chars):
                chars[index + 1] = "\n" if text[index + 1] == "\n" else " "
            index += 2
            continue
        elif (state == "string" and current == '"') or (state == "char" and current == "'"):
            state = "code"
        index += 1
    return "".join(chars)


def without_include_directives(text: str) -> str:
    """Blank include directives without expanding unrelated header contents.

    The compiler still performs translation-phase line splicing, conditional
    evaluation, and macro expansion on the translation unit itself.  Includes
    are omitted because this gate owns call sites in these source files, not
    declarations or generated tokens in their transitive headers.
    """
    lines = text.splitlines(keepends=True)
    result = []
    index = 0
    while index < len(lines):
        group = [lines[index]]
        index += 1
        while group[-1].endswith("\\\n") or group[-1].endswith("\\\r\n"):
            if index >= len(lines):
                break
            group.append(lines[index])
            index += 1
        logical = "".join(group).replace("\\\r\n", "").replace("\\\n", "")
        if re.match(r"\s*#\s*include\b", logical):
            result.extend(
                "".join("\n" if char == "\n" else " " for char in line)
                for line in group
            )
        else:
            result.extend(group)
    return "".join(result)


def preprocess_source(text: str) -> str:
    """Use the real C++ preprocessor for translation-phase semantics."""
    compiler = shlex.split(os.environ.get("CXX", "clang++"))
    if not compiler:
        raise RuntimeError("CXX selects no compiler")
    process = subprocess.run(
        compiler + ["-E", "-P", "-x", "c++", "-"],
        input=without_include_directives(text),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if process.returncode != 0:
        diagnostic = process.stderr.strip().replace("\n", " | ")
        raise RuntimeError(
            f"C++ preprocessing failed rc={process.returncode}: {diagnostic}"
        )
    return process.stdout


def cpp_tokens(text: str):
    visible = mask_comments_and_literals(preprocess_source(text))
    return [(match.group(0), match.start()) for match in TOKEN_PATTERN.finditer(visible)]


def matching_token(tokens, opening: int, left: str, right: str):
    depth = 0
    for index in range(opening, len(tokens)):
        if tokens[index][0] == left:
            depth += 1
        elif tokens[index][0] == right:
            depth -= 1
            if depth == 0:
                return index
    return None


def function_body(tokens, symbol):
    """Return the unique definition body token span for a qualified symbol."""
    needle = [symbol[0], "("] if len(symbol) == 1 else [symbol[0], "::", symbol[1], "("]
    candidates = []
    values = [token for token, _ in tokens]
    for start in range(len(tokens) - len(needle) + 1):
        if values[start : start + len(needle)] != needle:
            continue
        opening = start + len(needle) - 1
        closing = matching_token(tokens, opening, "(", ")")
        if closing is None:
            continue
        cursor = closing + 1
        while cursor < len(tokens) and tokens[cursor][0] not in ("{", ";"):
            cursor += 1
        if cursor >= len(tokens) or tokens[cursor][0] != "{":
            continue
        body_end = matching_token(tokens, cursor, "{", "}")
        if body_end is not None:
            candidates.append((cursor, body_end))
    return candidates[0] if len(candidates) == 1 else None


def phase_calls(tokens):
    """Yield (face, token index) for lexically active VerifyPhaseEnter calls."""
    values = [token for token, _ in tokens]
    for index in range(len(tokens) - 5):
        if values[index : index + 4] != ["VerifyPhaseEnter", "(", "VerifyFace", "::"]:
            continue
        spelling = values[index + 4]
        if spelling in FACE_SPELLINGS and values[index + 5] == ",":
            yield FACE_SPELLINGS[spelling], index


def check(root: Path):
    sources = {
        source.name: cpp_tokens(source.read_text(encoding="utf-8"))
        for source in sorted(root.glob("*.cpp"))
    }
    result = {face: True for face in FACES}
    targets = []
    owned_ranges = {source_name: [] for source_name in sources}

    for face, source_name, symbol in EXPECTED_TARGETS:
        source_tokens = sources.get(source_name, [])
        parsed = function_body(source_tokens, symbol)
        matches = []
        if parsed is not None:
            matches = [
                call_face
                for call_face, token_index in phase_calls(source_tokens)
                if parsed[0] < token_index < parsed[1]
            ]
        ok = matches == [face]
        result[face] = result[face] and ok
        targets.append((face, source_name, "::".join(symbol), ok, matches))
        if parsed is not None:
            owned_ranges[source_name].append((parsed, face))

    unexpected = []
    for source_name, source_tokens in sources.items():
        for face, token_index in phase_calls(source_tokens):
            owners = [
                owner_face
                for (start, end), owner_face in owned_ranges[source_name]
                if start < token_index < end
            ]
            if owners != [face]:
                unexpected.append(f"{source_name}:{face}")
                result[face] = False

    return result, targets, unexpected


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_verify_phase_matrix.py <runtime/src/Heap/Verify>")
        return 2
    try:
        result, targets, unexpected = check(Path(sys.argv[1]))
    except (OSError, RuntimeError) as error:
        print(f"VERIFY_PHASE_PREPROCESS_FAIL {error}", file=sys.stderr)
        return 2
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
