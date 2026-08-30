#!/usr/bin/env python3
# 冻结合同：
# ① 本 lint 的保证范围 = 「当前七行 manifest 所声明的 direct-call structural identity」；
#    即：该 test 的函数体内存在对该 declaration 的直接调用。
# ② 它不保证：运行时可达性、数据流、间接调用（函数指针/虚派发/回调）、宏生成的调用。
# ③ 行为尺在别处：七刀 cut matrix（每轮跑）+ gc_unit；那才是“测试是否穿过产品”的证据。
# ④ 不许再扩这个 lint：若发现新的失效形态，正确动作是记账并依赖行为尺，
#    不是给这个 lint 加一层。
"""Validate mutual-wait manifest consumers against the compiler AST.

The compiler, rather than this script, performs C++ line splicing, macro
expansion, conditional compilation, tokenization, parsing, and name lookup.
This script only walks the resulting JSON AST and compares resolved call
expressions with the independently fixed manifest rows.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Iterator


@dataclass(frozen=True)
class ManifestRow:
    test_name: str
    anchor: str
    carrier: str
    consumer: str
    cut_site: str
    consumer_symbol: str
    consumer_arity: int
    consumer_owner: str
    consumer_declaration: str


def _load_manifest(path: Path) -> list[ManifestRow]:
    lines = [
        (line_number, line)
        for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1)
        if line and not line.startswith("#")
    ]
    if not lines:
        raise ValueError("empty manifest")
    expected_header = [
        "test_name", "stable_product_anchor", "carrier", "consumer_callsite",
        "cut_site", "consumer_symbol", "consumer_arity", "consumer_owner",
        "consumer_declaration",
    ]
    header = lines[0][1].split("\t")
    if header != expected_header:
        raise ValueError("manifest header does not match the AST declaration schema")
    rows: list[ManifestRow] = []
    for line_number, line in lines[1:]:
        fields = line.split("\t")
        if len(fields) != len(header):
            raise ValueError(f"manifest line {line_number} has {len(fields)} columns")
        try:
            arity = int(fields[6])
        except ValueError as error:
            raise ValueError(f"manifest line {line_number} has non-integer arity") from error
        owner, declaration = fields[7:9]
        rows.append(ManifestRow(*fields[:6], arity, owner, declaration))
    return rows


def _decode_json_stream(text: str) -> list[Any]:
    decoder = json.JSONDecoder()
    values: list[Any] = []
    offset = 0
    while offset < len(text):
        while offset < len(text) and text[offset].isspace():
            offset += 1
        if offset == len(text):
            break
        value, offset = decoder.raw_decode(text, offset)
        values.append(value)
    return values


def _children(node: Any) -> Iterable[dict[str, Any]]:
    if not isinstance(node, dict):
        return ()
    return (child for child in node.get("inner", []) if isinstance(child, dict))


def _walk(node: Any) -> Iterator[dict[str, Any]]:
    if not isinstance(node, dict):
        return
    yield node
    for child in _children(node):
        yield from _walk(child)


def _function_body(node: dict[str, Any]) -> dict[str, Any] | None:
    if node.get("kind") != "FunctionDecl":
        return None
    return next((child for child in _children(node) if child.get("kind") == "CompoundStmt"), None)


def _resolved_reference_ids(node: dict[str, Any]) -> set[str]:
    declaration_ids: set[str] = set()
    for reference in _walk(node):
        member = reference.get("referencedMemberDecl")
        if reference.get("kind") == "MemberExpr" and isinstance(member, str):
            declaration_ids.add(member)
        referenced = reference.get("referencedDecl")
        if isinstance(referenced, dict) and referenced.get("kind") in {
            "FunctionDecl", "CXXMethodDecl", "CXXConversionDecl",
        } and isinstance(referenced.get("id"), str):
            declaration_ids.add(referenced["id"])
    return declaration_ids


def _resolved_callee_ids(call: dict[str, Any]) -> set[str]:
    children = list(_children(call))
    if not children:
        return set()
    # The first resolved function/member reference in preorder is the direct
    # callee.  Do not union references from the receiver expression: in
    # ``make_manager().target()`` that would incorrectly admit make_manager as
    # an alternative identity for target.
    for node in _walk(children[0]):
        member = node.get("referencedMemberDecl")
        if node.get("kind") == "MemberExpr" and isinstance(member, str):
            return {member}
        referenced = node.get("referencedDecl")
        if isinstance(referenced, dict) and referenced.get("kind") in {
            "FunctionDecl", "CXXMethodDecl", "CXXConversionDecl",
        } and isinstance(referenced.get("id"), str):
            return {referenced["id"]}
    return set()


def _contains_resolved_call(body: dict[str, Any], declaration_ids: set[str], arity: int) -> bool:
    call_kinds = {"CallExpr", "CXXMemberCallExpr", "CXXOperatorCallExpr"}
    for node in _walk(body):
        if node.get("kind") not in call_kinds:
            continue
        children = list(_children(node))
        if _resolved_callee_ids(node) & declaration_ids and len(children) - 1 == arity:
            return True
    return False


def _probe_function_name(row: ManifestRow, index: int) -> str:
    suite = row.test_name.split(".", 1)[0]
    return f"{suite}__MutualWaitResolvedDeclaration_{index}"


def _write_probe_translation_unit(source: Path, rows: list[ManifestRow], directory: Path) -> Path:
    """Include the real TU and resolve every manifest declaration in that AST.

    Pointer casts in ``consumer_declaration`` disambiguate overloads.  Because
    probes and calls are compiled in one clang invocation, their opaque AST ids
    are directly comparable and never persisted as unstable pointer strings.
    """
    source_name = str(source.resolve()).replace("\\", "\\\\").replace('"', '\\"')
    lines = [f'#include "{source_name}"', ""]
    for index, row in enumerate(rows):
        lines.extend([
            f"void {_probe_function_name(row, index)}()",
            "{",
            f"    (void)({row.consumer_declaration});",
            "}",
            "",
        ])
    wrapper = directory / "mutualwait_resolved_declaration_probe.cpp"
    wrapper.write_text("\n".join(lines), encoding="utf-8")
    return wrapper


def _product_texts(root: Path) -> list[str]:
    suffixes = {".c", ".cc", ".cpp", ".h", ".hpp", ".inl"}
    texts: list[str] = []
    for path in root.rglob("*"):
        if path.is_file() and path.suffix in suffixes:
            try:
                texts.append(path.read_text(encoding="utf-8"))
            except (OSError, UnicodeError):
                continue
    return texts


def _consumer_schema_matches(row: ManifestRow) -> bool:
    """Keep the human-readable callsite and AST key as one manifest fact."""
    opening = row.consumer.find("(")
    if opening <= 0 or not row.consumer.endswith(")"):
        return False
    callee = row.consumer[:opening].rsplit(".", 1)[-1].rsplit("->", 1)[-1].rsplit("::", 1)[-1]
    arguments = row.consumer[opening + 1:-1].strip()
    arity = 0 if not arguments else arguments.count(",") + 1
    return (callee == row.consumer_symbol and arity == row.consumer_arity
            and bool(row.consumer_owner)
            and not row.consumer_owner.startswith("::")
            and not row.consumer_owner.endswith("::")
            and f"&{row.consumer_owner}::{row.consumer_symbol}" in row.consumer_declaration)


def _emit_error(token: str, row: ManifestRow | None = None, detail: str = "") -> None:
    parts = [token]
    if row is not None:
        parts.append(f"test_name={row.test_name}")
    if detail:
        parts.append(f"detail={detail}")
    print(" ".join(parts), file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--product-root", required=True, type=Path)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--compile-arg", action="append", default=[])
    parser.add_argument("--expected-test", action="append", default=[])
    args = parser.parse_args()

    try:
        rows = _load_manifest(args.manifest)
    except (OSError, UnicodeError, ValueError) as error:
        _emit_error("GC_UNIT_MUTUALWAIT_MANIFEST_ERROR", detail=str(error))
        return 6

    expected = args.expected_test
    actual = [row.test_name for row in rows]
    status = 0
    if len(actual) != len(expected):
        _emit_error("GC_UNIT_MUTUALWAIT_ROW_COUNT_MISMATCH",
                    detail=f"rows={len(actual)} expected={len(expected)}")
        status = 1
    if len(set(actual)) != len(actual):
        _emit_error("GC_UNIT_MUTUALWAIT_DUPLICATE_TEST", detail="manifest test names are not unique")
        status = 1
    for test_name in expected:
        if test_name not in actual:
            _emit_error("GC_UNIT_MUTUALWAIT_EXPECTED_TEST_MISSING", detail=f"test_name={test_name}")
            status = 1
    for row in rows:
        if row.carrier not in {"product_so_dlsym", "product_so_link"}:
            _emit_error("GC_UNIT_MUTUALWAIT_UNKNOWN_CARRIER", row, row.carrier)
            status = 1
        if not _consumer_schema_matches(row):
            _emit_error("GC_UNIT_MUTUALWAIT_CONSUMER_SCHEMA_MISMATCH", row,
                        f"consumer={row.consumer} symbol={row.consumer_symbol} arity={row.consumer_arity}")
            status = 1

    suite_prefixes = {row.test_name.split(".", 1)[0] + "_" for row in rows if "." in row.test_name}
    if len(suite_prefixes) != 1:
        _emit_error("GC_UNIT_MUTUALWAIT_AST_FILTER_ERROR", detail="manifest spans zero or multiple suites")
        return 6
    try:
        with tempfile.TemporaryDirectory(prefix="mutualwait-ast-") as temporary:
            wrapper = _write_probe_translation_unit(args.source, rows, Path(temporary))
            command = [
                args.compiler, "-Xclang", "-ast-dump=json", "-Xclang",
                f"-ast-dump-filter={next(iter(suite_prefixes))}", "-fsyntax-only",
                *args.compile_arg, str(wrapper),
            ]
            proc = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, check=False)
    except OSError as error:
        _emit_error("GC_UNIT_MUTUALWAIT_AST_TOOL_ERROR", detail=str(error))
        return 6
    if proc.returncode != 0:
        _emit_error("GC_UNIT_MUTUALWAIT_AST_COMPILE_ERROR", detail=f"rc={proc.returncode}")
        sys.stderr.write(proc.stderr)
        return 6
    try:
        ast_roots = _decode_json_stream(proc.stdout)
    except json.JSONDecodeError as error:
        _emit_error("GC_UNIT_MUTUALWAIT_AST_JSON_ERROR", detail=str(error))
        return 6

    definitions: dict[str, list[dict[str, Any]]] = {}
    for root in ast_roots:
        for node in _walk(root):
            body = _function_body(node)
            name = node.get("name")
            if body is not None and isinstance(name, str):
                definitions.setdefault(name, []).append(body)
    product_texts = _product_texts(args.product_root)
    for index, row in enumerate(rows):
        function_name = row.test_name.replace(".", "_", 1)
        bodies = definitions.get(function_name, [])
        probe_bodies = definitions.get(_probe_function_name(row, index), [])
        target_ids = _resolved_reference_ids(probe_bodies[0]) if len(probe_bodies) == 1 else set()
        row_ok = True
        if not bodies:
            _emit_error("GC_UNIT_MUTUALWAIT_TEST_MISSING", row)
            row_ok = False
        elif len(bodies) != 1:
            _emit_error("GC_UNIT_MUTUALWAIT_TEST_AMBIGUOUS", row, f"count={len(bodies)}")
            row_ok = False
        elif len(probe_bodies) != 1 or not target_ids:
            _emit_error("GC_UNIT_MUTUALWAIT_DECLARATION_MISSING", row,
                        f"owner={row.consumer_owner} declaration={row.consumer_declaration} "
                        f"probe_count={len(probe_bodies)}")
            row_ok = False
        elif not _contains_resolved_call(bodies[0], target_ids, row.consumer_arity):
            _emit_error("GC_UNIT_MUTUALWAIT_CONSUMER_MISSING", row,
                        f"consumer={row.consumer} owner={row.consumer_owner} "
                        f"symbol={row.consumer_symbol} declaration={row.consumer_declaration} "
                        f"arity={row.consumer_arity}")
            row_ok = False
        anchor_symbol = row.anchor.rsplit("::", 1)[-1]
        if not any(anchor_symbol in text for text in product_texts):
            _emit_error("GC_UNIT_MUTUALWAIT_ANCHOR_MISSING", row, row.anchor)
            row_ok = False
        if not any(row.cut_site in text for text in product_texts):
            _emit_error("GC_UNIT_MUTUALWAIT_CUT_SITE_MISSING", row, row.cut_site)
            row_ok = False
        if row_ok:
            print(f"GATE_MUTUALWAIT_PRODUCT_MANIFEST_ROW_OK test_name={row.test_name}")
        else:
            status = 1
    if status:
        return 5
    print(f"GATE_MUTUALWAIT_PRODUCT_MANIFEST_OK rows={len(rows)} "
          "source=clear_entries_product_unit.cpp analyzer=clang-resolved-declaration-id")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
