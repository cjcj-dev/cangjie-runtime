#!/usr/bin/env python3
"""Run the real metadata executable; record every target assertion, fail closed."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import subprocess
import shutil
import tempfile


def invoke(exe, case, timeout=30, environment=None):
    try:
        child = subprocess.run([str(exe), case], capture_output=True, text=True,
                               errors="replace", timeout=timeout, env=environment)
        return {"rc": child.returncode, "output": child.stdout + child.stderr}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"rc": None, "output": str(error), "launch_error": True}


def cases(windows, exe=None):
    if exe is not None:
        exported = invoke(exe, "--list")
        if exported["rc"] != 0:
            raise RuntimeError("metadata registration export failed: " + exported["output"])
        names = [line for line in exported["output"].splitlines() if line.startswith("ManagedMetadata.")]
        if not names or len(names) != len(set(names)):
            raise RuntimeError("empty or duplicate ManagedMetadata registration")
        return names

    names = (["WinCurrentAbsentDescriptor", "WinCurrentAbsentStackMap", "WinCurrentPresent",
              "WinCallerAbsentDescriptor", "WinCallerAbsentStackMap", "WinCallerPresent"] if windows else
             ["CallerSpAbsentStackMap", "CallerSpNative", "CallerSpPresent"])
    return ["ManagedMetadata." + name for name in names]


def accepts(case, child, windows, abort_rc):
    output = child["output"]
    if child.get("launch_error") or "METADATA_CHILD " + case not in output:
        return False
    if not windows:
        if child["rc"] != 0 or "METADATA_COMPLETE " + case not in output:
            return False
        # These original tests already assert the product result. Require their
        # visible consumer output as well as the registry completion marker.
        specific = {
            "TextDescriptorIsRegistered": "METADATA_TEXT_PC_TARGET code=1 data=1 owned=1 executed=1",
            "ExecutableWithoutDescriptorIsNative": "METADATA_EXECUTABLE_CLASSIFICATION_TARGET native=1 executed=1",
            "UnregisteredPCIsNative": "METADATA_UNREGISTERED_ROOTS_TARGET",
            "DataAddressIsNotCode": "METADATA_DATA_PC_TARGET registered=1 code=0",
            "RootsMissingQualification": "A2_ROOTS_RESULT done=0",
            "RootsZeroQualification": "A2_ROOTS_RESULT done=0",
        }
        name = case.split(".", 1)[1]
        marker = specific.get(name)
        if marker is not None:
            return marker in output and (name == "UnregisteredPCIsNative" or "METADATA_REGISTERED header=" in output)
        return ("METADATA_REGISTERED header=" in output and
                "target=1 assertion-executed" in output)
    address = re.search(r"METADATA_INPUT startPC=(\S+) ip=(\S+)", output)
    if not address or "METADATA_REGISTERED header=" not in output:
        return False
    if "Absent" in case:
        diagnostic = "funcdesc" if "Descriptor" in case else "stackmap"
        expected = "managed frame missing " + diagnostic + " startPC=" + address[1] + " ip=" + address[2]
        return child["rc"] == abort_rc and expected in output and "METADATA_COMPLETE" not in output
    return (child["rc"] == 0 and "METADATA_COMPLETE " + case in output and
            "WINDOWS_FRAME" in output and "assertion-executed" in output)


def run(exe, destination, exclude=()):
    selected = [case for case in cases(os.name == "nt", exe) if case not in exclude]
    if not selected:
        raise RuntimeError("empty selected ManagedMetadata set")
    windows = os.name == "nt"
    if windows:
        import ctypes
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    control = invoke(exe, "--abort-control")
    # MSVCRT/UCRT abort convention is calibrated with this exact executable.
    valid_control = (not control.get("launch_error") and "ABORT_CONTROL" in control["output"] and
                     control["rc"] in ((3, -1073740791, 1073740791, 3221226505) if windows else (-6,)))
    results = {}
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
        pending = {case: pool.submit(invoke, exe, case) for case in selected}
    for case, future in pending.items():
        result = future.result()
        loaded = re.search(r"^RUNTIME_MODULE (.+)$", result["output"], re.MULTILINE)
        expected_module = exe.parent / ("libcangjie-runtime.dll" if windows else "libcangjie-runtime.dylib" if os.uname().sysname == "Darwin" else "libcangjie-runtime.so")
        result["module_matches"] = bool(loaded and Path(loaded[1].strip()).resolve() == expected_module.resolve())
        result["pass"] = (valid_control and result["module_matches"] and
                          accepts(case, result, windows, control["rc"]))
        print("SUPERVISOR_ASSERT", case, "PASS" if result["pass"] else "FAIL", "assertion-executed", flush=True)
        print(result["output"], flush=True)
        results[case] = result
    unknown = invoke(exe, "ManagedMetadata.Unknown")
    missing = invoke(exe.with_name("metadata-missing-executable"), selected[0])
    timeout = invoke(exe, "--timeout-control", timeout=0.1)
    non_target = invoke(exe, "--non-target-control")
    rejected = {"missing_executable": missing, "timeout": timeout,
                "non_target_exception": non_target, "unknown_filter": unknown}
    if windows:
        with tempfile.TemporaryDirectory(prefix="missing-library-", dir=exe.parent.parent) as temporary:
            orphan = Path(temporary) / exe.name
            shutil.copy2(exe, orphan)
            isolated = dict(os.environ)
            isolated["PATH"] = os.pathsep.join(p for p in isolated["PATH"].split(os.pathsep)
                                                if Path(p).resolve() != exe.parent.resolve())
            rejected["missing_dll"] = invoke(orphan, selected[0], environment=isolated)
    preflight = {name: not accepts(selected[0], result, windows, control["rc"])
                 for name, result in rejected.items()}
    report = {"preflight_rejected": preflight, "preflight_inputs": rejected, "abort_control": control, "abort_control_valid": valid_control,
              "unknown_filter": unknown, "cases": results}
    destination.write_text(json.dumps(report, indent=2))
    return int(not valid_control or unknown["rc"] != 64 or not all(preflight.values()) or
               not all(x["pass"] for x in results.values()))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("exe", type=Path)
    parser.add_argument("result", type=Path)
    parser.add_argument("--exclude", action="append", default=[])
    args = parser.parse_args()
    raise SystemExit(run(args.exe.resolve(), args.result, args.exclude))
