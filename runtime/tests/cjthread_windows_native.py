#!/usr/bin/env python3
"""Exercise the real Windows parent configure, cmd script and native CJThread build.

No compiler or command substitutes are used. Cuts redirect valid product outputs;
their native builds must still succeed and only the intended assertion may fail.
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

BASE = "a891df782f6132909c81afb9ecb4c05a73278d03"
ARM = sys.argv[1]
assert platform.system() == "Windows", "requires native Windows Python"
REPO = Path(__file__).resolve().parents[2]
WORK = Path(os.environ["RUNNER_TEMP"]) / "cjthread-native"
TREE = WORK / "runtime"
LOG = WORK / "evidence"
LOG.mkdir(parents=True, exist_ok=True)
shutil.copytree(REPO / "runtime", TREE)
ENV = dict(os.environ, GC_UNIT_GATE_SKIP="1",
           CMAKE_BUILD_PARALLEL_LEVEL=str(os.cpu_count()),
           CANGJIE_BUILD_JOBS=str(os.cpu_count()))
GIT = shutil.which("git") or str(Path(os.environ["ProgramFiles"]) / "Git/cmd/git.exe")
record = {"arm": ARM, "baseline": BASE, "git_on_path": shutil.which("git"),
          "git": GIT, "cpus": os.cpu_count(), "platform": platform.platform(),
          "boot_time": subprocess.check_output(
              ["powershell.exe", "-NoProfile", "-Command",
               "(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('o')"], text=True).strip()}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, name, env=ENV):
    start = time.monotonic()
    with (LOG / (name + ".log")).open("w") as log:
        log.write(repr([str(x) for x in command]) + "\n")
        log.flush()
        proc = subprocess.run([str(x) for x in command], cwd=TREE, env=env,
                              stdout=log, stderr=subprocess.STDOUT, timeout=600)
    record[name] = {"rc": proc.returncode, "wall": time.monotonic() - start}
    print(name, record[name], flush=True)
    return proc.returncode


def replace(path, old, new):
    text = path.read_text()
    assert text.count(old) == 1, (path, old)
    changed = text.replace(old, new)
    path.write_text(changed)
    relative = "runtime/" + path.relative_to(TREE).as_posix()
    (LOG / "cut.diff").write_text("".join(difflib.unified_diff(
        text.splitlines(True), changed.splitlines(True),
        fromfile="a/" + relative, tofile="b/" + relative)))


bat = TREE / "build/build_cjthread_windows.bat"
config = TREE / "config.cmake"
parent = TREE / "CMakeLists.txt"
dependency = TREE / "third_party/third_party_bounds_checking_function"
url = "https://gitcode.com/openharmony/third_party_bounds_checking_function"
if ARM == "baseline":
    # Restore the exact baseline build inputs; all other runtime sources are the
    # same candidate tree, isolating the two build entry fixes.
    for path in [bat, config]:
        relative = "runtime/" + path.relative_to(TREE).as_posix()
        path.write_bytes(subprocess.check_output([GIT, "-C", REPO, "show", BASE + ":" + relative]))
    # Isolate the path failure from the separately tested acquisition failure.
    assert run([GIT, "clone", "--depth", "1", "--branch", "OpenHarmony-v6.0-Release",
                url, dependency], "dependency-seed") == 0
elif ARM == "cut-path":
    replace(bat, '    cd /d "%BUILD_PATH%"', '    cd /d "%PROJECT_PATH%"')
elif ARM == "cut-output":
    replace(parent, 'set(OTHER_DEFINITIONS " -DRUNTIME_OUTPUT_ROOT=${CMAKE_OUTPUT_DIRECTORY}")',
            'set(OTHER_DEFINITIONS " -DRUNTIME_OUTPUT_ROOT=${CMAKE_OUTPUT_DIRECTORY}/cut-output")')
elif ARM == "incomplete":
    dependency.mkdir(parents=True)
elif ARM == "clone-failure":
    # A real git clone of an empty repository lacks the requested release tag.
    # Redirect only this child's URL via Git's documented per-process config.
    empty = WORK / "empty.git"
    assert run([GIT, "init", "--bare", empty], "empty-repository") == 0
    ENV.update(GIT_CONFIG_COUNT="1",
               GIT_CONFIG_KEY_0="url." + empty.as_posix() + ".insteadOf",
               GIT_CONFIG_VALUE_0=url)
elif ARM not in ["green", "restored"]:
    raise ValueError(ARM)

record["source_sha256"] = {str(p.relative_to(TREE)): digest(p) for p in [bat, config, parent]}
build = TREE / "CMakebuild"
command = ["cmake", "-S", TREE, "-B", build, "-G", "Ninja",
           "-DWINDOWS_FLAG=1", "-DCOPYGC_FLAG=1", "-DDOPRA_FLAG=1",
           "-DCMAKE_BUILD_TYPE=Release", "-DRUNTIME_TRACE_FLAG=1",
           "-DCJ_SDK_VERSION=0.0.1", "-DDISABLE_VERSION_CHECK=1",
           "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
           "-DCMAKE_AR_PATH=llvm-ar", "-DCMAKE_INSTALL_PREFIX=" + str(WORK / "install")]
rc = run(command, "configure")
text = (LOG / "configure.log").read_text(errors="replace")
checks = {}
if ARM in ["incomplete", "clone-failure"]:
    marker = "Boundscheck dependency is incomplete:" if ARM == "incomplete" else "Boundscheck clone failed:"
    checks["dependency_diagnostic"] = rc != 0 and marker in text
    checks["no_child_on_invalid_dependency"] = "build cjthread for windows on windows" not in text
    checks["no_dependency_overlay_on_failure"] = not (dependency / "CMakeLists.txt").exists()
    expected_failures = []
else:
    checks["native_configure"] = rc == 0
    caches = [p for p in TREE.rglob("CMakeCache.txt")
              if "CMAKE_PROJECT_NAME:STATIC=cjthread" in p.read_text(errors="replace")]
    checks["one_child_cache"] = len(caches) == 1
    cache = caches[0] if len(caches) == 1 else None
    checks["child_directory"] = cache == build / "cjthread-build/CMakeCache.txt"
    values = {}
    if cache:
        shutil.copy2(cache, LOG / "child-CMakeCache.txt")
        for line in cache.read_text(errors="replace").splitlines():
            if ":" in line and "=" in line and not line.startswith(("#", "//")):
                key, value = line.split("=", 1)
                values[key.split(":", 1)[0]] = value
    output = values.get("RUNTIME_OUTPUT_ROOT")
    checks["output_directory"] = bool(output) and Path(output) == build / "runtime-staging"
    archives = list(TREE.rglob("libcangjie-thread.a"))
    checks["native_archive"] = len(archives) == 1
    if len(archives) == 1:
        archive = archives[0]
        record["archive"] = {"path": str(archive), "sha256": digest(archive)}
        checks["native_archive"] = run(["llvm-readobj", "--file-headers", archive], "archive-identity") == 0
        checks["native_archive"] &= "Format: COFF-x86-64" in (LOG / "archive-identity.log").read_text()
    checks["dependency_consumed"] = (dependency / "include/securec.h").is_file() and "securec.h' file not found" not in text
    if (dependency / "include/securec.h").is_file():
        record["securec_sha256"] = digest(dependency / "include/securec.h")
    expected_failures = {"baseline": ["child_directory", "output_directory"],
                         "cut-path": ["child_directory"],
                         "cut-output": ["output_directory"]}.get(ARM, [])

failures = sorted(key for key, passed in checks.items() if not passed)
for key, passed in checks.items():
    print(f"NATIVE_ASSERT {key} {'PASS' if passed else 'FAIL'}", flush=True)
record.update(checks=checks, failures=failures, expected_failures=sorted(expected_failures),
              assertion_rc=int(bool(failures)), utc_end=time.time())
record["valid"] = failures == sorted(expected_failures)
(LOG / "result.json").write_text(json.dumps(record, indent=2))
print("NATIVE_RESULT", json.dumps(record), flush=True)
sys.exit(0 if record["valid"] else 1)
