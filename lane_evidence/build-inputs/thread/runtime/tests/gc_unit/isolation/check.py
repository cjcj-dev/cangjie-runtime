#!/usr/bin/env python3
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Run on kkk2: check.py <scene ELF> <output directory> [N=5].

Compile scene.cpp with -g -O0 -pthread and -I pointing at the actual gc_unit
headers. All arms use the unchanged 60-second harness deadline. This driver
checks scene stacks and process custody, not just an expected failure rc.
"""
import concurrent.futures
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

binary = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
n = int(sys.argv[3]) if len(sys.argv) > 3 else 5
out.mkdir(parents=True, exist_ok=True)
assert ctypes.CDLL(None).prctl(36, 1, 0, 0, 0) == 0  # Adopt cut-arm survivors for cleanup only.
(out / 'elf.sha256').write_text(hashlib.sha256(binary.read_bytes()).hexdigest() + '\n')
(out / 'uptime-before.txt').write_text(subprocess.check_output(['uptime'], text=True))
(out / 'cpu-affinity.txt').write_text(str(sorted(os.sched_getaffinity(0))) + '\n')


def run(case):
    mode, depth, iteration = case
    directory = out / f'{mode}-depth{depth}-n{iteration}'
    directory.mkdir()
    env = dict(os.environ, ISOLATION_MODE=mode, ISOLATION_DEPTH=str(depth))
    started = time.monotonic()
    with (directory / 'run.log').open('w') as log:
        result = subprocess.run([str(binary)], env=env, stdout=log, stderr=log, timeout=110)
    text = (directory / 'run.log').read_text()
    leaves = [int(pid) for pid in re.findall(r'ISOLATION_LEAF_PID=(\d+)', text)]
    remaining = [pid for pid in leaves if Path(f'/proc/{pid}').exists()]
    expected_rc = 1 if mode in ('block', 'missing', 'wrong') else 0
    stack_seen = ' in IsolationBlockedWorker ' in text
    ok = result.returncode == expected_rc and len(leaves) == 1 and not remaining
    if mode == 'block':
        ok = ok and stack_seen and '[ STACKS ] collector status=0' in text
    if mode == 'abort':
        ok = ok and 'matched=1' in text
    record = dict(mode=mode, depth=depth, iteration=iteration, rc=result.returncode,
                  wall=time.monotonic() - started, leaves=leaves, survivors=remaining,
                  stack_seen=stack_seen, ok=ok)
    (directory / 'result.json').write_text(json.dumps(record, indent=2))
    return record


cases = [(mode, depth, i) for mode in ('pass', 'abort', 'missing', 'wrong', 'block')
         for depth in (1, 2, 3) for i in range(1, n + 1)]
with concurrent.futures.ThreadPoolExecutor(max_workers=15) as pool:
    results = list(pool.map(run, cases))

# No scenario is still running here. Reap only this driver's adopted children;
# never use a process-name pattern or alter the tested supervisor's behavior.
adopted = set()
while True:
    children = set()
    for task in Path(f'/proc/{os.getpid()}/task').iterdir():
        try:
            children.update(int(pid) for pid in (task / 'children').read_text().split())
        except FileNotFoundError:
            pass  # A joined driver thread can disappear during enumeration.
    if not children:
        break
    adopted.update(children)
    for pid in children:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    for pid in children:
        os.waitpid(pid, 0)
record = dict(results=results, adopted_after_runs=sorted(adopted))
(out / 'result.json').write_text(json.dumps(record, indent=2))
(out / 'uptime-after.txt').write_text(subprocess.check_output(['uptime'], text=True))
failed = [f"{r['mode']}/depth{r['depth']}/n{r['iteration']}" for r in results if not r['ok']]
print(json.dumps(dict(total=len(results), failed=failed, adopted_after_runs=sorted(adopted))))
sys.exit(1 if failed or adopted else 0)
