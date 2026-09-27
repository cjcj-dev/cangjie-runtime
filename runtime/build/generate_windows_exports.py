#!/usr/bin/env python3

# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

import argparse
import os
import hashlib
import json
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


EXPORT_RE = re.compile(r"^(\S+)\s+@(\d+)(?:\s+(DATA))?$")


@dataclass(frozen=True)
class Export:
    name: str
    ordinal: int
    is_data: bool


def parse_exports(path: Path) -> list[Export]:
    exports: list[Export] = []
    names: set[str] = set()
    ordinals: set[int] = set()
    saw_header = False
    for line_number, original in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = original.strip()
        if not line or line.startswith(";"):
            continue
        if line == "EXPORTS" and not saw_header and not exports:
            saw_header = True
            continue
        matched = EXPORT_RE.fullmatch(line)
        if not saw_header or matched is None:
            raise ValueError(f"{path}:{line_number}: malformed export definition: {original}")
        name = matched.group(1)
        ordinal = int(matched.group(2))
        if ordinal == 0:
            raise ValueError(f"{path}:{line_number}: ordinal must be positive")
        if name in names:
            raise ValueError(f"{path}:{line_number}: duplicate export name: {name}")
        if ordinal in ordinals:
            raise ValueError(f"{path}:{line_number}: duplicate export ordinal: {ordinal}")
        names.add(name)
        ordinals.add(ordinal)
        exports.append(Export(name, ordinal, matched.group(3) is not None))
    if not saw_header or not exports:
        raise ValueError(f"{path}: empty export definition")
    return sorted(exports, key=lambda export: export.ordinal)


# The contract is captured from consumers, never from the DLL under test.
DEFAULT_REFERENCES = Path(__file__).resolve().parents[1] / "src/windows_export_references.json"
SOURCE_SUFFIXES = {".cj", ".c", ".cpp", ".cc", ".h", ".hpp", ".inc", ".def", ".td", ".S", ".s"}
TOKEN_RE = re.compile(r"[A-Za-z_][A-Za-z_0-9$]*")
COMMENT_RE = re.compile(r'("(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\')|//[^\n]*|/\*.*?\*/', re.S)


def consumer_text(text: str) -> str:
    # Preserve strings (compiler-emitted symbol names) and line positions.
    return COMMENT_RE.sub(lambda m: m.group(1) or "\n" * m.group().count("\n"), text)


def collect_references(raw_path: Path, consumers: list[str], output: Path) -> None:
    available = {export.name for export in parse_exports(raw_path)}
    references: dict[str, list[str]] = {}
    inputs = []
    for consumer in consumers:
        # kind:label@revision=/absolute/source/subtree; the label is portable.
        identity, directory = consumer.split("=", 1)
        kind_label, revision = identity.split("@", 1)
        kind, label = kind_label.split(":", 1)
        if kind not in {"emitter", "source"} or not label or re.fullmatch(r"[0-9a-f]{40}", revision) is None:
            raise ValueError("consumer must be emitter:label@40-digit-revision=directory or source:label@40-digit-revision=directory")
        root = Path(directory)
        files = sorted(p for p in root.rglob("*") if p.is_file() and p.suffix in SOURCE_SUFFIXES)
        if not files:
            raise ValueError(f"no consumer sources in {root}")
        digest = hashlib.sha256()
        # Resolve the compiler's string constants from their definitions instead
        # of maintaining another list of runtime prefixes.
        constants: dict[str, str] = {}
        texts = []
        for path in files:
            contents = path.read_bytes()
            relative = path.relative_to(root).as_posix()
            digest.update(relative.encode() + b"\0" + contents + b"\0")
            texts.append((relative, consumer_text(contents.decode("utf-8"))))
        definitions = re.compile(r'(?:const std::string|(?:public )?let)\s+(\w+)(?:\s*:\s*String)?\s*=\s*([^;\n]+)')
        pending = [m.groups() for _, text in texts for m in definitions.finditer(text)]
        while pending:
            remaining = []
            for name, expression in pending:
                pieces = [part.strip() for part in expression.split("+")]
                if all(re.fullmatch(r'"[A-Za-z_0-9$]*"', part) or part in constants for part in pieces):
                    constants[name] = "".join(part[1:-1] if part.startswith('"') else constants[part] for part in pieces)
                else:
                    remaining.append((name, expression))
            if len(remaining) == len(pending):
                break
            pending = remaining
        concat = re.compile(r'\b([A-Za-z_]\w*)\s*\+\s*"([A-Za-z_0-9$]+)"')
        for relative, text in texts:
            for line_number, line in enumerate(text.splitlines(), 1):
                # Emitters reference target symbols in string literals or macro
                # tables; their host C++ calls are not runtime dependencies.
                fragments = [line] if kind == "source" or Path(relative).suffix in {".def", ".td"} else re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', line)
                names = {token for fragment in fragments for token in TOKEN_RE.findall(fragment)}
                names.update(constants[m.group(1)] + m.group(2) for m in concat.finditer(line)
                             if m.group(1) in constants)
                for name in sorted(names & available):
                    references.setdefault(name, []).append(f"{label}/{relative}:{line_number}")
        inputs.append({"kind": kind, "label": label, "revision": revision, "sha256": digest.hexdigest(), "files": len(files)})
    if not references:
        raise ValueError("consumer sources reference no exports")
    output.write_text(json.dumps({"inputs": inputs, "symbols": dict(sorted(references.items()))}, indent=2) + "\n")
    print(f"WINDOWS_EXPORT_REFERENCES symbols={len(references)} output={output}")


def read_references(path: Path) -> set[str]:
    document = json.loads(path.read_text(encoding="utf-8"))
    symbols = document["symbols"]
    if not symbols or any(not isinstance(name, str) or not anchors for name, anchors in symbols.items()):
        raise ValueError(f"{path}: empty or invalid consumer references")
    return set(symbols)


def select_exports(exports: list[Export], references: set[str]) -> dict[str, Export]:
    return {export.name: export for export in exports if export.name in references}


def render_exports(exports: list[Export], source_ref: str) -> str:
    lines = [
        "; Generated by runtime/build/generate_windows_exports.py.",
        f"; source-ref: {source_ref}",
        "; source: consumer references intersected with a complete Windows linker --output-def.",
        "; DO NOT hand-edit: regenerate from a reviewed Windows runtime build.",
        "",
        "EXPORTS",
    ]
    for export in exports:
        data = " DATA" if export.is_data else ""
        lines.append(f"    {export.name} @{export.ordinal}{data}")
    return "\n".join(lines) + "\n"


def write_exports(raw_path: Path, output_path: Path, source_ref: str, references_path: Path) -> None:
    references = read_references(references_path)
    selected = select_exports(parse_exports(raw_path), references)
    missing = sorted(references - selected.keys())
    if missing:
        raise ValueError(f"missing consumer exports: {', '.join(missing)}")
    contents = render_exports(list(selected.values()), source_ref)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    file_descriptor, temporary_name = tempfile.mkstemp(prefix=f".{output_path.name}.", dir=output_path.parent)
    try:
        with os.fdopen(file_descriptor, "w", encoding="utf-8", newline="\n") as output:
            output.write(contents)
        os.replace(temporary_name, output_path)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise
    print(f"WINDOWS_EXPORT_DEF_WRITE output={output_path} exports={len(parse_exports(output_path))}")


def check_exports(raw_path: Path, expected_path: Path, references_path: Path) -> int:
    references = read_references(references_path)
    actual = select_exports(parse_exports(raw_path), references)
    registered = parse_exports(expected_path)
    expected = select_exports(registered, references)
    if set(expected) != references or len(expected) != len(registered):
        raise ValueError("registered definition does not match the consumer reference set; regenerate it")
    missing = sorted(expected.keys() - actual.keys())
    changed = sorted(
        name for name in actual.keys() & expected.keys()
        if actual[name].is_data != expected[name].is_data
    )
    print(
        f"WINDOWS_EXPORT_DEF_CHECK actual={len(actual)} expected={len(expected)} "
        f"missing={len(missing)} changed={len(changed)}"
    )
    for name in missing:
        print(f"FATAL: missing export: {name}", file=sys.stderr)
    for name in changed:
        print(f"FATAL: changed export: expected={expected[name]} actual={actual[name]}", file=sys.stderr)
    return 1 if missing or changed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate or check the Windows runtime export definition")
    subparsers = parser.add_subparsers(dest="command", required=True)

    write_parser = subparsers.add_parser("write", help="write a canonical definition from linker --output-def")
    write_parser.add_argument("raw", type=Path)
    write_parser.add_argument("output", type=Path)
    write_parser.add_argument("--source-ref", required=True)

    check_parser = subparsers.add_parser("check", help="compare linker --output-def with the committed definition")
    check_parser.add_argument("raw", type=Path)
    check_parser.add_argument("expected", type=Path)

    collect_parser = subparsers.add_parser("collect", help="capture consumer references from pinned source trees")
    collect_parser.add_argument("raw", type=Path)
    collect_parser.add_argument("output", type=Path)
    collect_parser.add_argument("--consumer", action="append", required=True)
    for subparser in (write_parser, check_parser):
        subparser.add_argument("--references", type=Path, default=DEFAULT_REFERENCES)

    arguments = parser.parse_args()
    try:
        if arguments.command == "collect":
            collect_references(arguments.raw, arguments.consumer, arguments.output)
            return 0
        if arguments.command == "write":
            write_exports(arguments.raw, arguments.output, arguments.source_ref, arguments.references)
            return 0
        return check_exports(arguments.raw, arguments.expected, arguments.references)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"FATAL: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
