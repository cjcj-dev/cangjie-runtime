#!/usr/bin/env python3
"""Compile the same source through the old and rebuilt complete SDKs at O0/O2."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

root = Path(sys.argv[1]).resolve()
old = Path('/root/sym_cangjie_runtime_560b_sym_cangjie_runtime_564_implement_r5664061027')
reference = Path('/root/sym_cjcj_llvm_1_implement_r5668195885')
source = reference / 'global_slot_probe.cj'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def compile_one(item):
    arm, level = item
    sdk = old / 'gate-sdk' if arm == 'baseline' else root / 'gate-sdk'
    out = root / arm / 'replay' / level
    out.mkdir(parents=True, exist_ok=True)
    (out / 'temps').mkdir(exist_ok=True)
    (out / source.name).write_bytes(source.read_bytes())
    env = os.environ.copy()
    env['CANGJIE_HOME'] = str(sdk)
    env['LD_LIBRARY_PATH'] = f'{old}/host/runtime/lib/linux_x86_64_cjnative:{sdk}/tools/lib:{old}/host/third_party/llvm/lib'
    argv = [str(sdk / 'bin/cjc'), source.name, '-' + level, '-g', '--static-std',
            '--save-temps', str(out / 'temps'), '-V', '-o', str(out / 'global_slot_probe')]
    start = time.monotonic()
    inputs = [sdk / 'bin/cjc', sdk / 'third_party/llvm/bin/llc', sdk / 'third_party/llvm/bin/opt', source]
    inputs += sorted((sdk / 'lib/linux_x86_64_cjnative').glob('libcangjie-std-*.a'))
    (out / 'inputs.json').write_text(json.dumps({str(p): sha(p) for p in inputs}, indent=2))
    with (out / 'compile.log').open('w') as log:
        rc = subprocess.run(argv, cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
    (out / 'compile.rc').write_text(str(rc) + '\n')
    if rc == 0:
        elf = out / 'global_slot_probe'
        (out / 'elf.sha256').write_text(sha(elf) + '  ' + str(elf) + '\n')
        for command, name in [(['objdump', '-drC', str(elf)], 'disassembly'),
                              (['nm', '--defined-only', str(elf)], 'defined')]:
            with (out / (name + '.txt')).open('w') as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            (out / (name + '.rc')).write_text(str(result.returncode) + '\n')
        for bc in (out / 'temps').glob('*.bc'):
            subprocess.run([str(root / 'llvm-build/bin/llvm-dis'), str(bc), '-o', str(bc.with_suffix('.ll'))], check=True)
    (out / 'wall.txt').write_text(str(time.monotonic() - start) + '\n')
    return arm, level, rc

with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    results = list(pool.map(compile_one, [(a, o) for a in ['baseline', 'green'] for o in ['O0', 'O2']]))
(root / 'evidence/probe-build.json').write_text(json.dumps(results, indent=2) + '\n')
print(results)
if any(rc for _, _, rc in results):
    sys.exit(1)
# Preserve the reviewed function-level checker, changing only its input root/arms.
checker = (reference / 'check-replay.py').read_text().replace(str(reference), str(root))
checker = checker.replace("['baseline','green','producer','consumer','restored']", "['baseline','green']")
(root / 'check-replay.py').write_text(checker)
subprocess.run([sys.executable, str(root / 'check-replay.py')], check=True)
