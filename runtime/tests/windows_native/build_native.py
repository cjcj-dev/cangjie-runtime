#!/usr/bin/env python3
"""Build the runtime product on a native Windows runner.

Build success is recorded separately from behavioral acceptance.
"""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time

assert platform.system() == "Windows", "requires native Windows"
source = Path(__file__).resolve().parents[2]
out = Path(os.environ["RUNNER_TEMP"]) / "runtime-windows"
out.mkdir(parents=True, exist_ok=True)
tree = out / "runtime"
shutil.copytree(source, tree)
env = dict(os.environ, GC_UNIT_GATE_SKIP="1",
           CANGJIE_BUILD_JOBS=str(os.cpu_count()),
           CMAKE_BUILD_PARALLEL_LEVEL=str(os.cpu_count()))
record = {"platform": platform.platform(), "jobs": os.cpu_count(),
          "head": os.environ.get("GITHUB_SHA"), "behavior": "NOT_RUN"}


def run(command, name):
    start = time.monotonic()
    with (out / (name + ".log")).open("w") as log:
        log.write(repr([str(x) for x in command]) + "\n")
        log.flush()
        rc = subprocess.run([str(x) for x in command], cwd=tree, env=env,
                            stdout=log, stderr=subprocess.STDOUT).returncode
    record[name] = {"rc": rc, "wall": time.monotonic() - start}
    (out / "result.json").write_text(json.dumps(record, indent=2))
    print(name, record[name], flush=True)
    return rc


build = tree / "CMakebuild"
rc = run(["cmake", "-S", tree, "-B", build, "-G", "Ninja",
          "-DWINDOWS_FLAG=1", "-DCOPYGC_FLAG=1", "-DDOPRA_FLAG=1",
          "-DCMAKE_BUILD_TYPE=Release", "-DRUNTIME_TRACE_FLAG=1",
          "-DCJ_SDK_VERSION=0.0.1", "-DDISABLE_VERSION_CHECK=1",
          "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
          "-DCMAKE_AR_PATH=llvm-ar", "-DCMAKE_INSTALL_PREFIX=" + str(out / "install")],
         "configure")
if not rc:
    rc = run(["cmake", "--build", build, "--parallel", str(os.cpu_count()), "--", "-k", "0"], "build")
if not rc:
    libs = list(tree.rglob("*cangjie-runtime.dll"))
    assert libs, "product DLL missing"
    product = out / "product"
    product.mkdir()
    for path in libs[0].parent.iterdir():
        if path.is_file() and path.suffix in (".dll", ".a"):
            shutil.copy2(path, product / path.name)
    record["products"] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in product.iterdir()}
    rc = run(["file", libs[0]], "file")
    if not rc:
        rc = run(["llvm-nm", "--defined-only", libs[0]], "defined")
    (out / "result.json").write_text(json.dumps(record, indent=2))
raise SystemExit(rc)
