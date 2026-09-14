#!/usr/bin/env python3
"""Install this run's runtime pair and record every compiler/std/runtime identity."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
sdk = root / 'gate-sdk'
files = []

def record(path, component):
    files.append(dict(component=component, path=str(path), realpath=str(path.resolve()),
                      sha256=hashlib.sha256(path.read_bytes()).hexdigest()))

for arm in ['default', 'testable']:
    for name in ['libcangjie-runtime.so', 'libboundscheck.so']:
        src = root / arm / 'build/runtime-staging/lib/x86_64_Release' / name
        record(src, arm + '/' + name)
        if arm == 'testable':
            dst = sdk / 'runtime/lib/linux_x86_64_cjnative' / name
            shutil.copy2(src, dst)
            record(dst, 'sdk/' + name)
for name, path in [('compiler', sdk / 'bin/cjc'), ('llc', sdk / 'third_party/llvm/bin/llc'),
                   ('opt', sdk / 'third_party/llvm/bin/opt')]:
    record(path, name)
for package in 'binary collection convert core env fs io math process runtime sync time'.split():
    for folder, name in [('lib', f'libcangjie-std-{package}.a'),
                         ('runtime/lib', f'libcangjie-std-{package}.so'),
                         ('modules', f'std/std.{package}.cjo')]:
        record(sdk / folder / 'linux_x86_64_cjnative' / name, 'std/' + package)
for p in sorted((root / 'host').rglob('*.so*')):
    if p.is_file():
        record(p, 'compiler-host')
stamps = {}
for arm in ['default', 'testable']:
    p = root / arm / 'build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so'
    lines = subprocess.check_output(['strings', str(p)], text=True).splitlines()
    stamps[arm] = [line for line in lines if line.startswith(('CJRT-COMMIT:', 'CJRT-DECLARED:'))]
compiler_lines = subprocess.check_output(['strings', str(sdk / 'bin/cjc')], text=True).splitlines()
identity = dict(llvm_sha='23e45a2e9dfbfc4a708d99bc7895fbc1d8ffcbaf',
                runtime_sha='fca1c8ce77f139b14cef9117c1246fe01b0aa084',
                frontend_sha='52cb48f5524f67f4df79761bac863441d2b72c79',
                compiler_stamps=[s for s in compiler_lines if s.startswith('CJCJ-COMMIT:')],
                runtime_stamps=stamps, files=files)
(root / 'evidence/identity.json').write_text(json.dumps(identity, indent=2) + '\n')
print('identity files=' + str(len(files)))
