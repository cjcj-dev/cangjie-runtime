#!/usr/bin/env python3
"""Run the real metadata executable; record every target assertion, fail closed."""
import argparse
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


def cases(windows):
    names = (["WinCurrentAbsentDescriptor", "WinCurrentAbsentStackMap", "WinCurrentPresent",
              "WinCallerAbsentDescriptor", "WinCallerAbsentStackMap", "WinCallerPresent"] if windows else
             ["CallerSpAbsentStackMap", "CallerSpNative", "CallerSpPresent"])
    return ["ManagedMetadata." + name for name in names]


def accepts(case, child, windows, abort_rc):
    output = child["output"]
    if child.get("launch_error") or "METADATA_CHILD " + case not in output:
        return False
    address = re.search(r"METADATA_INPUT startPC=(\S+) ip=(\S+)", output)
    if not address:
        return False
    if windows and "Absent" in case:
        diagnostic = "funcdesc" if "Descriptor" in case else "stackmap"
        expected = "managed frame missing " + diagnostic + " startPC=" + address[1] + " ip=" + address[2]
        return child["rc"] == abort_rc and expected in output and "METADATA_COMPLETE" not in output
    if child["rc"] != 0 or "METADATA_COMPLETE " + case not in output:
        return False
    if windows:
        return "WINDOWS_FRAME" in output and "assertion-executed" in output
    if "Absent" in case:
        expected = "managed frame missing stackmap startPC=" + address[1] + " ip=" + address[2]
        if expected not in output:
            return False
    return "target=1 assertion-executed" in output and (
        "Absent" in case or "CALLER_SP" in output)


def run(exe, destination):
    windows = os.name == "nt"
    if windows:
        import ctypes
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    control = invoke(exe, "--abort-control")
    # MSVCRT/UCRT abort convention is calibrated with this exact executable.
    valid_control = (not control.get("launch_error") and "ABORT_CONTROL" in control["output"] and
                     control["rc"] in ((3, -1073740791, 1073740791, 3221226505) if windows else (-6,)))
    results = {}
    for case in cases(windows):
        result = invoke(exe, case)
        loaded = re.search(r"^RUNTIME_MODULE (.+)$", result["output"], re.MULTILINE)
        expected_module = exe.parent / ("libcangjie-runtime.dll" if windows else "libcangjie-runtime.so")
        result["module_matches"] = bool(loaded and Path(loaded[1].strip()).resolve() == expected_module.resolve())
        result["pass"] = (valid_control and result["module_matches"] and
                          accepts(case, result, windows, control["rc"]))
        print("SUPERVISOR_ASSERT", case, "PASS" if result["pass"] else "FAIL", "assertion-executed", flush=True)
        print(result["output"], flush=True)
        results[case] = result
    unknown = invoke(exe, "ManagedMetadata.Unknown")
    missing = invoke(exe.with_name("metadata-missing-executable"), cases(windows)[0])
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
            rejected["missing_dll"] = invoke(orphan, cases(windows)[0], environment=isolated)
    preflight = {name: not accepts(cases(windows)[0], result, windows, control["rc"])
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
    args = parser.parse_args()
    raise SystemExit(run(args.exe.resolve(), args.result))
