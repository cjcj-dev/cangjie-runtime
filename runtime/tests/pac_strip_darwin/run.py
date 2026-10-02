"""One admitted Apple product batch; stop on first error, never retry inputs."""
import concurrent.futures
from datetime import datetime
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
from recipe import bind_tools, configure_env, verify_configured, preserve_then_delete, preserve_arms

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(sys.argv[1]).resolve()
OUT.mkdir(parents=True, exist_ok=False)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
RESULT = {"head": os.environ.get("PAC1481_HEAD"), "commands": [],
          "green": "NOT_RUN", "cut": "NOT_RUN", "restored": "NOT_RUN"}
SAVE_LOCK = threading.Lock()
PLAN = json.loads(Path(__file__).with_name("PLAN.json").read_text())
DEADLINE = datetime.fromisoformat(PLAN["deadline_utc"]).timestamp() if PLAN["deadline_utc"] else 0
RESULT["deadline_utc"] = PLAN["deadline_utc"]


def save():
    with SAVE_LOCK:
        (OUT / "result.json").write_text(json.dumps(RESULT, indent=2) + "\n")


def run(command, name, cwd=ROOT, env=None, timeout=1800):
    start = time.monotonic()
    remaining = DEADLINE - time.time() - PLAN["archive_reserve_seconds"]
    if remaining <= 0:
        RESULT["commands"].append({"command": list(map(str, command)), "log": name,
                                   "rc": "NOT_RUN", "reason": "absolute deadline/archive reserve"})
        save()
        raise RuntimeError("absolute deadline: subsequent commands NOT_RUN")
    with (OUT / name).open("w") as log:
        log.write(shlex.join(map(str, command)) + "\n")
        log.flush()
        try:
            process = subprocess.Popen(list(map(str, command)), cwd=cwd, env=env,
                                       stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            rc = process.wait(timeout=min(timeout, remaining))
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
            "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF", f"-DCANGJIE_COMPILER_CACHE:FILEPATH={RESULT['tools']['sccache']['path']}",
            f"-DCMAKE_CXX_COMPILER_LAUNCHER={RESULT['tools']['sccache']['path']}",
            f"-DCMAKE_C_COMPILER_LAUNCHER={RESULT['tools']['sccache']['path']}", "-DRUNTIME_FORWARD_PTRAUTH_CFI=0",
            f"-DRUNTIME_BACKWARD_PTRAUTH_CFI={int(pac)}",
            f"-DCMAKE_OSX_SYSROOT={RESULT['sdk_path']}", "-DCMAKE_OSX_ARCHITECTURES=arm64"]


def build(tree, name, pac):
    build_dir = tree / "runtime/CMakebuild"
    if build_dir.exists():
        raise RuntimeError("fresh configure directory required")
    env = configure_env(os.environ, RESULT["tools"])
    RESULT[name + "_configure_input"] = {"argv": list(map(str, configure(tree, pac))),
        "env": {k: env[k] for k in ("CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER")}}
    save()
    require([RESULT["tools"]["sccache"]["path"], "--show-stats"], name + "-before-child-cache.log")
    require(configure(tree, pac), name + "-configure.log", env=env)
    require([RESULT["tools"]["sccache"]["path"], "--show-stats"], name + "-after-child-cache.log")
    RESULT[name + "_cache_consumption"] = verify_configured(build_dir, RESULT["tools"])
    save()
    build_dir = tree / "runtime/CMakebuild"
    require(["cmake", "--build", build_dir, "--target", "cangjie-runtime", "preinstall",
             "--parallel", str(os.cpu_count()), "--verbose"], name + "-build.log")
    for filename in ("CMakeCache.txt", "compile_commands.json"):
        shutil.copy2(build_dir / filename, OUT / (name + "-" + filename))
    shutil.copy2(build_dir / "cjthread-build/CMakeCache.txt", OUT / (name + "-cjthread-CMakeCache.txt"))
    shutil.copy2(build_dir / "src/CMakeFiles/cangjie-runtime.dir/link.txt", OUT / (name + "-product-link.txt"))
    require([RESULT["tools"]["sccache"]["path"], "--show-stats"], name + "-sccache.log")
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


def entry(tree, relative_source, owner):
    entries = json.loads((tree / "runtime/CMakebuild/compile_commands.json").read_text())
    source = (tree / relative_source).resolve()
    found, = [e for e in entries if Path(e["file"]).resolve() == source
              and f"CMakeFiles/{owner}.dir/" in e["command"]]
    return found, shlex.split(found["command"])


def observations(log):
    text = (OUT / log).read_text()
    rows = re.findall(r"ASSERT_EXECUTED target=(\d) raw=([0-9a-f]+) signed=([0-9a-f]+) "
                      r"modifier=([0-9a-f]+) raw_result=(\d) signed_result=(\d) expected=(\d) pass=(\d)", text)
    if len(rows) != 2 or [r[0] for r in rows] != ["0", "1"]:
        raise RuntimeError("missing target assertions")
    return rows


def function_block(text, symbol):
    blocks = re.split(r"\n[0-9a-f]+ <", text.lower())
    block, = [block for block in blocks if symbol in block.splitlines()[0]]
    return block


def lr_flow(block, require_stripped_return):
    # Bounded symbolic check of the actual function block, not an opcode elsewhere.
    registers = {"x30": "incoming_lr", "x0": "input_pc"}
    bases = {"sp": 0}
    slots = {}
    def write(register, value=None, base=None):
        # W writes zero-extend into the same X register, invalidating 64-bit tags.
        alias = "x" + register[1:] if re.fullmatch(r"w(?:[0-9]|[12][0-9]|30)", register) else register
        registers[alias] = value if alias == register else None
        bases.pop(alias, None)
        if base is not None and alias == register:
            bases[alias] = base

    xpac = False
    returns = 0
    instruction = re.compile(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{8}|[0-9a-f]{2}\s+[0-9a-f]{2}\s+[0-9a-f]{2}\s+[0-9a-f]{2})\s+(\w+)\s*(.*?)\s*$")
    for line in block.splitlines():
        match = instruction.match(line)
        if not match:
            if re.match(r"^\s*[0-9a-f]+:", line):
                raise RuntimeError("INVALID: unparsed instruction line")
            continue
        op, operands = match.groups()
        operands = operands.replace("lr", "x30").replace("fp", "x29")
        parts = [part.strip() for part in operands.split(",")]
        if op == "mov" and len(parts) == 2:
            write(parts[0], registers.get(parts[1]), bases.get(parts[1]))
        elif op in ("add", "sub"):
            if len(parts) != 3 or not re.fullmatch(r"#(?:0x[0-9a-f]+|[0-9]+)", parts[2]):
                raise RuntimeError("INVALID: unsupported add/sub operand")
            offset = int(parts[2][1:], 0) * (-1 if op == "sub" else 1)
            base = bases.get(parts[1])
            write(parts[0], base=base + offset if base is not None else None)
        elif op in ("stp", "str", "stur", "ldp", "ldr", "ldur"):
            memory = re.search(r"\[(sp|x29)(?:,\s*#(-?(?:0x[0-9a-f]+|\d+)))?\](!)?(?:,\s*#(-?(?:0x[0-9a-f]+|\d+)))?", operands)
            if memory is None:
                raise RuntimeError("INVALID: unsupported memory operand")
            if memory[1] not in bases:
                if op.startswith("st"):
                    raise RuntimeError("INVALID: unknown store address")
                # Ordinary frame/PC loads are unrelated to preserved return state.
                if op.startswith("ld"):
                    for register in parts[:2 if op == "ldp" else 1]:
                        write(register)
                continue
            base, offset, pre, post = memory.groups()
            if (pre or post) and base in parts[:2 if op == "ldp" else 1]:
                raise RuntimeError("INVALID: writeback overlaps data register")
            address = bases[base] + int(offset or "0", 0)
            count = 2 if op in ("stp", "ldp") else 1
            for index, register in enumerate(parts[:count]):
                if op.startswith("st"):
                    if not register.startswith("x"):
                        raise RuntimeError("INVALID: unsupported load/store register width")
                    slots[address + index * 8] = registers.get(register)
                else:
                    if not register.startswith("x"):
                        raise RuntimeError("INVALID: unsupported load/store register width")
                    write(register, slots.get(address + index * 8))
            if pre:
                write(base, base=address)
            if post:
                write(base, base=bases[base] + int(post, 0))
        elif op == "xpaclri" or (op == "hint" and operands in ("#7", "#0x7")):
            xpac = True
            write("x30", "stripped_pc" if registers.get("x30") == "input_pc" else None)
        elif op == "bl":
            for index in range(19):
                write("x" + str(index))
            write("x30", "call_return")
        elif op == "ret":
            returns += 1
            if registers.get(parts[0] or "x30") != "incoming_lr":
                raise RuntimeError("actual return does not restore incoming LR")
            if require_stripped_return and registers.get("x0") != "stripped_pc":
                raise RuntimeError("strip result dataflow into x0 not established")
        elif op in ("b", "br", "blr", "cbz", "cbnz", "tbz", "tbnz") or op.startswith("b."):
            raise RuntimeError("INVALID: unsupported LR control-flow shape")
        elif op not in ("nop", "cmp", "tst", "cset", "adrp", "adr", "and", "orr", "eor", "ubfx"):
            raise RuntimeError("INVALID: unsupported instruction " + op)
        elif op not in ("nop", "cmp", "tst") and parts:
            write(parts[0])
    if not returns:
        raise RuntimeError("INVALID: no encoded return instruction")
    if require_stripped_return and not xpac:
        raise RuntimeError("actual strip/return instruction flow not established")
    return True


def instruction_checks():
    producer = (OUT / "producer-disassembly.log").read_text().lower()
    if not re.search(r"d503211f|1f 21 03 d5", producer):
        raise RuntimeError("producer lacks actual PACIA1716 encoding")
    assembly = (OUT / "product-disassembly.log").read_text().lower()
    target = function_block(assembly, "isn2cstubframe")
    inline = bool(re.search(r"xpaclri|hint\s+#(?:0x)?7\b", target))
    (OUT / "actual-n2c-caller.log").write_text(target)
    if not lr_flow(target, False):
        raise RuntimeError("INVALID: caller LR checker rejected")
    if not inline:
        if not re.search(r"\bbl\b[^\n]*<[^>]*ptrauthstripinstpointer", target):
            raise RuntimeError("actual caller does not bind real strip callee")
        callee = function_block(assembly, "ptrauthstripinstpointer")
        (OUT / "actual-strip-callee.log").write_text(callee)
        if not lr_flow(callee, True):
            raise RuntimeError("INVALID: callee LR checker rejected")
    (OUT / "actual-n2c-caller.log").write_text(target)
    (OUT / "instruction-check.json").write_text(json.dumps({"producer_encoding": "d503211f",
        "product_strip": "XPACLRI", "bounded_lr_text_check": True, "complete_cfg_proof": False,
        "inline": inline, "scope": "actual full-product N2C caller / bound strip callee"}, indent=2))


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
    if os.environ.get("PAC1481_DEADLINE") != PLAN["deadline_utc"]:
        raise RuntimeError("frozen absolute deadline mismatch")
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    if head != RESULT["head"] or os.environ.get("PAC1481_LABEL") != "p1481-" + head:
        raise RuntimeError("admitted event/head mismatch")
    if subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True).strip():
        raise RuntimeError("dirty checkout")
    if not PLAN["deadline_utc"]:
        raise RuntimeError("new candidate/deadline/event authorization MISSING")
    RESULT["tools"] = bind_tools(os.environ)
    save()
    require(["cmake", "--version"], "cmake-version.log")
    version = re.search(r"cmake version (\d+)\.(\d+)", (OUT / "cmake-version.log").read_text())
    if not version or tuple(map(int, version.groups())) < (3, 17):
        raise RuntimeError("CMake >=3.17 required for fresh child launcher environment")
    RESULT["compiler_entities"] = {name: bind for name in ("clang", "clang++")
        for bind in [__import__("recipe").entity(shutil.which(name) or "", "current runner PATH")]}
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
         ROOT / "runtime/tests/pac_strip_darwin/PLAN.json",
         ROOT / "runtime/tests/pac_strip_darwin/consumer-strip.diff")}
    require([RESULT["tools"]["sccache"]["path"], "--show-stats"], "sccache-before.log")
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
    nm, objdump, readobj = (RESULT["tools"][key]["path"] for key in ("llvm-nm", "llvm-objdump", "llvm-readobj"))
    require([nm, "--defined-only", libraries / "libcangjie-runtime.dylib"], "product-defined.log")
    require(["nm", "-gU", libraries / "libcangjie-runtime.dylib"], "product-exports.log")
    exported = (OUT / "product-exports.log").read_text()
    for symbol in ("_unwindPCForN2CStub", "_unwindPCForExclusiveStubFull", "IsN2CStubFrame"):
        if symbol not in exported:
            raise RuntimeError("missing real product export: " + symbol)
    require([objdump, "--disassemble", libraries / "libcangjie-runtime.dylib"], "product-disassembly.log")
    compile_entry, args = entry(pac, "runtime/src/UnwindStack/MachineFrame.cpp", "UnwindStack")
    mem_entry, mem_args = entry(pac, "runtime/src/Base/MemUtils.cpp", "Base")
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
    try:
        instruction_checks()
    except Exception as error:
        (OUT / "instruction-invalid.json").write_text(json.dumps({"status": "INVALID", "reason": str(error), "behavior": "NOT_RUN"}))
        raise
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
    try:
        run(["uptime"], "uptime-after.log", timeout=10)
    except RuntimeError:
        RESULT["uptime_after"] = "NOT_RUN absolute deadline"
    preserved = preserve_arms(OUT, ("default-source", "pac-source", "cut-source"), RESULT)
    save()
    if not preserved:
        sys.exit(20)
