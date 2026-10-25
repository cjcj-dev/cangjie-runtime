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
import subprocess
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
DEFAULT_HISTORY = DEFAULT_REFERENCES.with_name("windows_x86_64_exports.def")
DEFAULT_RETIRED = DEFAULT_REFERENCES.with_name("windows_retired_exports.def")


def retired_exports(path: Path | None) -> dict[str, Export]:
    # The absence only supports older source snapshots and CLI fixtures.
    source = path or DEFAULT_RETIRED
    if path is None and not source.exists():
        return {}
    return {entry.name: entry for entry in parse_exports(source)}


def check_retired(exports: list[Export], retired: dict[str, Export]) -> None:
    reserved = {entry.ordinal for entry in retired.values()}
    for entry in exports:
        if entry.name in retired or entry.ordinal in reserved:
            raise ValueError(f"retired export name/ordinal reused: {entry}")
SOURCE_SUFFIXES = {".cj", ".c", ".cpp", ".cc", ".h", ".hpp", ".inc", ".def", ".td", ".S", ".s"}
TOKEN_RE = re.compile(r"[A-Za-z_][A-Za-z_0-9$]*")
COMMENT_RE = re.compile(r'("(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\')|//[^\n]*|/\*.*?\*/', re.S)


def consumer_text(text: str) -> str:
    # Preserve strings (compiler-emitted symbol names) and line positions.
    return COMMENT_RE.sub(lambda m: m.group(1) or "\n" * m.group().count("\n"), text)


def capture_native_sources(build: Path, root: Path, output: Path) -> None:
    """Make a portable source set from a successful native CMake configure."""
    root = root.resolve()
    commands = json.loads((build / "compile_commands.json").read_text())
    cache = (build / "CMakeCache.txt").read_text()
    target = re.search(r"^EXPORT_SOURCE_SYSTEM:INTERNAL=(.+)$", cache, re.M)
    if target is None:
        raise ValueError("native build must use export_source_config")
    sources = set()
    for command in commands:
        path = (Path(command["directory"]) / command["file"]).resolve()
        if path.is_relative_to(root):
            sources.add(path.relative_to(root).as_posix())
    configurations = sorted(root.glob("**/native/CMakeLists.txt"))
    missing = [p.parent.relative_to(root).as_posix() for p in configurations
               if not any((root / name).parent == p.parent for name in sources)]
    if not sources or missing:
        raise ValueError(f"native configuration has no translation units for: {missing}")
    document = {"system": target[1], "sources": sorted(sources),
                "configurations": {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                                   for p in configurations}}
    output.write_text(json.dumps(document, indent=2) + "\n")
    print(f"NATIVE_SOURCE_CAPTURE system={target[1]} directories={len(configurations)} sources={len(sources)}")


def source_files(root: Path, kind: str, selection: Path | None) -> list[Path]:
    files = sorted(p for p in root.rglob("*") if p.is_file() and p.suffix in SOURCE_SUFFIXES)
    if kind == "emitter":
        return files
    if selection is None:
        raise ValueError("source consumer requires --native-sources label=manifest")
    document = json.loads(selection.read_text())
    if document["system"] != "Windows":
        raise ValueError("Windows export collection requires a Windows native source configuration")
    configurations = {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in root.glob("**/native/CMakeLists.txt")}
    if configurations != document["configurations"]:
        raise ValueError("native source configuration differs from consumer source tree; recapture it")
    selected = set(document["sources"])
    if not selected or any(not (root / name).is_file() or not (root / name).resolve().is_relative_to(root.resolve())
                           for name in selected):
        raise ValueError("native source configuration contains missing or invalid source paths")
    return [p for p in files if p.suffix == ".cj" or p.relative_to(root).as_posix() in selected]


def runtime_definitions(identity: str) -> tuple[dict, dict[str, list[str]]]:
    """Find explicit C-linkage definitions, independently of the candidate raw.

    This is additional ownership evidence, not a C++ parser or a claim that every
    platform compiles every definition. The complete Windows raw remains needed
    for asm aliases, C++ mangling, DATA and ordinal evidence.
    """
    label_ref, directory = identity.split("=", 1)
    label, revision = label_ref.split("@", 1)
    if not label or re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise ValueError("runtime source must be LABEL@40-digit-revision=directory")
    root = Path(directory)
    files = sorted(p for p in root.rglob("*") if p.is_file() and p.suffix in {".cpp", ".cc", ".c"})
    if not files:
        raise ValueError(f"no runtime definitions in {root}")
    definitions: dict[str, list[str]] = {}
    digest = hashlib.sha256()
    pattern = re.compile(r'extern\s+"C"\s+(?:[\w:*&<>]+\s+)*?([A-Za-z_]\w*)\s*'
                         r'\([^;{}]*\)\s*(?:noexcept\s*)?\{')
    for path in files:
        relative = path.relative_to(root).as_posix()
        contents = path.read_bytes()
        digest.update(relative.encode() + b"\0" + contents + b"\0")
        text = consumer_text(contents.decode("utf-8"))
        for match in pattern.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            definitions.setdefault(match[1], []).append(f"{label}/{relative}:{line}")
    if not definitions:
        raise ValueError("runtime source contains no explicit C-linkage definitions")
    return {"label": label, "revision": revision, "sha256": digest.hexdigest(), "files": len(files)}, definitions


def collect_references(raw_path: Path, consumers: list[str], output: Path, native_sources: list[str],
                       runtime_source: str, historical: Path, diagnostics: Path | None,
                       runtime_capture: Path | None, retired_path: Path | None) -> None:
    available = {export.name for export in parse_exports(raw_path)}
    history = parse_exports(historical)
    retired = retired_exports(retired_path)
    for entry in history:
        if entry.name in retired and retired[entry.name] != entry:
            raise ValueError(f"retired export does not match historical definition: {entry.name}")
    runtime_input, definitions = runtime_definitions(runtime_source)
    if runtime_capture is not None:
        capture = json.loads(runtime_capture.read_text())
        if capture["system"] != "Windows" or capture["revision"] != runtime_input["revision"]:
            raise ValueError("runtime definition capture must match the fixed Windows source revision")
        root = Path(runtime_source.split("=", 1)[1]).resolve()
        definitions = {}
        linkage = capture.get("linkage", {})
        targets = linkage.get("targets", [])
        root_target = next((target for target in targets if target["id"] == linkage.get("root")), None)
        if not root_target or root_target["name"] != "cangjie-runtime" or root_target["type"] != "SHARED_LIBRARY":
            raise ValueError("Windows runtime capture lacks the normal DLL link closure")
        expected_sources = {(target["id"], source) for target in targets for source in target["sources"]}
        actual_sources = [(obj.get("target"), obj["source"]) for obj in capture["objects"]]
        if not expected_sources or set(actual_sources) != expected_sources or len(actual_sources) != len(set(actual_sources)):
            raise ValueError("Windows runtime capture is incomplete for the selected link objects")
        for obj in capture["objects"]:
            source = (root / obj["source"]).resolve()
            if not source.is_relative_to(root) or hashlib.sha256(source.read_bytes()).hexdigest() != obj["source_sha256"]:
                raise ValueError(f"runtime object source differs from fixed source: {obj['source']}")
            for name in obj["defined"]:
                definitions.setdefault(name, []).append(f"{runtime_input['label']}/{obj['source']} ({obj['object']})")
        runtime_input["windows_object_capture_sha256"] = hashlib.sha256(runtime_capture.read_bytes()).hexdigest()
    # Ownership must not depend solely on a DLL that may be missing a new export.
    owned = available | definitions.keys() | {entry.name for entry in history}
    references: dict[str, list[str]] = {}
    inputs = []
    selections = dict(item.split("=", 1) for item in native_sources)
    for consumer in consumers:
        # kind:label@revision=/absolute/source/subtree; the label is portable.
        identity, directory = consumer.split("=", 1)
        kind_label, revision = identity.split("@", 1)
        kind, label = kind_label.split(":", 1)
        if kind not in {"emitter", "source"} or not label or re.fullmatch(r"[0-9a-f]{40}", revision) is None:
            raise ValueError("consumer must be emitter:label@40-digit-revision=directory or source:label@40-digit-revision=directory")
        root = Path(directory)
        selection = Path(selections[label]) if label in selections else None
        files = source_files(root, kind, selection)
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
        constant_definitions = re.compile(r'(?:const std::string|(?:public )?let)\s+(\w+)(?:\s*:\s*String)?\s*=\s*([^;\n]+)')
        pending = [m.groups() for _, text in texts for m in constant_definitions.finditer(text)]
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
                for name in sorted(names & owned):
                    references.setdefault(name, []).append(f"{label}/{relative}:{line_number}")
        identity = {"kind": kind, "label": label, "revision": revision, "sha256": digest.hexdigest(), "files": len(files)}
        if selection is not None:
            identity["native_sources_sha256"] = hashlib.sha256(selection.read_bytes()).hexdigest()
        inputs.append(identity)
    if not references:
        raise ValueError("consumer sources reference no exports")
    if references.keys() & retired.keys():
        raise ValueError(f"current consumers still require retired exports: {sorted(references.keys() & retired.keys())}")
    document = {"inputs": inputs, "symbols": dict(sorted(references.items())),
                "runtime": runtime_input,
                "definitions": {name: definitions[name] for name in sorted(references) if name in definitions},
                "historical": {"sha256": hashlib.sha256(historical.read_bytes()).hexdigest(),
                               "exports": [entry.__dict__ for entry in history]},
                "retired": [entry.__dict__ for entry in retired.values()]}
    required = set(references) | ({entry.name for entry in history} - retired.keys())
    missing = sorted(required - available)
    if diagnostics is not None:
        diagnostics.write_text(json.dumps({**document, "missing_from_raw": missing}, indent=2) + "\n")
    if missing:
        raise ValueError(f"incomplete Windows raw; missing required exports: {', '.join(missing)}")
    output.write_text(json.dumps(document, indent=2) + "\n")
    print(f"WINDOWS_EXPORT_REFERENCES symbols={len(references)} output={output}")


def read_references(path: Path) -> set[str]:
    document = json.loads(path.read_text(encoding="utf-8"))
    symbols = document["symbols"]
    if not symbols or any(not isinstance(name, str) or not anchors for name, anchors in symbols.items()):
        raise ValueError(f"{path}: empty or invalid consumer references")
    return set(symbols)


def captured_history(path: Path) -> dict[str, Export]:
    document = json.loads(path.read_text(encoding="utf-8"))
    entries = document.get("historical", {}).get("exports", [])
    result = {entry["name"]: Export(**entry) for entry in entries}
    if len(result) != len(entries):
        raise ValueError("duplicate captured historical exports")
    return result


def select_exports(exports: list[Export], references: set[str]) -> dict[str, Export]:
    return {export.name: export for export in exports if export.name in references}


def render_exports(exports: list[Export], source_ref: str) -> str:
    lines = [
        "; Generated by runtime/build/generate_windows_exports.py.",
        f"; source-ref: {source_ref}",
        "; source: consumer references plus historical contract, checked against complete Windows linker --output-def.",
        "; DO NOT hand-edit: regenerate from a reviewed Windows runtime build.",
        "",
        "EXPORTS",
    ]
    for export in exports:
        data = " DATA" if export.is_data else ""
        lines.append(f"    {export.name} @{export.ordinal}{data}")
    return "\n".join(lines) + "\n"


def write_exports(raw_path: Path, output_path: Path, source_ref: str, references_path: Path,
                  historical: Path | None, retired_path: Path | None) -> None:
    references = read_references(references_path)
    retired = retired_exports(retired_path)
    if references & retired.keys():
        raise ValueError("current consumers require a retired export")
    history = parse_exports(historical or output_path)
    historical_by_name = {entry.name: entry for entry in history}
    for name, entry in captured_history(references_path).items():
        if historical_by_name.get(name, retired.get(name)) != entry:
            raise ValueError(f"historical definition lost or changed captured export: {name}")
    required = references | ({entry.name for entry in history} - retired.keys())
    raw = parse_exports(raw_path)
    check_retired(raw, retired)
    selected = select_exports(raw, required)
    missing = sorted(required - selected.keys())
    if missing:
        raise ValueError(f"missing consumer exports: {', '.join(missing)}")
    for entry in history:
        if entry.name in retired:
            continue
        if selected[entry.name] != entry:
            raise ValueError(f"historical export changed: expected={entry} actual={selected[entry.name]}")
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


def check_exports(raw_path: Path, expected_path: Path, references_path: Path,
                  dll: Path | None = None, readobj: str = "llvm-readobj",
                  retired_path: Path | None = None) -> int:
    references = read_references(references_path)
    retired = retired_exports(retired_path)
    raw = parse_exports(raw_path)
    if dll is not None:
        # Inspect the complete linked table, including exports outside the
        # registered subset. A raw sidecar is not proof of the actual DLL.
        result = subprocess.run([readobj, "--coff-exports", str(dll)], capture_output=True, text=True)
        print(result.stdout, end="")
        if result.returncode:
            raise ValueError(f"DLL export reader failed rc={result.returncode}: {result.stderr}")
        actual_dll = {}
        dll_ordinals = set()
        for block in re.findall(r"Export\s*\{([^{}]*)\}", result.stdout):
            name = re.search(r"^[ \t]*Name:[ \t]*(\S*)[ \t]*$", block, re.M)
            ordinal = re.search(r"^\s*Ordinal:\s*(\d+)\s*$", block, re.M)
            rva = re.search(r"^\s*RVA:\s*(0x[0-9A-Fa-f]+)\s*$", block, re.M)
            if not name or not ordinal or not rva or int(ordinal[1]) in dll_ordinals:
                raise ValueError("invalid or duplicate DLL export entry")
            dll_ordinals.add(int(ordinal[1]))
            # PE export address tables include empty ordinal holes, including
            # reviewed retirements. They are not named/forwarded exports.
            if not name[1] and int(rva[1], 16) == 0:
                continue
            if not name[1] or int(rva[1], 16) == 0 or name[1] in actual_dll:
                raise ValueError("invalid or unnamed DLL export entry")
            actual_dll[name[1]] = int(ordinal[1])
        if actual_dll != {entry.name: entry.ordinal for entry in raw}:
            raise ValueError("complete DLL export names/ordinals differ from linker raw")
    registered = parse_exports(expected_path)
    check_retired(raw, retired)
    check_retired(registered, retired)
    if references & retired.keys():
        raise ValueError("current consumers require a retired export")
    expected = {entry.name: entry for entry in registered}
    for name, entry in captured_history(references_path).items():
        if expected.get(name, retired.get(name)) != entry:
            raise ValueError(f"registered definition lost or changed captured historical export: {name}")
    if not references <= expected.keys():
        raise ValueError("registered definition does not match the consumer reference set; regenerate it")
    actual = select_exports(raw, set(expected))
    missing = sorted(expected.keys() - actual.keys())
    changed = sorted(
        name for name in actual.keys() & expected.keys()
        if actual[name] != expected[name]
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


def write_link_definition(historical: Path, output: Path, retired_path: Path | None) -> None:
    retired = retired_exports(retired_path)
    history = parse_exports(historical)
    for entry in history:
        if entry.name in retired and entry != retired[entry.name]:
            raise ValueError(f"retired historical identity mismatch: {entry.name}")
    active = [entry for entry in history if entry.name not in retired]
    # LLD assigns unnumbered exports above the maximum explicit ordinal. Keep
    # retired holes below that maximum; do not create placeholder exports.
    if retired and max(entry.ordinal for entry in active) <= max(entry.ordinal for entry in retired.values()):
        raise ValueError("link definition cannot reserve a retired highest ordinal")
    check_retired(active, retired)
    contents = render_exports(active, hashlib.sha256(historical.read_bytes()).hexdigest())
    output.write_text(contents.replace("; source-ref:", "; historical-sha256:") +
                      "; Link input only: new consumer contract still requires actual Windows raw.\n")
    print(f"WINDOWS_LINK_DEFINITION active={len(active)} retired={len(retired)}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate or check the Windows runtime export definition")
    subparsers = parser.add_subparsers(dest="command", required=True)

    write_parser = subparsers.add_parser("write", help="write a canonical definition from linker --output-def")
    write_parser.add_argument("raw", type=Path)
    write_parser.add_argument("output", type=Path)
    write_parser.add_argument("--source-ref", required=True)
    write_parser.add_argument("--historical", type=Path, help="previous registered definition; defaults to output")

    check_parser = subparsers.add_parser("check", help="compare linker --output-def with the committed definition")
    check_parser.add_argument("raw", type=Path)
    check_parser.add_argument("expected", type=Path)
    check_parser.add_argument("--dll", type=Path)
    check_parser.add_argument("--readobj", default="llvm-readobj")

    collect_parser = subparsers.add_parser("collect", help="capture consumer references from pinned source trees")
    collect_parser.add_argument("raw", type=Path)
    collect_parser.add_argument("output", type=Path)
    collect_parser.add_argument("--consumer", action="append", required=True)
    collect_parser.add_argument("--native-sources", action="append", default=[], metavar="LABEL=MANIFEST")
    collect_parser.add_argument("--runtime-source", required=True, metavar="LABEL@REVISION=DIRECTORY")
    collect_parser.add_argument("--runtime-definitions", type=Path,
                                help="normal Windows runtime COFF object capture; resolves conditional definitions")
    collect_parser.add_argument("--historical", type=Path, default=DEFAULT_HISTORY)
    collect_parser.add_argument("--diagnostics", type=Path, help="retain provenance and missing symbols, even on failure")
    capture_parser = subparsers.add_parser("capture-native", help="capture all configured std native translation units")
    capture_parser.add_argument("build", type=Path)
    capture_parser.add_argument("root", type=Path, help="stdlib/libs source root")
    capture_parser.add_argument("output", type=Path)
    link_parser = subparsers.add_parser("link-definition", help="preserve active historical ordinals at the actual link")
    link_parser.add_argument("historical", type=Path)
    link_parser.add_argument("output", type=Path)
    for subparser in (write_parser, check_parser, collect_parser, link_parser):
        subparser.add_argument("--retired", type=Path, help="explicit reviewed ABI retirements; never inferred from callers")
    for subparser in (write_parser, check_parser):
        subparser.add_argument("--references", type=Path, default=DEFAULT_REFERENCES)

    arguments = parser.parse_args()
    try:
        if arguments.command == "capture-native":
            capture_native_sources(arguments.build, arguments.root, arguments.output)
            return 0
        if arguments.command == "link-definition":
            write_link_definition(arguments.historical, arguments.output, arguments.retired)
            return 0
        if arguments.command == "collect":
            collect_references(arguments.raw, arguments.consumer, arguments.output, arguments.native_sources,
                               arguments.runtime_source, arguments.historical, arguments.diagnostics,
                               arguments.runtime_definitions, arguments.retired)
            return 0
        if arguments.command == "write":
            write_exports(arguments.raw, arguments.output, arguments.source_ref, arguments.references,
                          arguments.historical, arguments.retired)
            return 0
        return check_exports(arguments.raw, arguments.expected, arguments.references, arguments.dll,
                             arguments.readobj, arguments.retired)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"FATAL: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
