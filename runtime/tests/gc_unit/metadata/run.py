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
import time


def invoke(exe, case, timeout=30, environment=None):
    started = time.time_ns()
    monotonic = time.monotonic()
    child = None
    try:
        child = subprocess.Popen([str(exe), case], stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True,
                                 errors="replace", env=environment)
        print("EXEC_START", case, "pid=" + str(child.pid), "utc_ns=" + str(started), flush=True)
        stdout, stderr = child.communicate(timeout=timeout)
        result = {"rc": child.returncode, "output": stdout + stderr, "pid": child.pid}
    except subprocess.TimeoutExpired:
        child.kill()  # exact process owned by this invocation
        stdout, stderr = child.communicate()
        result = {"rc": child.returncode, "output": stdout + stderr,
                  "pid": child.pid, "timeout": True, "launch_error": True}
    except OSError as error:
        result = {"rc": None, "output": str(error), "launch_error": True}
    result.update(start_ns=started, exit_ns=time.time_ns(), wall=time.monotonic() - monotonic)
    print("EXEC_EXIT", case, "rc=" + str(result["rc"]), "utc_ns=" + str(result["exit_ns"]), flush=True)
    return result


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


def run(exe, destination, exclude=(), only=(), skip_controls=False):
    exported = cases(os.name == "nt", exe)
    selected = [case for case in exported if case not in exclude and (not only or case in only)]
    if not selected or any(case not in exported for case in only):
        raise RuntimeError("empty or unregistered selected ManagedMetadata set")
    representative = "ManagedMetadata.RootsRecordedZeroRoots"
    if representative in selected:
        selected.remove(representative)
        selected.insert(0, representative)
    windows = os.name == "nt"
    if windows:
        import ctypes
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    report = {"exported": exported, "selected": selected, "cases": {},
              "controls_skipped": skip_controls, "first_failure": None, "not_started": list(selected)}
    def save():
        destination.write_text(json.dumps(report, indent=2))
    save()
    abort_rc = 3 if windows else -6
    if not skip_controls:
        control = invoke(exe, "--abort-control")
        valid_control = (not control.get("launch_error") and "ABORT_CONTROL" in control["output"] and
                         control["rc"] in ((3, -1073740791, 1073740791, 3221226505) if windows else (-6,)))
        report.update(abort_control=control, abort_control_valid=valid_control)
        save()
        if not valid_control:
            report["first_failure"] = "abort-control"
            save()
            return 1
        abort_rc = control["rc"]
        # Explicitly authorized negative calibration precedes all product targets.
        rejected = {
            "unknown_filter": invoke(exe, "ManagedMetadata.Unknown"),
            "missing_executable": invoke(exe.with_name("metadata-missing-executable"), selected[0]),
            "timeout": invoke(exe, "--timeout-control", timeout=0.1),
            "non_target_exception": invoke(exe, "--non-target-control"),
        }
        if windows:
            with tempfile.TemporaryDirectory(prefix="missing-library-", dir=exe.parent.parent) as temporary:
                orphan = Path(temporary) / exe.name
                shutil.copy2(exe, orphan)
                isolated = dict(os.environ)
                isolated["PATH"] = os.pathsep.join(p for p in isolated["PATH"].split(os.pathsep)
                                                    if Path(p).resolve() != exe.parent.resolve())
                rejected["missing_dll"] = invoke(orphan, selected[0], environment=isolated)
        preflight = {name: not accepts(selected[0], result, windows, abort_rc)
                     for name, result in rejected.items()}
        report.update(preflight_inputs=rejected, preflight_rejected=preflight)
        save()
        if rejected["unknown_filter"]["rc"] != 64 or not all(preflight.values()):
            report["first_failure"] = "negative-calibration"
            save()
            return 1
    # No submitted backlog: only the current case can have started at first failure.
    for case in selected:
        result = invoke(exe, case)
        loaded = re.search(r"^RUNTIME_MODULE (.+)$", result["output"], re.MULTILINE)
        expected_module = exe.parent / ("libcangjie-runtime.dll" if windows else "libcangjie-runtime.dylib" if os.uname().sysname == "Darwin" else "libcangjie-runtime.so")
        result["module_matches"] = bool(loaded and Path(loaded[1].strip()).resolve() == expected_module.resolve())
        result["pass"] = result["module_matches"] and accepts(case, result, windows, abort_rc)
        if case == representative:
            result["pass"] = result["pass"] and all(marker in result["output"] for marker in (
                "METADATA_PUBLIC_CLASSIFICATION managed=1 native=0 qualified=1",
                "ROOTS_RECORDED_CONSUMER_TARGET returned=1 done=0",
                "EH_QUALIFIED_CONSUMER_TARGET restored=1 executed=1"))
        print("SUPERVISOR_ASSERT", case, "PASS" if result["pass"] else "FAIL", "assertion-executed", flush=True)
        print(result["output"], flush=True)
        report["cases"][case] = result
        report["not_started"].remove(case)
        if not result["pass"]:
            report["first_failure"] = {"case": case, "observed_ns": time.time_ns()}
            save()
            print("STOP_SUBMISSION", case, "not_started=" + str(len(report["not_started"])), flush=True)
            return 1
        save()
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("exe", type=Path)
    parser.add_argument("result", type=Path)
    parser.add_argument("--exclude", action="append", default=[])
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--skip-controls", action="store_true")
    args = parser.parse_args()
    raise SystemExit(run(args.exe.resolve(), args.result, args.exclude, args.only, args.skip_controls))
