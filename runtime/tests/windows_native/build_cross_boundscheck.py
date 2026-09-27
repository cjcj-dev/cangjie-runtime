#!/usr/bin/env python3
"""Run the real boundscheck CMake subdirectory on Linux with native cross tools.

The parent starts with the runtime's clang selection. The product subdirectory
must retain upstream's MinGW override on the Linux-to-Windows route.
"""
from concurrent.futures import ThreadPoolExecutor
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

source = Path(sys.argv[1]).resolve()
output = Path(sys.argv[2]).resolve()
dependency = source / "third_party/third_party_bounds_checking_function"
assert (dependency / "include/securec.h").is_file()
product = (source / "build/cmake/CMakeLists.txt").read_text()
assert sys.platform == "linux"


def arm(name):
    root = output / name
    root.mkdir(parents=True)
    dep = root / "boundscheck"
    shutil.copytree(dependency, dep)
    text = product
    if name == "cut":
        text = text.replace("if(NOT CMAKE_HOST_WIN32 OR CMAKE_CROSSCOMPILING)", "if(FALSE)")
        assert text != product
        (root / "cut.diff").write_text("".join(difflib.unified_diff(
            product.splitlines(True), text.splitlines(True),
            fromfile="a/runtime/build/cmake/CMakeLists.txt",
            tofile="b/runtime/build/cmake/CMakeLists.txt")))
    (dep / "CMakeLists.txt").write_text(text)
    # Use real cross compilers for CMake's Windows ABI identification. Then
    # expose the same parent clang selection as runtime/config.cmake:205.
    (root / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.19)
project(BoundscheckCross LANGUAGES C CXX)
set(WINDOWS_FLAG 1)
set(SECURE_CFLAG_FOR_SHARED_LIBRARY "$ENV{CFLAGS}")
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/boundscheck/include")
add_subdirectory(boundscheck)
message(STATUS "CROSS_ROUTE=${CMAKE_CROSSCOMPILING} HOST=${CMAKE_HOST_SYSTEM_NAME}")
''')
    record = {"arm": name, "product_cmake_sha256": hashlib.sha256(text.encode()).hexdigest()}
    maps = " ".join(f"-{flag}={root}=/usr/src/cangjie-runtime" for flag in
                    ("ffile-prefix-map", "fdebug-prefix-map", "fmacro-prefix-map"))
    env = dict(os.environ, CCACHE_DIR="/root/.ccache", CCACHE_BASEDIR=str(root), CCACHE_NOHASHDIR="1",
               CFLAGS=maps, CXXFLAGS=maps, ASMFLAGS=maps)

    def run(args, label):
        start = time.monotonic()
        with (root / (label + ".log")).open("w") as log:
            log.write(repr([str(a) for a in args]) + "\n")
            log.flush()
            rc = subprocess.run([str(a) for a in args], env=env, stdout=log, stderr=subprocess.STDOUT).returncode
        record[label] = {"rc": rc, "wall": time.monotonic() - start}
        return rc

    build = root / "build"
    rc = run(["cmake", "-S", root, "-B", build, "-G", "Ninja", "-DCMAKE_SYSTEM_NAME=Windows",
              "-DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc", "-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++",
              "-DCMAKE_C_COMPILER_LAUNCHER=ccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
              "-DCMAKE_ASM_COMPILER_LAUNCHER=ccache", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"], "configure")
    assert rc == 0, record
    rc = run(["cmake", "--build", build, "--parallel", str(os.cpu_count()), "--verbose"], "build")
    objects = sorted((build / "boundscheck").rglob("*.obj"))
    formats = [subprocess.check_output(["file", "-b", obj], text=True).strip() for obj in objects]
    record["objects"] = [{"path": str(obj.relative_to(build)), "file": fmt,
                           "sha256": hashlib.sha256(obj.read_bytes()).hexdigest()}
                          for obj, fmt in zip(objects, formats)]
    object_format_ok = len(objects) == 4 and all(
        ("ELF 64-bit" if name == "cut" else "Intel amd64 COFF") in fmt for fmt in formats)
    dlls = list(build.rglob("*.dll"))
    pe = False
    if rc == 0 and len(dlls) == 1:
        record["dll_sha256"] = hashlib.sha256(dlls[0].read_bytes()).hexdigest()
        run(["file", dlls[0]], "file")
        pe = "PE32+" in (root / "file.log").read_text()
    record.update(assertion_rc=int(rc != 0 or not pe), cross_route="CROSS_ROUTE=TRUE HOST=Linux" in (root / "configure.log").read_text())
    record["valid"] = record["cross_route"] and object_format_ok and (record["assertion_rc"] == (1 if name == "cut" else 0))
    shutil.copy2(build / "compile_commands.json", root / "compile_commands.json")
    (root / "result.json").write_text(json.dumps(record, indent=2))
    print(json.dumps(record), flush=True)
    # Keep failures and published DLL identity; discard successful build trees.
    if name != "cut" and record["valid"]:
        shutil.copy2(dlls[0], root / dlls[0].name)
        shutil.rmtree(build)
    return record["valid"]


with ThreadPoolExecutor(max_workers=3) as pool:
    valid = list(pool.map(arm, ["candidate", "cut", "restored"]))
sys.exit(0 if all(valid) else 1)
