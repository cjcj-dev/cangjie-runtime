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
parser.add_argument("mode", choices=["prepare", "arm", "capability", "a2", "a2-resume"])
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


def run(command, label, cwd=None, timeout=None):
    start = time.monotonic()
    argv = [str(x) for x in command]
    record[label] = {"status": "STARTED", "argv": argv, "cwd": str(cwd) if cwd else os.getcwd(),
                     "command_sha256": hashlib.sha256(json.dumps(argv).encode()).hexdigest()}
    save() # retain command identity even if the outer workflow timeout terminates us
    with (out / (label + ".log")).open("w") as log:
        log.write(repr(argv) + "\n")
        log.flush()
        try:
            rc = subprocess.run(argv, env=env, cwd=cwd, stdout=log,
                                stderr=subprocess.STDOUT, timeout=timeout).returncode
        except subprocess.TimeoutExpired as error:
            rc = 124
            record[label]["failure"] = str(error)
            log.write("\nTIMEOUT: " + str(error) + "\n")
        except OSError as error:
            rc = 127
            record[label]["failure"] = str(error)
            log.write("\nLAUNCH_ERROR: " + str(error) + "\n")
    record[label].update(rc=rc, wall=time.monotonic() - start, status="FINISHED")
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

def a2_run_bundle(runtime_hash=None):
    suffix = ".exe" if windows else ""
    executable = bundle / ("metadata-code-shape" + suffix)
    environment = dict(env)
    environment["PATH"] = str(bundle) + os.pathsep + environment.get("PATH", "")
    environment["LD_LIBRARY_PATH"] = str(bundle)
    environment["DYLD_LIBRARY_PATH"] = str(bundle)
    before = dict(env)
    env.update(environment)
    record["test_elf_sha256"] = digest(executable)
    extension = ".dll" if windows else (".dylib" if mac else ".so")
    runtime = bundle / ("libcangjie-runtime" + extension)
    record["runtime_sha256"] = digest(runtime)
    identity = json.loads((bundle / "a2-candidate.json").read_text())
    assert record["test_elf_sha256"] == identity["test_elf_sha256"]
    assert all(digest(bundle / name) == value for name, value in identity["fixtures"].items())
    if args.arm in ("candidate", "restored"):
        assert record["runtime_sha256"] == identity["runtime_sha256"]
    else:
        assert record["runtime_sha256"] != identity["runtime_sha256"]
    rc = run([executable], "a2-run", timeout=120)
    env.clear()
    env.update(before)
    record["execution_rc"] = rc
    output = (out / "a2-run.log").read_text()
    expected_cross = int(args.arm == "descriptor-owner")
    expected_data = int(args.arm == "code-only")
    targets = ["METADATA_SAME_OWNER_TARGET same=1 code=1 data_member=1 executed=1",
               f"METADATA_CROSS_OWNER_TARGET cross={expected_cross} foreign_registered=1 distinct=1 executed=1",
               "METADATA_ABSENT_TARGET absent=0 executed=1",
               f"METADATA_CODE_ONLY_TARGET data={expected_data} executed=1"]
    if not mac:
        prefix_names = ["ordinary", "contiguous"] if windows else ["ordinary", "contiguous", "outside", "hole"]
        for name in prefix_names:
            assert f"METADATA_PREFIX_INPUT name={name} qualified=1" in output, output
            assert f"METADATA_PREFIX_DESCRIPTOR name={name} qualified=1" in output, output
            expected = int(name not in ("hole", "outside"))
            targets.append(f"METADATA_PREFIX_TARGET name={name} accepted={expected} expected={expected} executed=1")
        record["prefix_targets"] = prefix_names
        if windows:
            applicability = "APPLICABILITY_NOT_PRODUCIBLE_BY_CURRENT_WINDOWS_REGISTRATION"
            assert "METADATA_PREFIX_APPLICABILITY name=hole status=" + applicability in output, output
            record["prefix_internal_hole"] = applicability
    assert all(target in output for target in targets), output
    assert rc == (1 if expected_cross or expected_data else 0), (rc, output)
    record["behavior"] = "PRECISE_RED" if expected_cross or expected_data else "PASS"
    save()

if args.mode == "a2-resume":
    metadata = bundle / ("metadata.exe" if windows else "metadata")
    metadata.chmod(metadata.stat().st_mode | 0o111)
    before = dict(env)
    env["PATH"] = str(bundle) + os.pathsep + env.get("PATH", "")
    env["LD_LIBRARY_PATH"] = str(bundle)
    record["regression_elf_sha256"] = digest(metadata)
    rc = run(["python3", source / "tests/gc_unit/metadata/run.py", metadata,
              out / "metadata.json"], "metadata-regression", timeout=120)
    env.clear(); env.update(before)
    assert rc == 0, rc
    executable = bundle / ("metadata-code-shape.exe" if windows else "metadata-code-shape")
    executable.chmod(executable.stat().st_mode | 0o111)
    a2_run_bundle()
    raise SystemExit(0)

if args.mode == "a2" and args.arm == "restored":
    a2_run_bundle()
    raise SystemExit(0)

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
    if args.mode == "a2" and args.arm in ("code-only", "descriptor-owner"):
        patches = []
        for relative in ("src/Loader/ElfUnloadQuiescence.cpp", "src/ObjectModel/MFuncdesc.inline.h"):
            path = tree / relative
            before = path.read_text()
            after = before
            if args.arm == "code-only" and relative.endswith("ElfUnloadQuiescence.cpp"):
                assert after.count("maps.Find(address, codeOnly)") == 1
                after = after.replace("maps.Find(address, codeOnly)", "maps.Find(address, false)")
                after = after.replace("FindImageInterval(unloadWriterIntervals, address, codeOnly)",
                                      "FindImageInterval(unloadWriterIntervals, address, false)")
            if args.arm == "descriptor-owner":
                after = after.replace("return image->Contains(reinterpret_cast<Uptr>(desc)) ? desc : nullptr;", "return desc;")
                after = after.replace("return image->Contains(descriptor) ? descriptor : 0;", "return descriptor;")
            if after != before:
                path.write_text(after)
                patches.append("".join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                    fromfile="a/runtime/" + relative, tofile="b/runtime/" + relative)))
        assert patches
        (out / "cut.diff").write_text("".join(patches))
    if args.arm in ("mach-pc", "mach-desc"):
        assert args.mode == "capability" and mac and args.config == "pac-off"
        path = tree / "src/Loader/ElfUnloadQuiescence.cpp"
        before = path.read_text()
        old, new = (("RegisteredImageForAddress(startPC, true)", "RegisteredImageForAddress(startPC)")
                    if args.arm == "mach-pc" else
                    ("return image->Contains(descriptor) ? descriptor : 0;", "return descriptor;"))
        assert before.count(old) == 1
        after = before.replace(old, new)
        path.write_text(after)
        (out / "cut.diff").write_text("".join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
            fromfile="a/runtime/src/Loader/ElfUnloadQuiescence.cpp",
            tofile="b/runtime/src/Loader/ElfUnloadQuiescence.cpp")))
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
        shape_rc = run([testbuild / "metadata-code-shape"], "code-shape-run")
        if args.arm in ("mach-pc", "mach-desc"):
            assert shape_rc == 1, shape_rc
            output = (out / "code-shape-run.log").read_text()
            target = "PC" if args.arm == "mach-pc" else "DESC"
            other = "DESC" if target == "PC" else "PC"
            assert "METADATA_MAP_" + target + "_OWNER pass=0 executed=1" in output
            assert "METADATA_MAP_" + other + "_OWNER pass=1 executed=1" in output
            assert "METADATA_MAP_SAME_OWNER pass=1 executed=1" in output
            assert "METADATA_ABSENT_TARGET absent=0 executed=1" in output
            assert "METADATA_SAME_OWNER_TARGET same=1 code=1 data_member=1 executed=1" in output
            record["code_shape"] = "CONTROL_ARM: precise target red " + target
        else:
            assert shape_rc == 0, shape_rc
            record["code_shape"] = "PASS: real Mach-O text/data and function map"
        record["capability_products"] = {str(p.relative_to(build)): digest(p)
            for p in (build / "runtime-staging").rglob("*.dylib")}
        record["behavior"] = "NOT_RUN: PAC metadata ABI/fixtures not supplied; build capability only"
        save()
        raise SystemExit(0)
    extension = ".dll" if windows else (".dylib" if mac else ".so")
    libs = list((build / "runtime-staging").rglob("*cangjie-runtime" + extension))
    assert len(libs) == 1, libs
    libdir = out / "linked-product"
    libdir.mkdir()
    for path in (build / "runtime-staging").rglob("*"):
        if path.is_file() and (path.suffix in (".dll", ".so", ".dylib") or path.name.endswith(".dll.a")):
            shutil.copy2(path, libdir / path.name)
    # Capture hashes at link completion, before any arm can replace the library.
    record["linked_runtime"] = {"path": str(libs[0]), "sha256": digest(libs[0])}
    checked(["llvm-nm" if windows else "nm", "--defined-only", libs[0]], "symbols")
    if args.mode == "a2":
        if args.arm == "candidate":
            record["fixture_generator_sha256"] = {name: digest(tree / "tests/gc_unit" / name)
                for name in ("metadata_owner_records.cpp", "a2_contiguous.ld", "a2_prefix_hole.py")}
            save()
            testbuild = out / "a2-test-build"
            checked(["cmake", "-S", tree / "tests/gc_unit/metadata", "-B", testbuild, "-G", "Ninja",
                     "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_ASM_COMPILER=clang",
                     "-DCMAKE_CXX_COMPILER_LAUNCHER=sccache", "-DCMAKE_ASM_COMPILER_LAUNCHER=sccache",
                     "-DMANAGED_METADATA_LINKER=" + env.get("MANAGED_METADATA_LINKER", ""),
                 "-DGCV2_RUNTIME_LIB_DIR=" + str(libdir), "-DPRODUCT_BUILD=" + str(build)], "a2-test-configure")
            targets = ["metadata-code-shape"] if mac or args.config == "default" else ["metadata-code-shape", "metadata"]
            checked(["cmake", "--build", testbuild, "--target", *targets, "--parallel", str(os.cpu_count())], "a2-test-build")
            bundle.mkdir()
            exe = testbuild / ("metadata-code-shape.exe" if windows else "metadata-code-shape")
            shutil.copy2(exe, bundle / exe.name)
            fixtures = {}
            for path in testbuild.iterdir():
                if path.is_file() and path.name.startswith("cj_metadata_") and path.suffix == extension:
                    shutil.copy2(path, bundle / path.name)
                    fixtures[path.name] = digest(path)
            assert len(fixtures) == (2 if mac else 3 if windows else 4)
            record["fixture_sha256"] = fixtures
            record["test_elf_sha256"] = digest(bundle / exe.name)
            save() # identities survive the first inspection failure
            if not mac:
                # Preserve exact producer commands, headers and bytes; leave
                # Apple capability and existing full-mode selection unchanged.
                checked(["cmake", "--build", testbuild, "--target", "help"], "a2-targets")
                checked(["ninja", "-C", testbuild, "-t", "commands", *targets], "a2-test-commands")
                imports = ["llvm-readobj", "--coff-imports"] if windows else ["nm", "--undefined-only"]
                checked([*imports, bundle / exe.name], "a2-test-imports")
                for name in fixtures:
                    inspector = ["llvm-readobj", "--file-headers", "--sections"]
                    if not windows:
                        inspector.append("--program-headers")
                    checked([*inspector, bundle / name], "a2-input-" + name)
            for path in libdir.iterdir():
                if path.is_file() and path.suffix in (".dll", ".so", ".dylib"):
                    shutil.copy2(path, bundle / path.name)
            identity = dict(test_elf_sha256=digest(bundle / exe.name),
                            runtime_sha256=digest(bundle / ("libcangjie-runtime" + extension)), fixtures=fixtures)
            (bundle / "a2-candidate.json").write_text(json.dumps(identity, indent=2))
            if not mac and args.config == "testable":
                metadata = testbuild / ("metadata.exe" if windows else "metadata")
                shutil.copy2(metadata, bundle / metadata.name)
                managed_image = testbuild / ("cj_managed_metadata.dll" if windows else "libcj_managed_metadata.so")
                shutil.copy2(managed_image, bundle / managed_image.name)
                before = dict(env)
                env["PATH"] = str(bundle) + os.pathsep + env.get("PATH", "")
                env["LD_LIBRARY_PATH"] = str(bundle)
                env["DYLD_LIBRARY_PATH"] = str(bundle)
                rc = run(["python3", tree / "tests/gc_unit/metadata/run.py", bundle / metadata.name,
                          out / "metadata.json"], "metadata-regression", timeout=120)
                env.clear(); env.update(before)
                assert rc == 0, rc
        else:
            # Test ELF and fixture DSOs are the candidate's immutable artifacts.
            for path in libdir.iterdir():
                if path.is_file() and path.suffix in (".dll", ".so", ".dylib"):
                    shutil.copy2(path, bundle / path.name)
        a2_run_bundle()
        raise SystemExit(0)
    if args.config == "default":
        save()
        raise SystemExit(0)
    if args.mode == "prepare":
        testbuild = out / "test-build"
        checked(["cmake", "-S", tree / "tests/gc_unit/metadata", "-B", testbuild, "-G", "Ninja",
                 "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_ASM_COMPILER=clang",
                 "-DCMAKE_CXX_COMPILER_LAUNCHER=sccache", "-DCMAKE_ASM_COMPILER_LAUNCHER=sccache",
                 "-DMANAGED_METADATA_LINKER=" + env.get("MANAGED_METADATA_LINKER", ""),
                 "-DGCV2_RUNTIME_LIB_DIR=" + str(libdir), "-DPRODUCT_BUILD=" + str(build)], "test-configure")
        checked(["cmake", "--build", testbuild, "--parallel", str(os.cpu_count())], "test-build")
        if not windows:
            record["code_shape_elf"] = digest(testbuild / "metadata-code-shape")
            checked([testbuild / "metadata-code-shape"], "code-shape-run")
            record["code_shape"] = "PASS: real ELF text/data"
        bundle.mkdir()
        exe = testbuild / ("metadata.exe" if windows else "metadata")
        shutil.copy2(exe, bundle / exe.name)
        managed_image = testbuild / ("cj_managed_metadata.dll" if windows else
                                      "libcj_managed_metadata.dylib" if mac else "libcj_managed_metadata.so")
        shutil.copy2(managed_image, bundle / managed_image.name)
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
            checked(["llvm-readobj", "--unwind", managed_image], "pe-unwind")
            unwind = (out / "pe-unwind.log").read_text()
            for symbol in ("ManagedMetadataNative", "ManagedMetadataNoMap", "ManagedMetadataEmpty"):
                assert symbol in unwind, "missing PE unwind input: " + symbol
            checked(["llvm-objdump", "-s", "-d", exe], "pe-input")
        elif mac or platform.machine().lower() in ("aarch64", "arm64"):
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
