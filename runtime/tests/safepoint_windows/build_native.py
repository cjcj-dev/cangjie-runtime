#!/usr/bin/env python3
"""Build the ordinary-safepoint product on a native Windows runner.

Build success is recorded separately from behavioral acceptance.
"""
import difflib
import sys
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
arm = sys.argv[1] if len(sys.argv) > 1 else "candidate"
out = Path(os.environ["RUNNER_TEMP"]) / "safepoint-windows" / arm
out.mkdir(parents=True, exist_ok=True)
# Matrix jobs use isolated machines and the same absolute product paths.
# This also keeps paths embedded by nested builds identical across arms.
tree = Path(os.environ["RUNNER_TEMP"]) / "safepoint-product" / "runtime"
shutil.copytree(source, tree)
stub = tree / "src/arch/x86_64_windows/HandleSafepointStub.S"
before = stub.read_text()
if arm == "cut-home":
    after = before.replace("#define SafepointStubArgumentSaveSize    32",
                           "#define SafepointStubArgumentSaveSize    0")
elif arm == "cut-restore":
    after = before.replace("movapd  -368(%rbp), %xmm15", "movapd  -352(%rbp), %xmm15")
elif arm in ("candidate", "restored"):
    after = before
else:
    raise ValueError(arm)
if arm.startswith("cut"):
    assert after != before, "knife no longer matches"
    stub.write_text(after)
    (out / "cut.diff").write_text("".join(difflib.unified_diff(
        before.splitlines(True), after.splitlines(True),
        fromfile="a/runtime/src/arch/x86_64_windows/HandleSafepointStub.S",
        tofile="b/runtime/src/arch/x86_64_windows/HandleSafepointStub.S")))
env = dict(os.environ, GC_UNIT_GATE_SKIP="1",
           CANGJIE_BUILD_JOBS=str(os.cpu_count()),
           CMAKE_BUILD_PARALLEL_LEVEL=str(os.cpu_count()))
record = {"platform": platform.platform(), "jobs": os.cpu_count(),
          "head": os.environ.get("GITHUB_SHA"), "arm": arm, "behavior": "NOT_RUN"}


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
          "-DCMAKE_AR_PATH=llvm-ar",
          "-DCMAKE_C_FLAGS=-ffile-prefix-map=" + tree.as_posix() + "=/usr/src/cangjie-runtime",
          "-DCMAKE_CXX_FLAGS=-ffile-prefix-map=" + tree.as_posix() + "=/usr/src/cangjie-runtime",
          "-DCMAKE_ASM_FLAGS=-ffile-prefix-map=" + tree.as_posix() + "=/usr/src/cangjie-runtime",
          "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,--no-insert-timestamp", "-DCMAKE_INSTALL_PREFIX=" + str(tree.parent / "install")],
         "configure")
if not rc:
    rc = run(["cmake", "--build", build, "--parallel", str(os.cpu_count())], "build")
# Preserve the linked product even when a later build guard rejects it.
# This does not change the build return code or qualify it for acceptance.
libs = sorted((build / "runtime-staging").rglob("*cangjie-runtime.dll"))
if libs:
    product = out / "product"
    product.mkdir()
    for pattern in ("*.dll", "*.dll.a"):
        for path in (build / "runtime-staging").rglob(pattern):
            shutil.copy2(path, product / path.name)
    record["products"] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in product.iterdir()}
    run(["llvm-nm", "--defined-only", product / libs[0].name], "defined")
    run(["llvm-objdump", "-d", "--disassemble-symbols=CJ_MCC_HandleSafepoint,MRT_UpdateUwContext,HandleSafepoint,MRT_GetThreadLocalData,MRT_DeleteC2NContext", product / libs[0].name], "disassembly")
for path in (build / "windows_x86_64_exports.raw.def", tree / "src/windows_x86_64_exports.def"):
    if path.is_file():
        shutil.copy2(path, out / path.name)
run(["clang++", "--version"], "compiler")
(out / "result.json").write_text(json.dumps(record, indent=2))
raise SystemExit(rc)
