#!/usr/bin/env python3
"""Build the runtime product on a native Windows runner.

Build success is recorded separately from behavioral acceptance.
"""
import difflib
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

arm = sys.argv[1] if len(sys.argv) > 1 else "candidate"
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


# Each negative arm restores a real POSIX dependency in one product TU.
# This is a native compilation contract, not a GC behavioral test.
cut_targets = {
    "cut-compiler": ("build/cmake/CMakeLists.txt", "memset_s.c.obj", "x86_64-w64-mingw32-gcc"),
    "cut-region": ("src/Heap/Allocator/RegionSpace.h", "BaseObject.cpp.obj", "sys/mman.h"),
    "cut-filler": ("src/Heap/shared/collectedHeap.cpp", "collectedHeap.cpp.obj", "sys/mman.h"),
    "cut-limit": ("src/Heap/z/zAddressSpaceLimit.cpp", "zAddressSpaceLimit.cpp.obj", "sys/resource.h"),
}
if arm in cut_targets:
    relative, target_suffix, header = cut_targets[arm]
    path = tree / relative
    before = path.read_text()
    if arm == 'cut-compiler':
        after = before.replace('if(NOT CMAKE_HOST_WIN32)', 'if(TRUE)')
    elif arm == 'cut-region':
        after = before.replace('#include <memory>\n', '#include <memory>\n#include <sys/mman.h>\n')
    elif arm == 'cut-filler':
        after = before.replace('#ifdef _WIN64\n#include <memoryapi.h>\n#else\n#include <sys/mman.h>\n#endif', '#include <sys/mman.h>')
    else:
        after = before.replace('#ifndef _WIN64\n#include <sys/resource.h>\n#endif', '#include <sys/resource.h>')
    assert after != before
    path.write_text(after)
    (out / "cut.diff").write_text("".join(difflib.unified_diff(
        before.splitlines(True), after.splitlines(True),
        fromfile="a/runtime/" + relative, tofile="b/runtime/" + relative)))
elif arm not in ("candidate", "restored"):
    raise ValueError(arm)
record["source_sha256"] = {str(p.relative_to(tree)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (tree / "src").rglob("*") if p.is_file()}

build = tree / "CMakebuild"
rc = run(["cmake", "-S", tree, "-B", build, "-G", "Ninja",
          "-DWINDOWS_FLAG=1", "-DCOPYGC_FLAG=1", "-DDOPRA_FLAG=1",
          "-DCMAKE_BUILD_TYPE=Release", "-DRUNTIME_TRACE_FLAG=1",
          "-DCJ_SDK_VERSION=0.0.1", "-DDISABLE_VERSION_CHECK=1",
          "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
          "-DCMAKE_AR_PATH=llvm-ar", "-DCMAKE_INSTALL_PREFIX=" + str(out / "install")],
         "configure")
if not rc:
    for system in (build / "CMakeFiles").glob("*/CMakeSystem.cmake"):
        shutil.copy2(system, out / "CMakeSystem.cmake")
if not rc and arm in cut_targets:
    targets = subprocess.check_output(["ninja", "-C", build, "-t", "targets", "all"], text=True)
    matches = [line.split(": ", 1)[0] for line in targets.splitlines()
               if line.split(": ", 1)[0].endswith("/" + target_suffix)]
    assert len(matches) == 1, matches
    record["target"] = matches[0]
    rc = run(["cmake", "--build", build, "--target", matches[0]], "build")
    text = (out / "build.log").read_text(errors="replace")
    diagnostic = "CreateProcess failed" if arm == "cut-compiler" else "file not found"
    exact = rc != 0 and header in text and diagnostic in text and target_suffix[:-4] in text
    record.update(assertion_rc=int(rc != 0), expected_compile_failure=exact)
    (out / "result.json").write_text(json.dumps(record, indent=2))
    print("NATIVE_COMPILE_ASSERT", arm, "FAIL" if rc else "PASS", "expected=", exact, flush=True)
    raise SystemExit(0 if exact else 1)
if not rc:
    rc = run(["cmake", "--build", build, "--parallel", str(os.cpu_count()), "--", "-k", "0"], "build")
build_rc = rc
raw_exports = build / "windows_x86_64_exports.raw.def"
if raw_exports.is_file():
    shutil.copy2(raw_exports, out / raw_exports.name)
    record["raw_exports_sha256"] = hashlib.sha256(raw_exports.read_bytes()).hexdigest()
libs = list(tree.rglob("*cangjie-runtime.dll"))
if not build_rc:
    assert libs, "product DLL missing"
if libs:
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
raise SystemExit(build_rc or rc)
