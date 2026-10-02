"""Read-only runner qualification and preservation; no subprocess invocation."""
import hashlib
import os
from pathlib import Path
import shutil
import json


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
        commands = [p for p in root.rglob("*") if p.is_file() and
                    p.name in ("build.make", "build.ninja", "compile_commands.json")]
        consumed = [str(p) for p in commands if cache in p.read_text(errors="replace")]
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
             "build.make", "build.ninja", "rules.ninja"}
    for p in tree.rglob("*"):
        if p.is_file() and (p.name in names or p.suffix in (".rsp", ".log", ".rc") or
            "MachineFrame.cpp" in p.name or "MemUtils.cpp" in p.name or
            p.suffix in (".o", ".obj")):
            selected.append(p)
    for rel in ("runtime/src/UnwindStack/MachineFrame.cpp", "runtime/src/Base/MemUtils.cpp"):
        p = tree / rel
        if p.is_file() and p not in selected:
            selected.append(p)
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
        if not any(expected in r["path"] for r in records):
            records.append({"expected": expected, "status": "MISSING"})
    (target / "inventory.json").write_text(json.dumps(records, indent=2))
    shutil.rmtree(tree)
    return records
