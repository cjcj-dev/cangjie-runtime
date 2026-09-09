#!/usr/bin/env python3
"""Fault arms for the stable-symbol VerifyPhase matrix checker."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


TARGETS = (
    ("roots", "VerifyRoots.cpp", "void VerifyRoots::VerifyRootPayload("),
    ("objects", "VerifyHeap.cpp", "void VerifyHeapObjects("),
    ("marking", "MarkCompleteVerify.cpp", "void RunAtMarkEnd("),
    ("remembered", "VerifyRememberedSet.cpp", "void VerifyRememberedSetInvariant("),
    ("oops_prepare", "VerifyRegions.cpp", "void VerifyRegions::VerifyAfterPrepareYoung("),
    ("oops_mark", "VerifyRegions.cpp", "void VerifyRegions::VerifyAfterYoungMark("),
)

OBJECTS_DECLARATION = "void VerifyHeapObjects("
OBJECTS_CALL = "VerifyPhaseEnter(VerifyFace::Objects, point)"


def function_span(text: str, anchor: str):
    if text.count(anchor) != 1:
        raise RuntimeError(f"expected one function anchor {anchor!r}")
    start = text.find(anchor)
    opening = text.find("{", start + len(anchor))
    depth = 0
    for position in range(opening, len(text)):
        if text[position] == "{":
            depth += 1
        elif text[position] == "}":
            depth -= 1
            if depth == 0:
                return start, position + 1
    raise RuntimeError(f"unterminated function {anchor!r}")


def replace_call(path: Path, anchor: str, face: str) -> None:
    text = path.read_text(encoding="utf-8")
    start, end = function_span(text, anchor)
    body = text[start:end]
    call = f"VerifyPhaseEnter(VerifyFace::{face.capitalize()},"
    if body.count(call) != 1:
        raise RuntimeError(f"expected one {call!r} in {anchor!r}")
    body = body.replace(call, f"VerifyPhaseEnterCut(VerifyFace::{face.capitalize()},", 1)
    path.write_text(text[:start] + body + text[end:], encoding="utf-8")


def run_checker(checker: Path, verify_root: Path):
    process = subprocess.run(
        [sys.executable, str(checker), str(verify_root)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    matrix_line = next(
        line
        for line in process.stdout.splitlines()
        if line.startswith("VERIFY_PHASE_MATRIX ")
    )
    faces = dict(item.split("=", 1) for item in matrix_line.split()[1:])
    return process.returncode, faces, process.stdout


def print_arm(name: str, rc: int, faces: dict, output: str, ok: bool) -> None:
    red = sorted(face for face, state in faces.items() if state == "RED")
    print(f"ARM={name} RC={rc} RED={red} RESULT={'PASS' if ok else 'FAIL'}")
    print(output, end="" if output.endswith("\n") else "\n")


def copy_verify(repo: Path, temporary: str) -> Path:
    destination = Path(temporary) / "Verify"
    shutil.copytree(repo / "runtime/src/Heap/Verify", destination)
    return destination


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <candidate-repo>", file=sys.stderr)
        return 2
    repo = Path(sys.argv[1]).resolve()
    checker = repo / "runtime/tests/gc_unit/check_verify_phase_matrix.py"
    failures = 0

    # Both supported entries must invoke the real checker and these independent
    # fault arms exactly once.  Because this test itself follows the checker in
    # each entry, deleting only the checker call makes that same entry fail here.
    runner_calls = {}
    for runner_name in ("run_standalone.sh", "gate_gc_unit.sh"):
        runner_text = (repo / "runtime/tests/gc_unit" / runner_name).read_text(
            encoding="utf-8"
        )
        runner_calls[runner_name] = (
            runner_text.count('python3 "$SRC/check_verify_phase_matrix.py"'),
            runner_text.count('python3 "$SRC/test_verify_phase_matrix_checker.py"'),
        )
    wiring_ok = all(calls == (1, 1) for calls in runner_calls.values())
    print(
        f"ARM=runner_wiring RC={0 if wiring_ok else 1} "
        f"CALLS={runner_calls} RESULT={'PASS' if wiring_ok else 'FAIL'}"
    )
    failures += not wiring_ok

    with tempfile.TemporaryDirectory(prefix="verify_phase_baseline_") as temporary:
        verify_root = copy_verify(repo, temporary)
        rc, faces, output = run_checker(checker, verify_root)
        ok = rc == 0 and set(faces.values()) == {"GREEN"}
        print_arm("baseline", rc, faces, output, ok)
        failures += not ok

    # Moving every current product line must not change a symbol-keyed identity.
    with tempfile.TemporaryDirectory(prefix="verify_phase_line_shift_") as temporary:
        verify_root = copy_verify(repo, temporary)
        for source in verify_root.glob("*.cpp"):
            source.write_text("\n" + source.read_text(encoding="utf-8"), encoding="utf-8")
        rc, faces, output = run_checker(checker, verify_root)
        ok = rc == 0 and set(faces.values()) == {"GREEN"}
        print_arm("line_shift", rc, faces, output, ok)
        failures += not ok

    # Symbol identity is token-based: harmless declaration formatting is not an
    # identity change.
    with tempfile.TemporaryDirectory(prefix="verify_phase_whitespace_") as temporary:
        verify_root = copy_verify(repo, temporary)
        source = verify_root / "VerifyHeap.cpp"
        text = source.read_text(encoding="utf-8")
        source.write_text(
            text.replace(OBJECTS_DECLARATION, "void\n  VerifyHeapObjects (", 1),
            encoding="utf-8",
        )
        rc, faces, output = run_checker(checker, verify_root)
        ok = rc == 0 and set(faces.values()) == {"GREEN"}
        print_arm("signature_whitespace", rc, faces, output, ok)
        failures += not ok

    # A token in a comment, literal, or definitely disabled preprocessor branch
    # is not an executable admission. Each arm removes the real Objects call and
    # must therefore make exactly that face red.
    replacements = (
        ("comment_token", f"false /* {OBJECTS_CALL} */"),
        ("string_token", f'false && "{OBJECTS_CALL}"'),
        ("if_zero_token", f"\n#if 0\n{OBJECTS_CALL}\n#endif\nfalse"),
    )
    for arm, replacement in replacements:
        with tempfile.TemporaryDirectory(prefix=f"verify_phase_{arm}_") as temporary:
            verify_root = copy_verify(repo, temporary)
            source = verify_root / "VerifyHeap.cpp"
            text = source.read_text(encoding="utf-8")
            if text.count(OBJECTS_CALL) != 1:
                raise RuntimeError("expected exactly one Objects admission")
            source.write_text(text.replace(OBJECTS_CALL, replacement, 1), encoding="utf-8")
            rc, faces, output = run_checker(checker, verify_root)
            red = {name for name, state in faces.items() if state == "RED"}
            ok = rc == 1 and red == {"objects"}
            print_arm(arm, rc, faces, output, ok)
            failures += not ok

    for arm, source_name, anchor in TARGETS:
        face = arm.split("_", 1)[0]
        with tempfile.TemporaryDirectory(prefix=f"verify_phase_{arm}_") as temporary:
            verify_root = copy_verify(repo, temporary)
            replace_call(verify_root / source_name, anchor, face)
            rc, faces, output = run_checker(checker, verify_root)
            red = {name for name, state in faces.items() if state == "RED"}
            ok = rc == 1 and red == {face}
            print_arm(arm, rc, faces, output, ok)
            failures += not ok

    # Exactness works in both directions: an unregistered call is not allowed to
    # borrow the identity of a declared target that happens to use the same face.
    with tempfile.TemporaryDirectory(prefix="verify_phase_unexpected_") as temporary:
        verify_root = copy_verify(repo, temporary)
        source = verify_root / "VerifyPhase.cpp"
        source.write_text(
            source.read_text(encoding="utf-8")
            + "\nvoid UnexpectedVerifyPhaseTarget()\n"
            + "{\n    VerifyPhaseEnter(VerifyFace::Roots, \"unexpected\");\n}\n",
            encoding="utf-8",
        )
        rc, faces, output = run_checker(checker, verify_root)
        red = {name for name, state in faces.items() if state == "RED"}
        ok = rc == 1 and red == {"roots"} and "VerifyPhase.cpp:roots" in output
        print_arm("unexpected", rc, faces, output, ok)
        failures += not ok

    print(f"VERIFY_PHASE_FAULT_ARMS total=14 failures={failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
