#!/usr/bin/env python3
"""Check the instantiated product header using a CMake Heap.dir flags.make recipe.

Content tests cannot distinguish an atomic word copy from an ordinary copy.
This compile test observes LLVM atomic operations; run the ordinary-access cut
with this same script and flags to establish its sensitivity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--flags", type=Path, required=True)
parser.add_argument("--out", type=Path, required=True)
parser.add_argument("--compiler", default="clang++")
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
probe = Path(__file__).with_name("zutils_atomic_probe.cpp")
flags = {}
for line in args.flags.read_text().splitlines():
    if " = " in line:
        key, value = line.split(" = ", 1)
        flags[key] = shlex.split(value)
command = [args.compiler] + flags["CXX_DEFINES"] + flags["CXX_INCLUDES"] + flags["CXX_FLAGS"]
command += ["-S", "-emit-llvm", str(probe), "-o", str(args.out / "copy.ll")]
start = time.monotonic()
result = subprocess.run(command, text=True, capture_output=True)
(args.out / "compile.log").write_text(result.stdout + result.stderr)
record = {"command": command, "compile_rc": result.returncode, "wall": time.monotonic() - start,
          "probe_sha256": hashlib.sha256(probe.read_bytes()).hexdigest(), "checks": {}}
if result.returncode == 0:
    ir = (args.out / "copy.ll").read_text()
    record["ir_sha256"] = hashlib.sha256(ir.encode()).hexdigest()
    definitions = dict(re.findall(r"^define[^\n]*@([^ (]+)\([^\n]*\)[^\n]*\{\n(.*?)^}", ir, re.M | re.S))

    def reachable(name, seen):
        if name in seen or name not in definitions:
            return ""
        seen.add(name)
        body = definitions[name]
        # Follow actual emitted calls when the compiler chooses not to inline
        # Copy. This neither forces inlining nor treats a declaration as proof.
        for callee in re.findall(r"(?:call|invoke) [^\n]*?@([^ (]+)\(", body):
            if callee in definitions:
                body += "\n" + reachable(callee, seen)
        return body

    for name in ("zutils_atomic_dynamic", "zutils_atomic_small", "zutils_atomic_large", "zutils_disjoint_control"):
        match = re.search(r"^define[^\n]*@" + name + r"\([^\n]*\)[^\n]*\{\n(.*?)^}", ir, re.M | re.S)
        # Existence and the actual invariant are separate records.
        record["checks"][name + ".present"] = match is not None
        if match is None:
            continue
        visited = set()
        body = reachable(name, visited)
        record[name + ".reachable"] = sorted(visited)
        (args.out / (name + ".ll")).write_text(body)
        if name == "zutils_disjoint_control":
            ok = "llvm.memcpy" in body and not re.search(r"\b(load|store) atomic\b", body)
            record["checks"][name + ".ordinary_copy"] = bool(ok)
        else:
            loads = re.findall(r"\bload atomic i64\b", body)
            stores = re.findall(r"\bstore atomic i64\b", body)
            ordinary = re.search(r"\b(?:load|store) (?!atomic\b)", body)
            memory_call = re.search(r"@(?:llvm.memcpy|llvm.memmove|memcpy|memmove)", body)
            record["checks"][name + ".word_atomic_access"] = bool(loads and stores and not ordinary and not memory_call)
            record[name] = {"atomic_load_sites": len(loads), "atomic_store_sites": len(stores)}
    record["rc"] = int(len(record["checks"]) != 8 or not all(record["checks"].values()))
else:
    record["rc"] = 2
(args.out / "result.json").write_text(json.dumps(record, indent=2) + "\n")
for name, ok in record["checks"].items():
    print(("PASS " if ok else "FAIL ") + name)
print("COMPILE_RC=" + str(record["compile_rc"]) + " TEST_RC=" + str(record["rc"]))
raise SystemExit(record["rc"])
