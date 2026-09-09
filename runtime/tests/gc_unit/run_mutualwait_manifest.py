#!/usr/bin/env python3
"""Run the mutual-wait AST check once for a default/filler arm pair.

The default arm always executes the analyzer and publishes a one-use receipt.
The filler arm may consume that receipt only when every structural input has
the same content identity.  A filler-only invocation therefore still runs the
analyzer, and a later default invocation never inherits an earlier PASS.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Iterable


SCHEMA_VERSION = 1
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inc", ".inl", ".def"}


def _hash_file(digest: "hashlib._Hash", label: str, path: Path) -> None:
    digest.update(label.encode("utf-8"))
    digest.update(b"\0")
    digest.update(str(path).encode("utf-8"))
    digest.update(b"\0")
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    digest.update(b"\0")


def _source_files(root: Path, suffixes: set[str]) -> Iterable[Path]:
    if not root.is_dir():
        return ()
    return (
        path for path in sorted(root.rglob("*"))
        if path.is_file() and path.suffix.lower() in suffixes
    )


def _argument_value(arguments: list[str], option: str) -> str:
    prefix = option + "="
    for index, argument in enumerate(arguments):
        if argument.startswith(prefix):
            return argument[len(prefix):]
        if argument == option and index + 1 < len(arguments):
            return arguments[index + 1]
    raise ValueError(f"missing analyzer option {option}")


def _compile_arguments(arguments: list[str]) -> list[str]:
    compile_arguments: list[str] = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "--compile-arg" and index + 1 < len(arguments):
            compile_arguments.append(arguments[index + 1])
            index += 2
            continue
        if argument.startswith("--compile-arg="):
            compile_arguments.append(argument.split("=", 1)[1])
        index += 1
    return compile_arguments


def _include_directories(arguments: list[str]) -> list[Path]:
    directories: list[Path] = []
    arguments = _compile_arguments(arguments)
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "-I" and index + 1 < len(arguments):
            directories.append(Path(arguments[index + 1]).resolve())
            index += 2
            continue
        if argument.startswith("-I") and len(argument) > 2:
            directories.append(Path(argument[2:]).resolve())
        index += 1
    return directories


def _compiler_identity(compiler: str) -> tuple[Path, bytes]:
    resolved_name = shutil.which(compiler)
    if resolved_name is None:
        raise OSError(f"compiler not found: {compiler}")
    path = Path(resolved_name).resolve()
    version = subprocess.run(
        [str(path), "--version"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        check=False,
    )
    if version.returncode != 0:
        raise OSError(f"compiler --version failed: {path} rc={version.returncode}")
    return path, version.stdout


def input_identity(analyzer: Path, arguments: list[str]) -> str:
    source = Path(_argument_value(arguments, "--source")).resolve()
    manifest = Path(_argument_value(arguments, "--manifest")).resolve()
    product_root = Path(_argument_value(arguments, "--product-root")).resolve()
    compiler_name = _argument_value(arguments, "--compiler")
    compiler, compiler_version = _compiler_identity(compiler_name)

    digest = hashlib.sha256()
    digest.update(json.dumps(arguments, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
    digest.update(b"\0compiler-version\0")
    digest.update(compiler_version)
    _hash_file(digest, "runner", Path(__file__).resolve())
    _hash_file(digest, "analyzer", analyzer.resolve())
    _hash_file(digest, "source", source)
    _hash_file(digest, "manifest", manifest)
    _hash_file(digest, "compiler", compiler)

    seen: set[Path] = {source, manifest, analyzer.resolve(), Path(__file__).resolve(), compiler}
    roots = [(product_root, {".c", ".cc", ".cpp", ".h", ".hpp", ".inl"})]
    roots.extend((directory, SOURCE_SUFFIXES) for directory in _include_directories(arguments))
    for root, suffixes in roots:
        for path in _source_files(root, suffixes):
            resolved = path.resolve()
            if resolved in seen:
                continue
            seen.add(resolved)
            _hash_file(digest, "tree-input", resolved)
    return digest.hexdigest()


def _write_receipt(path: Path, record: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(record, output, sort_keys=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_name, path)
    finally:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass


def _read_receipt(path: Path) -> dict[str, object] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, OSError, UnicodeError, json.JSONDecodeError):
        return None
    if not isinstance(value, dict) or value.get("schema_version") != SCHEMA_VERSION:
        return None
    return value


def _run_analyzer(analyzer: Path, arguments: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(analyzer), *arguments], text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arm", required=True, choices=("default", "filler"))
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument("--analyzer", required=True, type=Path)
    parser.add_argument("analyzer_args", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    analyzer_args = args.analyzer_args
    if analyzer_args[:1] == ["--"]:
        analyzer_args = analyzer_args[1:]

    try:
        identity = input_identity(args.analyzer, analyzer_args)
    except (OSError, ValueError) as error:
        print(f"GC_UNIT_MUTUALWAIT_IDENTITY_ERROR detail={error}", file=sys.stderr)
        return 6

    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    lock_path = args.receipt.with_suffix(args.receipt.suffix + ".lock")
    with lock_path.open("a", encoding="utf-8") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if args.arm == "filler":
            receipt = _read_receipt(args.receipt)
            try:
                args.receipt.unlink()
            except FileNotFoundError:
                pass
            if receipt is not None and receipt.get("input_identity") == identity:
                rc = int(receipt.get("analyzer_rc", 6))
                if rc == 0:
                    print(f"GATE_MUTUALWAIT_AST_RECEIPT_CONSUMED arm=filler input_identity={identity}")
                    return 0
                print(
                    f"GC_UNIT_MUTUALWAIT_AST_RECEIPT_FAILURE arm=filler "
                    f"input_identity={identity} analyzer_rc={rc}", file=sys.stderr,
                )
                sys.stdout.write(str(receipt.get("stdout", "")))
                sys.stderr.write(str(receipt.get("stderr", "")))
                return rc
            print(f"GATE_MUTUALWAIT_AST_RECEIPT_MISS arm=filler input_identity={identity}")
        else:
            try:
                args.receipt.unlink()
            except FileNotFoundError:
                pass

        print(f"GATE_MUTUALWAIT_AST_EXECUTE arm={args.arm} input_identity={identity}")
        process = _run_analyzer(args.analyzer, analyzer_args)
        sys.stdout.write(process.stdout)
        sys.stderr.write(process.stderr)
        if args.arm == "default":
            _write_receipt(args.receipt, {
                "schema_version": SCHEMA_VERSION,
                "input_identity": identity,
                "analyzer_rc": process.returncode,
                "stdout": process.stdout if process.returncode != 0 else "",
                "stderr": process.stderr if process.returncode != 0 else "",
            })
            print(
                f"GATE_MUTUALWAIT_AST_RECEIPT_PUBLISHED arm=default "
                f"input_identity={identity} analyzer_rc={process.returncode}"
            )
        return process.returncode


if __name__ == "__main__":
    raise SystemExit(main())
