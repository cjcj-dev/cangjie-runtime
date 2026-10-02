"""Read-only runner qualification and preservation; no subprocess invocation."""
import hashlib
import os
from pathlib import Path
import shutil
import json
import re
import shlex


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def entity(value, source):
    path = Path(value)
    if not path.is_absolute():
        raise RuntimeError("tool must be absolute: " + value)
    path = path.resolve(strict=True)
    st = path.stat()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise RuntimeError("tool not executable: " + str(path))
    return {"path": str(path), "source": source, "sha256": digest(path),
            "size": st.st_size, "device": st.st_dev, "inode": st.st_ino}


def bind_tools(env):
    cache = entity(env["SCCACHE_PATH"], "current runner SCCACHE_PATH")
    root = Path(env["PAC1481_LLVM_BIN"])
    if not root.is_absolute():
        raise RuntimeError("LLVM installed collection must be absolute")
    tools = {name: entity(str(root / name), "current installed LLVM collection " + str(root))
             for name in ("llvm-nm", "llvm-objdump", "llvm-readobj")}
    if len({str(Path(v["path"]).parent) for v in tools.values()}) != 1:
        raise RuntimeError("object readers resolve to different collections")
    return {"sccache": cache, **tools}


def configure_env(env, tools):
    result = dict(env)
    for lang in ("C", "CXX"):
        result["CMAKE_" + lang + "_COMPILER_LAUNCHER"] = tools["sccache"]["path"]
    return result


def verify_configured(build, tools):
    cache = tools["sccache"]["path"]
    records = {}
    for label, root in (("top", build), ("child", build / "cjthread-build")):
        text = (root / "CMakeCache.txt").read_text()
        for key in ("CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER"):
            rows = [line.split("=", 1)[1] for line in text.splitlines()
                    if line.startswith(key + ":")]
            if rows != [cache]:
                raise RuntimeError(label + " cache launcher mismatch: " + key)
        if label == "top" and ("CANGJIE_COMPILER_CACHE:FILEPATH=" + cache) not in text.splitlines():
            raise RuntimeError("top cache entity mismatch")
        # Make recipes bind launcher, language and owning target in one command.
        # Never admit child text as evidence for the top build domain.
        consumed = []
        for path in root.rglob("build.make"):
            if label == "top" and (build / "cjthread-build") in path.parents:
                continue
            target = path.parent.name
            if not target.endswith(".dir"):
                continue
            for line in path.read_text().splitlines():
                if not line.startswith("\t") or " -c " not in line:
                    continue
                languages = re.findall(r"\$\((C|CXX)_FLAGS\)", line)
                if not languages and "$(ASM_FLAGS)" in line:
                    continue
                if not languages:
                    raise RuntimeError(label + " unsupported compile recipe: " + str(path))
                command = line.strip().split("&&")[-1].strip()
                words = shlex.split(command)
                if not words or words[0] != cache or "CMakeFiles/" + target + "/" not in line:
                    raise RuntimeError(label + " generated launcher consumption mismatch: " + str(path))
                consumed.append({"file": str(path), "target": target,
                                 "language": languages[0], "command": line.strip()})
        if not consumed:
            raise RuntimeError(label + " generated launcher consumption MISSING")
        records[label] = {"cache": str(root / "CMakeCache.txt"), "commands": consumed}
    for value in tools.values():
        if digest(Path(value["path"])) != value["sha256"]:
            raise RuntimeError("frozen tool changed")
    return records


def preserve_then_delete(tree, target):
    """Only delete after every selected real input has a verified retained copy."""
    target.mkdir(parents=True, exist_ok=False)
    selected = []
    names = {"CMakeCache.txt", "compile_commands.json", "link.txt", "flags.make",
             "build.make", "build.ninja", "rules.ninja",
             "runtime-build-inputs.txt", "runtime-build-config.txt",
             "runtime-product-hashes.json", "runtime-publish-args.txt"}
    for p in tree.rglob("*"):
        if p.is_file() and (p.name in names or p.suffix in (".rsp", ".log", ".rc") or
            "MachineFrame.cpp" in p.name or "MemUtils.cpp" in p.name or
            (p.name.startswith("CMake") and p.name.endswith("Compiler.cmake")) or
            p.suffix in (".o", ".obj", ".a", ".dylib")):
            selected.append(p)
    for rel in ("runtime/src/UnwindStack/MachineFrame.cpp", "runtime/src/Base/MemUtils.cpp"):
        p = tree / rel
        if p.is_file() and p not in selected:
            selected.append(p)
    # Only these generation-time records contain paths (RuntimeOutputLayout:67,84).
    # runtime-build-inputs.txt is canonical JSON identity (publisher:142).
    for listing in sorted(tree.rglob("runtime-cjthread-inputs.txt")) + sorted(tree.rglob("runtime-link-inputs.txt")):
        selected.append(listing)
        for line in listing.read_text().splitlines():
            if not line.strip():
                continue
            dependency = Path(line.strip())
            if not dependency.is_absolute():
                dependency = listing.parent / dependency
            dependency = dependency.resolve()
            if not dependency.is_relative_to(tree.resolve()) or not dependency.is_file():
                raise RuntimeError("formed publisher input missing/outside tree: " + str(dependency))
            selected.append(dependency)
    for path in tree.rglob("*"):
        if path.is_file() and ("runtime-generated-inputs" in path.parts or
                "identity" in path.name or "provenance" in path.name or
                "publish" in path.name or "publisher" in path.name):
            selected.append(path)
    selected = sorted(set(selected))
    records = []
    for p in selected:
        dest = target / p.relative_to(tree)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dest)
        value = digest(p)
        if digest(dest) != value:
            raise RuntimeError("preservation hash mismatch; original tree retained")
        records.append({"path": str(p.relative_to(tree)), "sha256": value})
    for expected in ("CMakeCache.txt", "compile_commands.json", "link.txt", "flags.make",
                     ".rsp", "MachineFrame.cpp.o", "MemUtils.cpp.o"):
        if not any(expected in r.get("path", "") for r in records):
            records.append({"expected": expected, "status": "MISSING"})
    (target / "inventory.json").write_text(json.dumps(records, indent=2))
    shutil.rmtree(tree)
    return records


def preserve_arms(out, names, result):
    """Independent arm receipts; preservation errors never replace first_error."""
    receipts = {}
    for name in names:
        tree = out / name
        if not tree.exists():
            receipts[name] = {"status": "NOT_CREATED"}
            continue
        try:
            inventory = preserve_then_delete(tree, out / (name + "-inputs"))
            receipts[name] = {"status": "COMPLETED", "inventory": inventory}
        except Exception as error:
            receipts[name] = {"status": "FAILED_RETAINED", "error": str(error),
                              "tree_exists": tree.exists()}
    result["preservation"] = receipts
    result["preservation_errors"] = {n: r["error"] for n, r in receipts.items() if "error" in r}
    return not result["preservation_errors"]
