#!/usr/bin/env python3
"""Run the same native sentinel EXE against separately built product DLLs."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

here = Path(__file__).resolve().parent
out = Path(os.environ["RUNNER_TEMP"]) / "safepoint-windows"

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

arms = ("candidate", "cut-home", "cut-restore", "restored")
tree = Path(os.environ["RUNNER_TEMP"]) / "safepoint-product" / "runtime"
product = out / "candidate/product"
includes = ["src", "src/Loader/BinaryFile", "src/Heap", "src/Heap/z/os/windows",
            "CMakebuild/runtime-staging/include", "include",
            "third_party/third_party_bounds_checking_function/include", "src/os/Windows"]
exe = out / "sentinel.exe"
command = ["clang++", "-std=gnu++17", "-O0", "-g", "-pthread", "-fno-rtti",
           "-fno-omit-frame-pointer", "-fvisibility-inlines-hidden"]
command += ["-I" + str(tree / x) for x in includes]
command += [here / "sentinel.cpp", here / "bridge.S", "-L" + str(product),
            "-lcangjie-runtime", "-lboundscheck", "-o", exe]
if sys.argv[1:] == ["--link"]:
    with (out / "link.log").open("w") as log:
        rc = subprocess.run([str(x) for x in command], stdout=log, stderr=subprocess.STDOUT).returncode
    (out / "link.json").write_text(json.dumps({"rc": rc, "command": list(map(str, command))}))
    sys.exit(rc)
exe_sha = sha(exe)

def execute(arm):
    home = out / arm
    local = home / "product" / exe.name
    shutil.copy2(exe, local)
    env = dict(os.environ)
    env["PATH"] = str(local.parent) + os.pathsep + env["PATH"]
    start = time.monotonic()
    with (home / "sentinel.log").open("w") as log:
        rc = subprocess.run([local], cwd=local.parent, env=env, stdout=log,
                            stderr=subprocess.STDOUT, timeout=60).returncode
    targets = [x for x in (home / "sentinel.log").read_text().splitlines() if x.startswith("SIMD_TARGET ")]
    dll = next(local.parent.glob("*cangjie-runtime.dll"))
    expected_simd = [True, arm != "cut-restore"]
    valid = (len(targets) == 2 and rc == int(arm == "cut-restore") and
             all(t.endswith("pass=" + str(int(p))) for t, p in zip(targets, expected_simd)))
    # Release callees need not use their caller's home area. Observe its live
    # boundaries without altering the inferior, independently of SIMD values.
    env["SAFEPOINT_PRODUCT"] = str(dll)
    env["SAFEPOINT_HOME_RESULT"] = str(home / "home.json")
    with (home / "home.log").open("w") as log:
        home_rc = subprocess.run(["gdb", "--batch", "-nx", "-q", "-ex", "set pagination off",
                                  "-ex", "set confirm off", "-ex", "start",
                                  "-x", str(here / "observe_home.py"), str(local)],
                                 cwd=local.parent, env=env, stdout=log,
                                 stderr=subprocess.STDOUT, timeout=60).returncode
    observation = json.loads((home / "home.json").read_text()) if (home / "home.json").exists() else {}
    entries = observation.get("entries", [])
    home_valid = (home_rc == int(arm == "cut-home") and observation.get("complete") and
                  observation.get("dll_sha256") == sha(dll) and
                  observation.get("inferior_rc") == rc and len(entries) == 4 and
                  all(e["frame_valid"] and e["passed"] == (arm != "cut-home") for e in entries))
    return {"arm": arm, "rc": rc, "targets": targets, "valid": valid,
            "home_rc": home_rc, "home_valid": bool(home_valid), "home": observation,
            "wall": time.monotonic()-start, "exe_sha256": sha(local),
            "dll_sha256": sha(dll), "dll": str(dll)}

with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    results = list(pool.map(execute, arms))
(out / "behavior.json").write_text(json.dumps(results, indent=2))
print(json.dumps(results, indent=2), flush=True)
products = {arm: {p.name: sha(p) for p in (out / arm / "product").glob("*.dll")} for arm in arms}
runtime = "libcangjie-runtime.dll"
identity = {
    "products": products,
    "restored_equal": products["candidate"] == products["restored"],
    "cuts_differ": all(products[a][runtime] != products["candidate"][runtime]
                       for a in ("cut-home", "cut-restore")),
    "other_products_equal": all({k: v for k, v in products[a].items() if k != runtime} ==
                                {k: v for k, v in products["candidate"].items() if k != runtime}
                                for a in arms),
}
(out / "identity.json").write_text(json.dumps(identity, indent=2))
sys.exit(0 if all(r["valid"] and r["home_valid"] and r["exe_sha256"] == exe_sha for r in results)
         and all(identity[k] for k in ("restored_equal", "cuts_differ", "other_products_equal")) else 1)
