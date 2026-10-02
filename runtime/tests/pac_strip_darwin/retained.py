"""One fixed-artifact continuation; existing execution and assertions only."""
import ast
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

source = Path(__file__).with_name("run.py")
receipt = json.loads(source.with_name("RETAINED.json").read_text())
# Load the existing definitions and initialization, excluding its batch driver.
# No copied assertion, reader override, product build or fixture compilation.
module = ast.parse(source.read_text(), filename=str(source))
assert isinstance(module.body[-1], ast.Try)
module.body.pop()
namespace = {"__file__": str(source), "__name__": "retained_execution"}
exec(compile(module, str(source), "exec"), namespace)
out = namespace["OUT"]
result = namespace["RESULT"]
save = namespace["save"]
require = namespace["require"]
sha = namespace["sha"]

try:
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    deadline = os.environ.get("PAC1481_RETAINED_DEADLINE", "")
    if (not deadline or head != os.environ.get("PAC1481_RETAINED_HEAD") or
            os.environ.get("PAC1481_LABEL") != "retained1481-" + head or
            os.environ.get("GITHUB_RUN_ATTEMPT") != "1"):
        raise RuntimeError("new exact head/event/deadline activation missing")
    if subprocess.check_output(["git", "status", "--porcelain"], text=True).strip():
        raise RuntimeError("dirty checkout")
    activated = datetime.fromisoformat(os.environ["PAC1481_RETAINED_ACTIVATED_AT"]).timestamp()
    expires = datetime.fromisoformat(deadline).timestamp()
    if expires - activated != 4800 or not activated <= time.time() < expires - 120:
        raise RuntimeError("fixed control 80-minute window unavailable")
    namespace["DEADLINE"] = expires
    result.update(deadline_utc=deadline, static_receipt=receipt,
                  run_id=os.environ["GITHUB_RUN_ID"], product_builds=0,
                  native_limit=1, cut="NOT_RUN not licensed", restored="NOT_RUN not licensed")
    save()
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise RuntimeError("official macos-15 arm64 required")
    require(["xcodebuild", "-version"], "xcode.log")
    if "Xcode 16.4" not in (out / "xcode.log").read_text().splitlines()[1:]:
        raise RuntimeError("Xcode16.4 required")
    require(["xcrun", "--sdk", "macosx", "--show-sdk-version"], "sdk-version.log")
    if (out / "sdk-version.log").read_text().splitlines()[-1] != "15.5":
        raise RuntimeError("SDK15.5 required")
    sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
    compiler = subprocess.check_output(["xcrun", "--find", "clang++"], text=True).strip()
    linker = subprocess.check_output(["xcrun", "--find", "ld"], text=True).strip()
    from recipe import entity
    result["new_tools"] = {p: entity(p, "current Xcode; no old byte identity claim") for p in (compiler, linker)}
    result["sdk_path"] = sdk
    result["sdk_settings_sha256"] = sha(Path(sdk) / "SDKSettings.json")
    require([linker, "-v"], "linker-version.log")
    require([compiler, "--version"], "compiler.log")
    if "Apple clang version 17." not in (out / "compiler.log").read_text():
        raise RuntimeError("Apple17 required")
    archive = Path(sys.argv[2]).resolve(strict=True)
    if sha(archive) != receipt["archive_sha256"]:
        raise RuntimeError("original artifact download hash mismatch")
    result["download"] = {"path": str(archive), "sha256": sha(archive)}
    import zipfile
    extracted = out / "original-artifact"
    with zipfile.ZipFile(archive) as bundle:
        if any(Path(n).is_absolute() or ".." in Path(n).parts for n in bundle.namelist()):
            raise RuntimeError("unsafe archive member")
        bundle.extractall(extracted)
    original = extracted / "sym_cangjie_runtime_1481_implement_r5960917053"
    libraries = out / "pac-dylibs"
    libraries.mkdir()
    for relative, expected in receipt["files"].items():
        origin = original / relative
        if sha(origin) != expected:
            raise RuntimeError("fixed input hash mismatch: " + relative)
        target = libraries / origin.name if origin.suffix == ".dylib" else out / origin.name
        shutil.copy2(origin, target)
        if sha(target) != expected:
            raise RuntimeError("entity copy mismatch")
    # Original generated inputs remain in the extracted artifact, without edits.
    require(["uptime"], "uptime-before.log")
    for library in libraries.iterdir():
        require(["otool", "-L", library], library.name + "-dependencies.log")
        require(["otool", "-l", library], library.name + "-load-commands.log")
    elf = out / "consumer"
    require([compiler, "-arch", "arm64", "-isysroot", sdk, "-fno-lto",
             out / "consumer.o", out / "producer.o", "-L" + str(libraries),
             "-Wl,-rpath," + str(libraries), "-lcangjie-runtime", "-lboundscheck", "-o", elf], "fixture-link.log")
    result["fixture_hashes"] = {p.name: sha(p) for p in (elf, out / "producer.o", out / "consumer.o")}
    save()
    require(["otool", "-L", elf], "fixture-dependencies.log")
    os.environ["DYLD_PRINT_SEGMENTS"] = "1"
    # Existing arm logs actual loader addresses/segments plus fixture MAPPINGs.
    rc, rows = namespace["arm"]("green", libraries, elf)
    if rc == 21:
        result["stop"] = "INCONCLUSIVE signed==raw; no resampling"
        sys.exit(21)
    if rc != 0 or [row[7] for row in rows] != ["1", "1"]:
        raise RuntimeError("green target/control failure")
    result["stop"] = "one green only; independent result review required; causal obligations outstanding"
except Exception as error:
    result["first_error"] = str(error)
    print(str(error), file=sys.stderr)
    sys.exit(20)
finally:
    save()
