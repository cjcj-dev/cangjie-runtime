#!/usr/bin/env python3
"""Replay the merged LLVM entry contracts on new and old product backends."""
import concurrent.futures
import json
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
old = Path('/root/sym_cangjie_runtime_560b_sym_cangjie_runtime_564_implement_r5664061027/gate-sdk/third_party/llvm/bin/llc')
checker = root / 'llvm-build/bin/FileCheck'

def run(item):
    arm, level, kind = item
    out = root / 'entry-checks' / f'{arm}-{level}-{kind}'
    out.mkdir(parents=True, exist_ok=True)
    src = root / 'llvm-src/llvm/test/CodeGen/X86/CangjieGC' / ('static-write-all-phases.ll' if kind == 'static' else 'static-atomic-runtime-contract.ll')
    llc = old if arm == 'old' else root / 'llvm-build/bin/llc'
    asm = out / 'output.s'
    with src.open() as inp, (out / 'lowered.log').open('w') as log:
        rc = subprocess.run([str(llc), '--cangjie-pipeline', '-mtriple=x86_64', '-' + level,
                             '-print-after=cj-barrier-lowering', '-o', str(asm)], stdin=inp, stdout=log, stderr=subprocess.STDOUT).returncode
    results = dict(arm=arm, level=level, kind=kind, llc_rc=rc)
    if rc == 0:
        cmd = [str(checker), str(src), '--implicit-check-not=gcNoRunning']
        if kind == 'static':
            cmd += ['--check-prefix=IR', '--implicit-check-not=call{{.*}}@GetGCPhase']
        with (out / 'lowered.log').open() as inp, (out / 'ir-check.log').open('w') as log:
            results['ir_rc'] = subprocess.run(cmd, stdin=inp, stdout=log, stderr=subprocess.STDOUT).returncode
        if kind == 'static':
            with asm.open() as inp, (out / 'asm-check.log').open('w') as log:
                results['asm_rc'] = subprocess.run([str(checker), str(src), '--check-prefix=ASM', '--implicit-check-not=GetGCPhase'], stdin=inp, stdout=log, stderr=subprocess.STDOUT).returncode
    (out / 'result.json').write_text(json.dumps(results, indent=2) + '\n')
    return results

with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    results = list(pool.map(run, [(a, o, k) for a in ['old', 'new'] for o in ['O0', 'O2'] for k in ['static', 'atomic']]))
(root / 'evidence/entry-checks.json').write_text(json.dumps(results, indent=2) + '\n')
for r in results:
    print(r)
    expected = 1 if r['arm'] == 'old' and r['kind'] == 'static' else 0
    if r['llc_rc'] != 0 or r.get('ir_rc') != expected or r.get('asm_rc', expected) != expected:
        sys.exit(1)
