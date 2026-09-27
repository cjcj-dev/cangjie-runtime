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

def build(arm):
    return subprocess.run([sys.executable, here / "build_native.py", arm]).returncode

arms = ("candidate", "cut-home", "cut-restore", "restored")
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    builds = dict(zip(arms, pool.map(build, arms)))
(out / "builds.json").write_text(json.dumps(builds, indent=2))
if any(builds.values()):
    sys.exit(2)
tree = out / "candidate/runtime"
product = out / "candidate/product"
includes = ["src", "src/Loader/BinaryFile", "src/Heap", "src/Heap/z/os/windows",
            "src/CJThread/src/runtime/schedule/include", "include",
            "third_party/third_party_bounds_checking_function/include", "src/os/Windows"]
exe = out / "sentinel.exe"
command = ["clang++", "-std=gnu++17", "-O0", "-g", "-pthread", "-fno-rtti",
           "-fno-omit-frame-pointer", "-fvisibility-inlines-hidden"]
command += ["-I" + str(tree / x) for x in includes]
command += [here / "sentinel.cpp", here / "bridge.S", "-L" + str(product),
            "-lcangjie-runtime", "-lboundscheck", "-o", exe]
with (out / "link.log").open("w") as log:
    rc = subprocess.run([str(x) for x in command], stdout=log, stderr=subprocess.STDOUT).returncode
(out / "link.json").write_text(json.dumps({"rc": rc, "command": list(map(str, command))}))
if rc:
    sys.exit(3)
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
    valid = (len(targets) == 2 and rc == (1 if arm.startswith("cut") else 0))
    if arm == "cut-restore":
        valid &= "pass=1" in targets[0] and "pass=0" in targets[1] if len(targets) == 2 else False
    dll = next(local.parent.glob("*cangjie-runtime.dll"))
    return {"arm": arm, "rc": rc, "targets": targets, "valid": valid,
            "wall": time.monotonic()-start, "exe_sha256": sha(local),
            "dll_sha256": sha(dll), "dll": str(dll)}

with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    results = list(pool.map(execute, arms))
(out / "behavior.json").write_text(json.dumps(results, indent=2))
print(json.dumps(results, indent=2), flush=True)
sys.exit(0 if all(r["valid"] and r["exe_sha256"] == exe_sha for r in results) else 1)
