"""One admitted Apple product batch; stop on first error, never retry inputs."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import resource
import signal
import shlex
import shutil
import subprocess
import sys
import tarfile
import threading
import time

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(sys.argv[1]).resolve()
OUT.mkdir(parents=True, exist_ok=False)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
RESULT = {"head": os.environ.get("PAC1481_HEAD"), "commands": [],
          "green": "NOT_RUN", "cut": "NOT_RUN", "restored": "NOT_RUN"}
SAVE_LOCK = threading.Lock()
DEADLINE = time.monotonic() + 55 * 60


def save():
    with SAVE_LOCK:
        (OUT / "result.json").write_text(json.dumps(RESULT, indent=2) + "\n")


def run(command, name, cwd=ROOT, env=None, timeout=1800):
    start = time.monotonic()
    with (OUT / name).open("w") as log:
        log.write(shlex.join(map(str, command)) + "\n")
        log.flush()
        try:
            process = subprocess.Popen(list(map(str, command)), cwd=cwd, env=env,
                                       stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            rc = process.wait(timeout=max(1, min(timeout, DEADLINE - start)))
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            rc = 124
    record = {"command": list(map(str, command)), "cwd": str(cwd), "log": name,
              "rc": rc, "wall": time.monotonic() - start}
    RESULT["commands"].append(record)
    (OUT / (name + ".rc")).write_text(str(rc) + "\n")
    save()
    print(f"{name}: rc={rc} wall={record['wall']:.2f}", flush=True)
    return rc


def require(command, name, **kwargs):
    rc = run(command, name, **kwargs)
    if rc:
        raise RuntimeError(f"first error: {name} rc={rc}")


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def snapshot(name):
    target = OUT / name
    target.mkdir()
    archive = OUT / (name + ".tar")
    require(["git", "archive", "--format=tar", "-o", archive, "HEAD"], name + "-archive.log")
    with tarfile.open(archive) as bundle:
        bundle.extractall(target, filter="data")
    archive.unlink()
    return target


def configure(tree, pac):
    # Native build.py recipe, with isolated source/output trees and explicit PAC axis.
    return ["cmake", "-S", tree / "runtime", "-B", tree / "runtime/CMakebuild",
            "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={tree}/install_aarch64",
            "-DCOPYGC_FLAG=1", "-DDOPRA_FLAG=1", "-DMRT_GC_UNIT_OHOS_HOST=OFF",
            "-DOHOS_FLAG=0", "-DANDROID_FLAG=0", "-DIOS_FLAG=0", "-DIOS_SIMULATOR_FLAG=0",
            "-DEULER_FLAG=0", "-DMACOS_FLAG=1", "-DRUNTIME_TRACE_FLAG=1", "-DASAN_FLAG=0",
            "-DHWASAN_FLAG=0", "-DSANITIZER_SUPPORT=OFF", "-DCOV=0", "-DDUMPADDRESS_FLAG=0",
            "-DDISABLE_VERSION_CHECK=1", "-DCJ_SDK_VERSION=0.0.1",
            "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
            "-DCMAKE_AR_PATH=ar", "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-rpath,@loader_path",
            "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-rpath,@loader_path/../../runtime/lib/darwin_aarch64_cjnative",
            "-DBUILD_CJTHREAD=ON", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF", "-DCANGJIE_COMPILER_CACHE=sccache",
            "-DCMAKE_CXX_COMPILER_LAUNCHER=sccache",
            "-DCMAKE_C_COMPILER_LAUNCHER=sccache", "-DRUNTIME_FORWARD_PTRAUTH_CFI=0",
            f"-DRUNTIME_BACKWARD_PTRAUTH_CFI={int(pac)}",
            f"-DCMAKE_OSX_SYSROOT={RESULT['sdk_path']}", "-DCMAKE_OSX_ARCHITECTURES=arm64"]


def build(tree, name, pac):
    require(configure(tree, pac), name + "-configure.log")
    build_dir = tree / "runtime/CMakebuild"
    require(["cmake", "--build", build_dir, "--target", "cangjie-runtime", "preinstall",
             "--parallel", str(os.cpu_count()), "--verbose"], name + "-build.log")
    for filename in ("CMakeCache.txt", "compile_commands.json"):
        shutil.copy2(build_dir / filename, OUT / (name + "-" + filename))
    shutil.copy2(build_dir / "cjthread-build/CMakeCache.txt", OUT / (name + "-cjthread-CMakeCache.txt"))
    shutil.copy2(build_dir / "src/CMakeFiles/cangjie-runtime.dir/link.txt", OUT / (name + "-product-link.txt"))
    require(["sccache", "--show-stats"], name + "-sccache.log")
    runtime, = list(build_dir.rglob("libcangjie-runtime.dylib"))
    bounds, = list(build_dir.rglob("libboundscheck.dylib"))
    retained = OUT / (name + "-dylibs")
    retained.mkdir()
    for path in (runtime, bounds):
        shutil.copy2(path, retained / path.name)
        require(["otool", "-L", path], name + "-" + path.name + "-deps.log")
    RESULT[name + "_hashes"] = {p.name: sha(p) for p in retained.iterdir()}
    save()
    return retained


def entry(tree, suffix):
    entries = json.loads((tree / "runtime/CMakebuild/compile_commands.json").read_text())
    found, = [e for e in entries if e["file"].endswith(suffix)
              and "cangjie-runtime.dir" in e["command"]]
    return found, shlex.split(found["command"])


def observations(log):
    text = (OUT / log).read_text()
    rows = re.findall(r"ASSERT_EXECUTED target=(\d) raw=([0-9a-f]+) signed=([0-9a-f]+) "
                      r"modifier=([0-9a-f]+) raw_result=(\d) signed_result=(\d) expected=(\d) pass=(\d)", text)
    if len(rows) != 2 or [r[0] for r in rows] != ["0", "1"]:
        raise RuntimeError("missing target assertions")
    return rows


def instruction_checks():
    producer = (OUT / "producer-disassembly.log").read_text().lower()
    if not re.search(r"d503211f|1f 21 03 d5", producer):
        raise RuntimeError("producer lacks actual PACIA1716 encoding")
    # Check actual product code, including compiler-generated caller LR preservation.
    assembly = (OUT / "machine-frame-disassembly.log").read_text().lower()
    blocks = re.split(r"\n[0-9a-f]+ <", assembly)
    target, = [block for block in blocks if "isn2cstubframe" in block.splitlines()[0]]
    if not re.search(r"xpaclri|hint\s+#(?:0x)?7\b|d50320ff|ff 20 03 d5", target):
        # Non-inlined strip is permissible only if the called real strip body has XPACLRI.
        mem = (OUT / "memutils-disassembly.log").read_text().lower()
        if "ptrauthstripinstpointer" not in target or not re.search(r"xpaclri|hint\s+#(?:0x)?7\b|d50320ff|ff 20 03 d5", mem):
            raise RuntimeError("product N2C strip instruction not established")
    if not re.search(r"\b(?:stp|str)\b[^\n]*\b(?:x30|lr)\b", target) or not re.search(r"\b(?:ldp|ldr)\b[^\n]*\b(?:x30|lr)\b", target):
        raise RuntimeError("caller LR preservation not established in real N2C product code")
    (OUT / "instruction-check.json").write_text(json.dumps({"producer_encoding": "d503211f",
        "product_strip": "XPACLRI", "caller_lr_saved_restored": True,
        "scope": "actual N2C product object only"}, indent=2))


def arm(name, libraries, elf):
    before = {p.name: sha(p) for p in libraries.iterdir()}
    env = os.environ.copy()
    env.update(DYLD_LIBRARY_PATH=str(libraries), DYLD_PRINT_LIBRARIES="1")
    rc = run([elf], name + ".log", env=env, timeout=120)
    RESULT[name] = rc
    RESULT[name + "_identity"] = {"elf": sha(elf), "producer": sha(OUT / "producer.o"),
                                  "dylibs": before, "dependencies": {}}
    text = (OUT / (name + ".log")).read_text()
    mappings = [Path(line.removeprefix("MAPPING ")) for line in text.splitlines()
                if line.startswith("MAPPING ")]
    for path in mappings:
        RESULT[name + "_identity"]["dependencies"][str(path)] = sha(path) if path.is_file() else "DYLD_SHARED_CACHE"
    save()
    if {p.name: sha(p) for p in libraries.iterdir()} != before:
        raise RuntimeError("dylib identity changed during execution")
    for leaf in ("libcangjie-runtime.dylib", "libboundscheck.dylib"):
        matches = [p for p in mappings if p.name == leaf]
        if matches != [libraries / leaf]:
            raise RuntimeError(f"loader identity failure: {leaf}: {matches}")
    return rc, observations(name + ".log")


def main():
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise RuntimeError("ordinary macos arm64 required")
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    if head != RESULT["head"] or os.environ.get("PAC1481_LABEL") != "p1481-" + head:
        raise RuntimeError("admitted event/head mismatch")
    if subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True).strip():
        raise RuntimeError("dirty checkout")
    require(["xcodebuild", "-version"], "xcode.log")
    if "Xcode 16.4\n" not in (OUT / "xcode.log").read_text():
        raise RuntimeError("Xcode16.4 required")
    for command, name in ((["uname", "-a"], "uname.log"), (["sw_vers"], "os.log"),
                          (["clang++", "--version"], "compiler.log"), (["uptime"], "uptime-before.log"),
                          (["xcrun", "--sdk", "macosx", "--show-sdk-version"], "sdk-version.log")):
        require(command, name)
    RESULT["sdk_path"] = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
    RESULT["jobs"] = os.cpu_count()
    RESULT["parallel_product_arms"] = 2
    RESULT["event"] = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text())
    RESULT["run_id"] = os.environ["GITHUB_RUN_ID"]
    RESULT["run_attempt"] = os.environ["GITHUB_RUN_ATTEMPT"]
    RESULT["source_hashes"] = {str(p.relative_to(ROOT)): sha(p) for p in
        (ROOT / "runtime/src/Base/MemUtils.cpp", ROOT / "runtime/src/Exception/EhFrameInfo.h",
         ROOT / ".github/workflows/pac-product-1481.yml", Path(__file__),
         ROOT / "runtime/tests/pac_strip_darwin/consumer.cpp",
         ROOT / "runtime/tests/pac_strip_darwin/producer.S",
         ROOT / "runtime/tests/pac_strip_darwin/consumer-strip.diff")}
    require(["sccache", "--show-stats"], "sccache-before.log")
    default, pac = snapshot("default-source"), snapshot("pac-source")
    # Trees do not share generated headers, CJThread outputs, or install prefixes.
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(build, tree, name, enabled) for tree, name, enabled in
                   ((default, "default", False), (pac, "pac", True))]
        outcomes = []
        for future in futures:
            try:
                outcomes.append(future.result())
            except Exception as error:
                outcomes.append(error)
        if any(isinstance(value, Exception) for value in outcomes):
            raise RuntimeError(str(outcomes))
    libraries = outcomes[1]
    nm = shutil.which("llvm-nm") or str(Path("/opt/homebrew/opt/llvm/bin/llvm-nm"))
    objdump = str(Path(nm).with_name("llvm-objdump"))
    readobj = str(Path(nm).with_name("llvm-readobj"))
    require([nm, "--defined-only", libraries / "libcangjie-runtime.dylib"], "product-defined.log")
    require(["nm", "-gU", libraries / "libcangjie-runtime.dylib"], "product-exports.log")
    exported = (OUT / "product-exports.log").read_text()
    for symbol in ("_unwindPCForN2CStub", "_unwindPCForExclusiveStubFull", "IsN2CStubFrame"):
        if symbol not in exported:
            raise RuntimeError("missing real product export: " + symbol)
    require([objdump, "--disassemble", libraries / "libcangjie-runtime.dylib"], "product-disassembly.log")
    compile_entry, args = entry(pac, "/MachineFrame.cpp")
    mem_entry, mem_args = entry(pac, "/MemUtils.cpp")
    for label, value, command in (("machine-frame", compile_entry, args), ("memutils", mem_entry, mem_args)):
        object_path = Path(value["directory"]) / command[command.index("-o") + 1]
        shutil.copy2(object_path, OUT / (label + ".o"))
        require([objdump, "--disassemble", "--reloc", object_path], label + "-disassembly.log")
    (OUT / "fixture-product-compile-entry.json").write_text(json.dumps(compile_entry, indent=2))
    # Inherit every product definition/include/ABI flag; only substitute the input/output TU.
    flags = args[1:]
    for option in ("-o", "-c"):
        at = flags.index(option)
        del flags[at:at + 2]
    if any("flto" in flag for flag in flags):
        raise RuntimeError("unexpected product LTO flags")
    flags += ["-fno-lto", "-O0"]
    compiler = args[0]
    fixture = ROOT / "runtime/tests/pac_strip_darwin"
    require([compiler, "-arch", "arm64", "-isysroot", RESULT["sdk_path"], "-fno-lto", "-c",
             fixture / "producer.S", "-o", OUT / "producer.o"], "producer-build.log")
    require([compiler, *flags, "-c", fixture / "consumer.cpp", "-o", OUT / "consumer.o"],
            "consumer-build.log", cwd=compile_entry["directory"])
    require([objdump, "--disassemble", OUT / "producer.o"], "producer-disassembly.log")
    require([readobj, "--relocations", OUT / "consumer.o"], "consumer-relocations.log")
    instruction_checks()
    elf = OUT / "consumer"
    require([compiler, "-arch", "arm64", "-isysroot", RESULT["sdk_path"], "-fno-lto",
             OUT / "consumer.o", OUT / "producer.o", "-L" + str(libraries),
             "-Wl,-rpath," + str(libraries), "-lcangjie-runtime", "-lboundscheck", "-o", elf], "fixture-link.log")
    RESULT["fixture_hashes"] = {p.name: sha(p) for p in (elf, OUT / "producer.o", OUT / "consumer.o")}
    save()
    require(["otool", "-L", elf], "fixture-dependencies.log")
    rc, green = arm("green", libraries, elf)
    if rc == 21:
        RESULT["stop"] = "owned signed==raw: causal arms NOT_RUN"
        return 21
    if rc != 0 or [row[7] for row in green] != ["1", "1"]:
        raise RuntimeError("green target/control failure")
    # Exactly one frozen baseline consumer cut; integer cast prerequisites remain.
    cut_tree = snapshot("cut-source")
    require(["git", "apply", "--unsafe-paths", fixture / "consumer-strip.diff"], "cut-apply.log", cwd=cut_tree)
    cut_libraries = build(cut_tree, "cut-product", True)
    # The sole variable is runtime consumer code; use the exact green dependency.
    shutil.copy2(libraries / "libboundscheck.dylib", cut_libraries / "libboundscheck.dylib")
    rc, cut = arm("cut", cut_libraries, elf)
    # ASLR changes numeric PCs between launches; compare invariant result columns.
    if rc != 1 or cut[0][3:] != ("0", "1", "0", "1", "0") or cut[1][3:] != ("0", "0", "0", "0", "1"):
        raise RuntimeError("cut was not precisely owned signed target red")
    if cut[0][1] == cut[0][2]:
        raise RuntimeError("cut owned value indistinguishable")
    restored = OUT / "restored-dylibs"
    shutil.copytree(libraries, restored)
    rc, rows = arm("restored", restored, elf)
    if rc != 0 or [row[7] for row in rows] != ["1", "1"]:
        raise RuntimeError("restore failed")
    original = RESULT["green_identity"]["dylibs"]
    if RESULT["restored_identity"]["dylibs"] != original or RESULT["cut_identity"]["dylibs"]["libcangjie-runtime.dylib"] == original["libcangjie-runtime.dylib"]:
        raise RuntimeError("invalid three-arm product identity")
    if RESULT["cut_identity"]["dylibs"]["libboundscheck.dylib"] != original["libboundscheck.dylib"]:
        raise RuntimeError("non-cut bounds dependency changed")
    RESULT["stop"] = "bounded N2C strip observations complete; no whole PAC ABI claim"


try:
    sys.exit(main() or 0)
except Exception as error:
    RESULT["first_error"] = str(error)
    print(str(error), file=sys.stderr)
    sys.exit(20)
finally:
    run(["uptime"], "uptime-after.log", timeout=10)
    for name in ("default-source", "pac-source", "cut-source"):
        tree = OUT / name
        if tree.exists():
            build_dir = tree / "runtime/CMakebuild"
            for filename in ("CMakeCache.txt", "compile_commands.json", "cjthread-build/CMakeCache.txt"):
                path = build_dir / filename
                if path.is_file():
                    shutil.copy2(path, OUT / (name + "-" + filename.replace("/", "-")))
            shutil.rmtree(tree)
    save()
