#!/usr/bin/env python3
"""Official-runner product build and isolated CHECK deletion arms for #1284."""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import tarfile
import subprocess
import time

import run as supervisor

parser = argparse.ArgumentParser()
parser.add_argument("mode", choices=["prepare", "arm", "capability"])
parser.add_argument("--config", choices=["default", "testable", "pac-off", "pac-on"], default="testable")
parser.add_argument("--arm", default="candidate")
args = parser.parse_args()
source = Path(__file__).resolve().parents[3]
out = Path(os.environ["RUNNER_TEMP"]) / "metadata"
out.mkdir(parents=True, exist_ok=True)
bundle = out / "bundle"
record = {"head": os.environ.get("GITHUB_SHA"), "platform": platform.platform(),
          "machine": platform.machine(), "ImageOS": os.environ.get("ImageOS"),
          "ImageVersion": os.environ.get("ImageVersion"), "cpu_count": os.cpu_count(),
          "config": args.config, "arm": args.arm, "behavior": "NOT_RUN"}
windows = platform.system() == "Windows"
mac = platform.system() == "Darwin"
env = dict(os.environ, GC_UNIT_GATE_SKIP="1", CMAKE_BUILD_PARALLEL_LEVEL=str(os.cpu_count()),
           SOURCE_DATE_EPOCH="1790640000", ZERO_AR_DATE="1", SCCACHE_IDLE_TIMEOUT="0")


def save():
    (out / "build-result.json").write_text(json.dumps(record, indent=2))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, label, cwd=None):
    start = time.monotonic()
    with (out / (label + ".log")).open("w") as log:
        log.write(repr([str(x) for x in command]) + "\n")
        log.flush()
        rc = subprocess.run([str(x) for x in command], env=env, cwd=cwd,
                            stdout=log, stderr=subprocess.STDOUT).returncode
    record[label] = {"rc": rc, "wall": time.monotonic() - start}
    save()
    print(label, record[label], flush=True)
    if rc:
        lines = (out / (label + ".log")).read_text(errors="replace").splitlines()
        errors = [line for line in lines if "error" in line.lower() or "undefined" in line.lower()]
        print("\n".join(errors[:25] or lines[-15:]), flush=True)
    return rc


def checked(command, label, cwd=None):
    rc = run(command, label, cwd)
    if rc:
        raise SystemExit(rc)


# Only public standard runners of the requested architecture qualify.
assert os.environ.get("GITHUB_REPOSITORY_VISIBILITY") == "public", "public repository required"
assert platform.machine().lower() in (("amd64", "x86_64") if windows else ("arm64", "aarch64"))
checked(["clang", "--version"], "compiler")

cuts = {
    "caller-sp": ("src/UnwindStack/FrameInfo.cpp", "desc->GetStackMap() != nullptr", 0, "CallerSpAbsentStackMap"),
    "current-desc": ("src/os/Windows/UnwindWin.cpp", "funcDesc != nullptr", 0, "WinCurrentAbsentDescriptor"),
    "current-map": ("src/os/Windows/UnwindWin.cpp", "funcDesc->GetStackMap() != nullptr", 0, "WinCurrentAbsentStackMap"),
    "caller-desc": ("src/os/Windows/UnwindWin.cpp", "funcDesc != nullptr", 1, "WinCallerAbsentDescriptor"),
    "caller-map": ("src/os/Windows/UnwindWin.cpp", "funcDesc->GetStackMap() != nullptr", 1, "WinCallerAbsentStackMap"),
}

if args.mode == "arm" and args.arm in ("candidate", "device"):
    pass
else:
    tree = out / "runtime"
    if args.arm == "baseline":
        base = "575e37b5f271065e08eb401a38fe60976f71a925"
        checked(["git", "fetch", "--depth=1", "origin", base], "fetch-base", source.parent)
        archive = out / "baseline.tar"
        checked(["git", "archive", "--format=tar", "--output=" + str(archive), base + ":runtime"],
                "archive-base", source.parent)
        with tarfile.open(archive) as package:
            package.extractall(tree, filter="data")
        archive.unlink()
        record["baseline_sha"] = base
    else:
        shutil.copytree(source, tree)
    product_sources = {str(p.relative_to(tree)): digest(p) for p in (tree / "src").rglob("*") if p.is_file()}
    record["uncut_product_source_sha256"] = hashlib.sha256(json.dumps(product_sources, sort_keys=True).encode()).hexdigest()
    if args.arm in cuts:
        relative, condition, index, _ = cuts[args.arm]
        path = tree / relative
        before = path.read_text()
        lines = before.splitlines(True)
        hits = [i for i, line in enumerate(lines) if "CHECK_DETAIL(" + condition + "," in line]
        assert len(hits) == (1 if args.arm == "caller-sp" else 2), hits
        i = hits[index]
        assert lines[i + 1].rstrip().endswith(");")
        del lines[i:i + 2]
        after = "".join(lines)
        path.write_text(after)
        (out / "cut.diff").write_text("".join(difflib.unified_diff(before.splitlines(True), lines,
            fromfile="a/runtime/" + relative, tofile="b/runtime/" + relative)))
    build = tree / "CMakebuild"
    flags = ["-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_ASM_COMPILER=clang",
             "-DCMAKE_C_COMPILER_LAUNCHER=sccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=sccache",
             "-DCMAKE_ASM_COMPILER_LAUNCHER=sccache", "-DCMAKE_BUILD_TYPE=Release", "-DCOPYGC_FLAG=1",
             "-DDOPRA_FLAG=1", "-DDISABLE_VERSION_CHECK=1", "-DRUNTIME_TRACE_FLAG=1", "-DCJ_SDK_VERSION=0.0.1",
             "-DWINDOWS_FLAG=" + str(int(windows)), "-DMACOS_FLAG=" + str(int(mac)), "-DOHOS_FLAG=0",
             "-DANDROID_FLAG=0", "-DIOS_FLAG=0", "-DIOS_SIMULATOR_FLAG=0", "-DCMAKE_AR_PATH=" + ("llvm-ar" if windows else "ar"),
             "-DMRT_TESTABLE_INTERNALS=" + ("ON" if args.config == "testable" else "OFF"),
             "-DMRT_GC_UNIT_TESTS=OFF", "-DRUNTIME_BACKWARD_PTRAUTH_CFI=" + str(int(args.config == "pac-on"))]
    prefix = "-ffile-prefix-map=" + str(tree) + "=/usr/src/cangjie-runtime"
    flags += ["-DCMAKE_" + kind + "_FLAGS=" + prefix for kind in ("C", "CXX", "ASM")]
    if windows:
        flags += ["-DCMAKE_SHARED_LINKER_FLAGS=-Wl,--no-insert-timestamp"]
    checked(["cmake", "-S", tree, "-B", build, "-G", "Ninja"] + flags, "configure")
    rc = run(["cmake", "--build", build, "--parallel", str(os.cpu_count())], "build")
    run(["sccache", "--show-stats"], "sccache-stats")
    if rc:
        record["missing"] = "Product build failed; see first compiler error in build.log"
        save()
        raise SystemExit(rc)
    if args.mode == "capability":
        libs = list((build / "runtime-staging").rglob("*cangjie-runtime.dylib"))
        assert len(libs) == 1, libs
        testbuild = out / "code-shape-build"
        checked(["cmake", "-S", tree / "tests/gc_unit/metadata", "-B", testbuild, "-G", "Ninja",
                 "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_ASM_COMPILER=clang",
                 "-DGCV2_RUNTIME_LIB_DIR=" + str(libs[0].parent), "-DPRODUCT_BUILD=" + str(build)],
                "code-shape-configure")
        checked(["cmake", "--build", testbuild, "--target", "metadata-code-shape",
                 "--parallel", str(os.cpu_count())], "code-shape-build")
        record["code_shape_elf"] = digest(testbuild / "metadata-code-shape")
        record["code_shape_runtime"] = digest(libs[0])
        checked([testbuild / "metadata-code-shape"], "code-shape-run")
        record["code_shape"] = "PASS: real Mach-O text/data and function map"
        record["capability_products"] = {str(p.relative_to(build)): digest(p)
            for p in (build / "runtime-staging").rglob("*.dylib")}
        record["behavior"] = "NOT_RUN: PAC metadata ABI/fixtures not supplied; build capability only"
        save()
        raise SystemExit(0)
    extension = ".dll" if windows else ".so"
    libs = list((build / "runtime-staging").rglob("*cangjie-runtime" + extension))
    assert len(libs) == 1, libs
    libdir = out / "linked-product"
    libdir.mkdir()
    for path in (build / "runtime-staging").rglob("*"):
        if path.is_file() and (path.suffix in (".dll", ".so") or path.name.endswith(".dll.a")):
            shutil.copy2(path, libdir / path.name)
    # Capture hashes at link completion, before any arm can replace the library.
    record["linked_runtime"] = {"path": str(libs[0]), "sha256": digest(libs[0])}
    checked(["llvm-nm" if windows else "nm", "--defined-only", libs[0]], "symbols")
    if args.config == "default":
        save()
        raise SystemExit(0)
    if args.mode == "prepare":
        testbuild = out / "test-build"
        checked(["cmake", "-S", tree / "tests/gc_unit/metadata", "-B", testbuild, "-G", "Ninja",
                 "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_ASM_COMPILER=clang",
                 "-DCMAKE_CXX_COMPILER_LAUNCHER=sccache", "-DCMAKE_ASM_COMPILER_LAUNCHER=sccache",
                 "-DGCV2_RUNTIME_LIB_DIR=" + str(libdir), "-DPRODUCT_BUILD=" + str(build)], "test-configure")
        checked(["cmake", "--build", testbuild, "--parallel", str(os.cpu_count())], "test-build")
        if not windows:
            record["code_shape_elf"] = digest(testbuild / "metadata-code-shape")
            checked([testbuild / "metadata-code-shape"], "code-shape-run")
            record["code_shape"] = "PASS: real ELF text/data"
        bundle.mkdir()
        exe = testbuild / ("metadata.exe" if windows else "metadata")
        shutil.copy2(exe, bundle / exe.name)
        for lib in libdir.iterdir():
            if lib.is_file() and (lib.suffix in (".dll", ".so", ".a")):
                shutil.copy2(lib, bundle / lib.name)
        record["bundle"] = {p.name: digest(p) for p in bundle.iterdir()}
        (bundle / "identity.json").write_text(json.dumps(record, indent=2))
        if windows:
            checked(["llvm-readobj", "--coff-imports", exe], "pe-imports")
            imports = (out / "pe-imports.log").read_text()
            for symbol in ("GetCurFrameInfo", "GetCallerFrameInfo", "LinkImage"):
                assert symbol in imports, "missing product import: " + symbol
            checked(["llvm-readobj", "--unwind", exe], "pe-unwind")
            unwind = (out / "pe-unwind.log").read_text()
            for symbol in ("MetadataNoDescriptor", "MetadataNoMap", "MetadataPresent"):
                assert symbol in unwind, "missing PE unwind input: " + symbol
            checked(["llvm-objdump", "-s", "-d", exe], "pe-input")
        else:
            checked(["nm", "--undefined-only", exe], "test-imports")
            assert "CallerSP" in (out / "test-imports.log").read_text(), "missing CallerSP product import"
        save()
        raise SystemExit(0)
    reference = json.loads((bundle / "identity.json").read_text())
    assert reference["uncut_product_source_sha256"] == record["uncut_product_source_sha256"]
    same = digest(libs[0]) == reference["bundle"][libs[0].name]
    if args.arm != "baseline":
        assert same == (args.arm == "restored"), "runtime identity mismatch"
    shutil.copy2(libs[0], bundle / libs[0].name)

reference = json.loads((bundle / "identity.json").read_text())
assert reference["head"] == os.environ.get("GITHUB_SHA"), "test bundle belongs to another commit"
for name, sha in reference["bundle"].items():
    if name != ("libcangjie-runtime.dll" if windows else "libcangjie-runtime.so"):
        assert digest(bundle / name) == sha, name
record["tested_bundle"] = {p.name: digest(p) for p in bundle.iterdir() if p.name != "identity.json"}
exe = bundle / ("metadata.exe" if windows else "metadata")
exe.chmod(exe.stat().st_mode | 0o111)
os.environ["LD_LIBRARY_PATH"] = str(bundle)
os.environ["PATH"] = str(bundle) + os.pathsep + os.environ["PATH"]
# The device knife cuts the real spawn line, not a product CHECK or an assertion.
if args.arm == "device":
    before = Path(supervisor.__file__).read_text()
    after = before.replace('[str(exe), case]', '[str(exe), "ManagedMetadata.Unknown" if case.startswith("ManagedMetadata.") else case]')
    assert after != before
    (out / "cut.diff").write_text("".join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
        fromfile="a/runtime/tests/gc_unit/metadata/run.py", tofile="b/runtime/tests/gc_unit/metadata/run.py")))
    scope = {"__name__": "device_cut"}
    exec(compile(after, "device-cut-run.py", "exec"), scope)
    execute = scope["run"]
else:
    execute = supervisor.run
rc = execute(exe, out / "test-result.json")
results = json.loads((out / "test-result.json").read_text())
failed = sorted(name for name, result in results["cases"].items() if not result["pass"])
expected = (["ManagedMetadata." + cuts[args.arm][3]] if args.arm in cuts else
            supervisor.cases(windows) if args.arm == "device" else [])
record.update(behavior="ran", test_rc=rc, failed=failed, expected_failed=sorted(expected))
save()
assert failed == sorted(expected), (failed, expected)
assert rc == int(bool(expected)), rc
