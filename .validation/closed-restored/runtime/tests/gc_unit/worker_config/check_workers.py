#!/usr/bin/env python3
"""Observe real runtime workers through /proc and the product REPORT log.
Run with an existing fixture ELF, SO directory and evidence directory.
No model/helper implementation of worker selection is linked into the fixture.
"""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

elf, so, out = map(lambda p: Path(p).resolve(), sys.argv[1:4])
out.mkdir(parents=True, exist_ok=True)
# The same 16-CPU affinity and 256MB heap give an ergonomic generation budget of 2.
# Expected tuples are contract examples, not an implementation of the formula.
cases = [
    ('default_dynamic', 'api', (0, 0, 0), (2, 2, 2)),
    ('explicit_total', 'api', (5, 0, 0), (5, 5, 5)),
    ('explicit_generations', 'api', (6, 3, 4), (6, 3, 4)),
    ('young_raises_total', 'api', (0, 5, 0), (5, 5, 2)),
    ('old_raises_total', 'api', (0, 0, 5), (5, 2, 5)),
    ('static_default', 'static', (0, 0, 0), (2, 1, 1)),
    ('static_total', 'static', (10, 0, 0), (10, 9, 1)),
    ('static_young', 'static', (10, 4, 0), (10, 4, 6)),
    ('static_old', 'static', (10, 0, 4), (10, 9, 4)),
    ('static_explicit', 'static', (10, 3, 4), (10, 3, 4)),
    ('minimum', 'static', (1, 0, 0), (1, 1, 1)),
]
# Exercise both supported configuration producers with the same observations.
cases += [(name + '_env', 'env_static' if mode == 'static' else 'env', values, expected)
          for name, mode, values, expected in list(cases)]

if len(sys.argv) > 4:
    selected = set(sys.argv[4].split(','))
    cases = [case for case in cases if case[0] in selected]
    if len(cases) != len(selected):
        raise SystemExit('unknown case filter')

def run(case):
    name, mode, values, expected = case
    work = out / name
    work.mkdir(exist_ok=True)
    env = dict(os.environ)
    for key in ('cjConcGCThreads', 'cjYoungGCThreads', 'cjOldGCThreads', 'cjUseDynamicNumberOfGCThreads'):
        env.pop(key, None)
    env.update(LD_LIBRARY_PATH=str(so), MRT_REPORT=str(work / 'mrt_report.txt'), cjHeapSize='256MB', cjProcessorNum='1')
    if mode.startswith('env'):
        for key, value in zip(('cjConcGCThreads', 'cjYoungGCThreads', 'cjOldGCThreads'), values):
            if value: env[key] = str(value)
        env['cjUseDynamicNumberOfGCThreads'] = '0' if mode == 'env_static' else '1'
    command = [str(elf), 'env' if mode.startswith('env') else mode, *map(str, values)]
    result = subprocess.run(command, env=env, cwd=work, text=True, capture_output=True, timeout=30)
    (work / 'stdout').write_text(result.stdout)
    (work / 'stderr').write_text(result.stderr)
    (work / 'rc').write_text(str(result.returncode))
    reports = '\n'.join(p.read_text() for p in work.glob('mrt_report*'))
    observation = re.search(r'OBSERVED young=(\d+) old=(\d+)', result.stdout)
    report = re.search(r'concurrent gc thread count (\d+), young (\d+), old (\d+)', reports)
    observed = tuple(map(int, observation.groups())) if observation else None
    budget = tuple(map(int, report.groups())) if report else None
    passed = result.returncode == 0 and observed == expected[1:] and budget == expected
    print(f'ASSERT_EXECUTED {name} rc={result.returncode} proc={observed} report={budget} expected={expected} {"PASS" if passed else "FAIL"}', flush=True)
    return dict(name=name, rc=result.returncode, proc=observed, report=budget, expected=expected, passed=passed)

identity = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in
            (elf, so / 'libcangjie-runtime.so', so / 'libboundscheck.so')}
(out / 'identity.json').write_text(json.dumps(identity, indent=2))
(out / 'affinity').write_text(str(sorted(os.sched_getaffinity(0))))
(out / 'uptime.before').write_text(subprocess.check_output(['uptime'], text=True))
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    results = list(pool.map(run, cases))
(out / 'uptime.after').write_text(subprocess.check_output(['uptime'], text=True))
(out / 'results.json').write_text(json.dumps(results, indent=2))
sys.exit(0 if all(r['passed'] for r in results) else 1)
