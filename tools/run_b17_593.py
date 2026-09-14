#!/usr/bin/env python3
"""Record managed runner exits, LOADFC observations, and loaded artifact identity."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

root = Path(sys.argv[1]).resolve()
cores = sys.argv[2:]
if len(cores) != 3:
    raise SystemExit('supply three reserved CPU ranges')
sdk = Path('/root/sym_cangjie_runtime_585_implement_r5669360435/gate-sdk')
lib = root / 'testable/build/runtime-staging/lib/x86_64_Release'
host = Path('/root/sym_cangjie_runtime_585_implement_r5669360435/host')
env = os.environ.copy()
env.update(CANGJIE_HOME=str(sdk), CJC=str(sdk / 'bin/cjc'),
           GC_UNIT_CJC_RUNTIME_LIB_DIR=str(host / 'runtime/lib/linux_x86_64_cjnative'),
           GCV2_RUNTIME_LIB_DIR=str(lib), CXX='clang++', cjHeapSize='24GB',
           LD_LIBRARY_PATH=f'{host}/runtime/lib/linux_x86_64_cjnative:{host}/third_party/llvm/lib:{host}/tools/lib',
           PATH=f'{sdk}/bin:{sdk}/tools/bin:{sdk}/third_party/llvm/bin:/usr/bin:/bin')
evidence = root / 'evidence'
subprocess.run(['uptime'], stdout=(evidence / (os.environ.get('B17_OUT', 'managed') + '-before.txt')).open('w'), check=True)
jobs = []
for i, cpu in enumerate(cores, 1):
    for name in os.environ.get('B17_RUNNERS', 'finalizer_trigger segmented_array_managed phase_entry_trigger').split():
        out = root / os.environ.get('B17_OUT', 'managed') / f'{name}-{i}'
        out.mkdir(parents=True, exist_ok=True)
        (out / 'cores.txt').write_text(cpu + '\n')
        runenv = dict(env, GC_UNIT_OUT=str(out), LD_DEBUG='files', LD_DEBUG_OUTPUT=str(out / 'loader'))
        log = (out / 'runner.log').open('w')
        argv = ['taskset', '-c', cpu, 'bash', str(root / 'testable/runtime/tests/gc_unit' / f'run_{name}.sh')]
        proc = subprocess.Popen(argv, env=runenv, stdout=log, stderr=subprocess.STDOUT)
        jobs.append((proc, out, log, time.monotonic(), argv))
seen = set()
while any(p.poll() is None for p, *_ in jobs):
    for pd in Path('/proc').iterdir():
        if not pd.name.isdigit() or pd.name in seen:
            continue
        try:
            exe = (pd / 'exe').resolve(strict=True)
            if root / os.environ.get('B17_OUT', 'managed') not in exe.parents:
                continue
            out = exe.parent
            (out / f'maps-{pd.name}.txt').write_text((pd / 'maps').read_text())
            (out / f'exe-{pd.name}.txt').write_text(str(exe) + '\n')
            if str(lib / 'libcangjie-runtime.so') in (out / f'maps-{pd.name}.txt').read_text():
                seen.add(pd.name)
        except (OSError, ValueError):
            pass
    time.sleep(0.02)
results = []
for proc, out, log, start, argv in jobs:
    rc = proc.wait()
    log.close()
    (out / 'runner.rc').write_text(str(rc) + '\n')
    files = {}
    for p in out.iterdir():
        if p.is_file() and p.open('rb').read(4) == b'\x7fELF':
            files[str(p)] = hashlib.sha256(p.read_bytes()).hexdigest()
            with (out / (p.name + '.ldd')).open('w') as f:
                lr = subprocess.run(['ldd', str(p)], env=dict(env, LD_LIBRARY_PATH=str(lib) + ':' + str(sdk / 'runtime/lib/linux_x86_64_cjnative')), stdout=f, stderr=subprocess.STDOUT)
            (out / (p.name + '.ldd.rc')).write_text(str(lr.returncode) + '\n')
    observations = []
    markstale = []
    # Count original workload logs only; runner.log can duplicate their tail.
    for p in out.glob('*.log'):
        if p.name == 'runner.log' or p.name.endswith('.build.log'):
            continue
        for n, line in enumerate(p.read_text(errors='replace').splitlines(), 1):
            if 'MARKSTALE' in line:
                markstale.append(dict(file=str(p), line=n, text=line))
            if 'LOADFC' in line:
                observations.append(dict(file=str(p), line=n, text=line))
    result = dict(out=str(out), argv=argv, rc=rc, wall=time.monotonic()-start,
                  cores=(out / 'cores.txt').read_text().strip(), elf_sha256=files,
                  loadfc_count=len(observations), loadfc=observations, markstale_count=len(markstale), markstale=markstale)
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    results.append(result)
subprocess.run(['uptime'], stdout=(evidence / (os.environ.get('B17_OUT', 'managed') + '-after.txt')).open('w'), check=True)
(evidence / (os.environ.get('B17_OUT', 'managed') + '-results.json')).write_text(json.dumps(results, indent=2) + '\n')
for r in results:
    print(Path(r['out']).name, 'rc=' + str(r['rc']), 'LOADFC=' + str(r['loadfc_count']))
