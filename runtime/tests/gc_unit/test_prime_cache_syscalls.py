#!/usr/bin/env python3
"""Observe physical commits through strace; no runtime counters or interposition.

Use the same product-linked cj_gc_unit ELF and replace only LD_LIBRARY_PATH.
ZGC anchors: zPageAllocator.cpp:952-994 (prime), :690-697 (cache return).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--lib", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cold-baseline", action="store_true")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    artifacts = [args.elf, args.lib / "libcangjie-runtime.so", args.lib / "libboundscheck.so"]
    identity = {str(p.resolve()): sha256(p) for p in artifacts}
    env = dict(os.environ, LD_LIBRARY_PATH=str(args.lib.resolve()))
    trace = args.out / "syscalls.log"
    log = args.out / "run.log"
    before = subprocess.check_output(["uptime"], text=True).strip()
    start = time.monotonic()
    with log.open("w") as output:
        run = subprocess.run([
            "strace", "-f", "-s", "512", "-e", "trace=fallocate,write",
            "-o", str(trace), str(args.elf.resolve()),
            "--gtest_filter=PrimeCache.RepeatedPagesReuseCommittedCapacity",
        ], env=env, stdout=output, stderr=subprocess.STDOUT, timeout=60)
    after = subprocess.check_output(["uptime"], text=True).strip()
    counts = [0, 0, 0, 0]
    began, ended = [], []
    active = None
    for line in trace.read_text().splitlines():
        marker = re.search(r'write\(1, "PrimeCache begin round=(\d+)', line)
        if marker:
            active = int(marker[1])
            began.append(active)
        marker = re.search(r'write\(1, "PrimeCache round=(\d+)', line)
        if marker:
            ended.append(int(marker[1]))
            active = None
        # mode=0 allocates backing. FALLOC_FL_PUNCH_HOLE is uncommit.
        if active is not None and re.search(r'fallocate\(\d+, 0, .*\)\s+= 0$', line):
            counts[active] += 1
    qualified = run.returncode == 0 and began == list(range(4)) and ended == list(range(4))
    # 32 small pages, four granules primed. First round is the positive
    # control: cold pages really commit. Every later round must reuse them.
    first = 32 if args.cold_baseline else 28
    passed = qualified and counts == [first, 0, 0, 0]
    result = dict(rc=run.returncode, qualified=qualified, counts=counts,
                  expected=[first, 0, 0, 0], passed=passed, identity=identity,
                  cpu_affinity=sorted(os.sched_getaffinity(0)),
                  uptime_before=before, uptime_after=after,
                  wall=time.monotonic() - start, began=began, ended=ended)
    (args.out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("PRIME_COMMIT_TARGET " + json.dumps(result, sort_keys=True))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
