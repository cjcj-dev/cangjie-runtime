#!/usr/bin/env python3
"""Fault arms for the stable-symbol VerifyPhase matrix checker."""

from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


TARGETS = (
    ("roots", "VerifyRoots.cpp", 0),
    ("objects", "VerifyHeap.cpp", 0),
    ("marking", "MarkCompleteVerify.cpp", 0),
    ("remembered", "VerifyRememberedSet.cpp", 0),
    ("oops_prepare", "VerifyRegions.cpp", 0),
    ("oops_mark", "VerifyRegions.cpp", 1),
)

OBJECTS_CALL = "VerifyPhaseEnter(VerifyFace::Objects, point)"
CPP_GAP = r"(?:\s|\\\r?\n)*"


def objects_call_pattern():
    return re.compile(
        rf"\bVerifyPhaseEnter{CPP_GAP}\({CPP_GAP}VerifyFace{CPP_GAP}"
        rf"::{CPP_GAP}Objects{CPP_GAP},{CPP_GAP}point{CPP_GAP}\)"
    )


def replace_objects_call(text: str, replacement: str) -> str:
    text, count = objects_call_pattern().subn(lambda _: replacement, text, count=1)
    if count != 1:
        raise RuntimeError("expected exactly one Objects admission")
    return text


def replace_call(path: Path, face: str, occurrence: int) -> None:
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(
        rf"\bVerifyPhaseEnter{CPP_GAP}\({CPP_GAP}VerifyFace{CPP_GAP}"
        rf"::{CPP_GAP}{face.capitalize()}{CPP_GAP},"
    )
    matches = list(pattern.finditer(text))
    if occurrence >= len(matches):
        raise RuntimeError(
            f"missing {face} admission occurrence {occurrence}; found {len(matches)}"
        )
    start = matches[occurrence].start()
    path.write_text(
        text[:start] + "VerifyPhaseEnterCut" + text[start + len("VerifyPhaseEnter") :],
        encoding="utf-8",
    )


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
        declaration = re.compile(r"\bvoid\s+VerifyHeapObjects\s*\(")
        changed, count = declaration.subn("void\n  VerifyHeapObjects (", text, count=1)
        if count != 1:
            raise RuntimeError("expected exactly one Objects function definition")
        source.write_text(changed, encoding="utf-8")
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
        ("if_zero_unsigned_token", f"\n#if 0U\n{OBJECTS_CALL}\n#endif\nfalse"),
        (
            "if_zero_expression_token",
            f"\n#if (1 - 1)\n{OBJECTS_CALL}\n#endif\nfalse",
        ),
    )
    for arm, replacement in replacements:
        with tempfile.TemporaryDirectory(prefix=f"verify_phase_{arm}_") as temporary:
            verify_root = copy_verify(repo, temporary)
            source = verify_root / "VerifyHeap.cpp"
            text = source.read_text(encoding="utf-8")
            source.write_text(replace_objects_call(text, replacement), encoding="utf-8")
            rc, faces, output = run_checker(checker, verify_root)
            red = {name for name, state in faces.items() if state == "RED"}
            ok = rc == 1 and red == {"objects"}
            print_arm(arm, rc, faces, output, ok)
            failures += not ok

    # Backslash-newline is removed before preprocessing tokens are formed, so
    # this is the same product call and must retain the stable identity.
    with tempfile.TemporaryDirectory(prefix="verify_phase_spliced_call_") as temporary:
        verify_root = copy_verify(repo, temporary)
        source = verify_root / "VerifyHeap.cpp"
        text = source.read_text(encoding="utf-8")
        source.write_text(
            replace_objects_call(
                text, "VerifyPhaseEnter\\\n(VerifyFace::Objects, point)"
            ),
            encoding="utf-8",
        )
        rc, faces, output = run_checker(checker, verify_root)
        ok = rc == 0 and set(faces.values()) == {"GREEN"}
        print_arm("spliced_call", rc, faces, output, ok)
        failures += not ok

    # Calls spelled through a local object-like macro are the same preprocessed
    # product call and must not be rejected as a textual identity change.
    with tempfile.TemporaryDirectory(prefix="verify_phase_macro_call_") as temporary:
        verify_root = copy_verify(repo, temporary)
        source = verify_root / "VerifyHeap.cpp"
        text = source.read_text(encoding="utf-8")
        changed = replace_objects_call(
            text, "VERIFY_PHASE_ENTER(VerifyFace::Objects, point)"
        )
        source.write_text(
            "#define VERIFY_PHASE_ENTER VerifyPhaseEnter\n" + changed,
            encoding="utf-8",
        )
        rc, faces, output = run_checker(checker, verify_root)
        ok = rc == 0 and set(faces.values()) == {"GREEN"}
        print_arm("macro_call", rc, faces, output, ok)
        failures += not ok

    for arm, source_name, occurrence in TARGETS:
        face = arm.split("_", 1)[0]
        with tempfile.TemporaryDirectory(prefix=f"verify_phase_{arm}_") as temporary:
            verify_root = copy_verify(repo, temporary)
            replace_call(verify_root / source_name, face, occurrence)
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

    print(f"VERIFY_PHASE_FAULT_ARMS total=18 failures={failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
