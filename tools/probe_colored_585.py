#!/usr/bin/env python3
"""Compile the same source through the old and rebuilt complete SDKs at O0/O2."""
import concurrent.futures
import hashlib
import json
import os
import re
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

if '--check-only' not in sys.argv:
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        arms = ['green'] if '--green-only' in sys.argv else ['baseline', 'green']
        results = list(pool.map(compile_one, [(a, o) for a in arms for o in ['O0', 'O2']]))
    (root / 'evidence/probe-build.json').write_text(json.dumps(results, indent=2) + '\n')
    print(results)
    if any(rc for _, _, rc in results):
        sys.exit(1)
# Preserve the reviewed function-level checker, changing only its input root/arms.
checker = (reference / 'check-replay.py').read_text().replace(str(reference), str(root))
checker = checker.replace("['baseline','green','producer','consumer','restored']", "['baseline','green']")
(root / 'check-replay.py').write_text(checker)
subprocess.run([sys.executable, str(root / 'check-replay.py')], check=True)
checks = json.loads((root / 'replay-checks.json').read_text())
for result in checks:
    directory = root / result['arm'] / 'replay' / result['level']
    assemblies = {str(p): p.read_text() for p in (directory / 'temps').glob('*.s')}
    asm_rows = []
    for row in result['static']:
        name = re.escape(row['function'])
        bodies = [(path, match.group(1)) for path, text in assemblies.items()
                  for match in re.finditer(r'^"?' + name + r'"?:\s*\n(.*?)^\.Lfunc_end\d+:', text, re.M | re.S)]
        calls = sum(len(re.findall(r'\b(?:callq?|jmpq?)\s+CJ_MCC_WriteStaticRef\b', body)) for _, body in bodies)
        guards = [line for _, body in bodies for line in body.splitlines() if re.search(r'\bcmp[lq]?\s+\$(?:8|9|0x8|0x9),', line)]
        asm_rows.append(dict(function=row['function'], files=[p for p, _ in bodies], runtime_calls=calls,
                             phase_compares=guards, passed=len(bodies) == 1 and calls == row['expected'] and not guards))
    (directory / 'assembly-checks.json').write_text(json.dumps(asm_rows, indent=2) + '\n')
    expected_static = result['arm'] == 'green'
    assembly_pass = bool(asm_rows) and all(row['passed'] for row in asm_rows)
    passed = result['static_pass'] == expected_static and result['atomic_pass'] and assembly_pass == expected_static
    print('ASSERT_REACHED', result['arm'], result['level'], 'static_route', passed)
    if not passed:
        sys.exit(2)
