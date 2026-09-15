#!/usr/bin/env python3
"""Build one LLVMCodeGen TU and a private llc against immutable P01 inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--receipt', required=True)
parser.add_argument('--source', required=True)
parser.add_argument('--source-sha', required=True)
parser.add_argument('--head', required=True)
parser.add_argument('--out', required=True)
parser.add_argument('--execute', action='store_true')
a = parser.parse_args()
receipt = json.loads(Path(a.receipt).read_text())
base = Path(receipt['root']).resolve()
build = base / 'llvm-build'
out = Path(a.out).resolve()
source = Path(a.source).resolve()
if out == base or base in out.parents:
    raise SystemExit('output must not be inside the preserved input')

def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

for name, info in receipt['inputs'].items():
    if digest(base / name) != info['sha256']:
        raise SystemExit('preserved input hash differs: ' + name)
if digest(source) != a.source_sha:
    raise SystemExit('candidate source hash differs')
canonical = str(base.parent / 'abi-canonical')

def words(target):
    item = receipt['commands'][target]
    if item['rc'] != 0:
        raise SystemExit('recipe extraction failed: ' + target)
    return [word.replace(canonical, str(base)) for word in shlex.split(item['stdout'])]

obj = out / 'CJBarrierLowering.cpp.o'
archive = out / 'libLLVMCodeGen.a'
exe = out / 'llc'
compile_cmd = words('lib/CodeGen/CMakeFiles/LLVMCodeGen.dir/CJBarrierLowering.cpp.o')
for flag, value in [('-o', obj), ('-MF', out / 'CJBarrierLowering.cpp.o.d'), ('-c', source)]:
    compile_cmd[compile_cmd.index(flag) + 1] = str(value)
compile_cmd[compile_cmd.index('-c'):compile_cmd.index('-c')] = [
    '-f' + kind + '-prefix-map=' + str(source.parent) + '=/usr/src/cangjie-llvm/llvm/lib/CodeGen'
    for kind in ('file', 'debug', 'macro')]
link_cmd = words('bin/llc')
if link_cmd[:2] != [':', '&&'] or link_cmd[-2:] != ['&&', ':']:
    raise SystemExit('unexpected link command wrapper')
link_cmd = link_cmd[2:-2]
link_cmd[link_cmd.index('-o') + 1] = str(exe)
link_cmd = [str(archive) if word == 'lib/libLLVMCodeGen.a' else word for word in link_cmd]
# Ninja's original recipe escaped this for a shell; subprocess passes argv directly.
link_cmd = [word.replace('\\', '') if word.startswith('-Wl,-rpath,') else word for word in link_cmd]
inputs = {}
for word in link_cmd[1:]:
    if word.startswith('-') or word in (str(archive), str(exe)):
        continue
    if word.endswith(('.a', '.o', '.so')):
        path = Path(word) if Path(word).is_absolute() else build / word
        inputs[str(path)] = {'size': path.stat().st_size, 'sha256': digest(path)}
plan = {'source_head': a.head, 'source_sha256': a.source_sha, 'preserved_source_head': receipt['source_sha'],
        'cwd': str(build), 'compile': compile_cmd, 'link': link_cmd, 'link_inputs': inputs,
        'space_budget_bytes': 256 * 1024 * 1024, 'baseline': receipt['inputs']}
print(json.dumps(plan, indent=2), flush=True)
if not a.execute:
    raise SystemExit(0)
if out.exists() and any(out.iterdir()):
    raise SystemExit('use a new output directory to preserve previous evidence')
if shutil.disk_usage(out.parent).free < plan['space_budget_bytes']:
    raise SystemExit('insufficient capacity for the bounded build')
out.mkdir(parents=True, exist_ok=True)
(out / 'plan.json').write_text(json.dumps(plan, indent=2) + '\n')
env = dict(os.environ, CCACHE_DIR='/root/.ccache', CCACHE_BASEDIR=str(source.parent), CCACHE_NOHASHDIR='1')
results = {}

def run(name, cmd):
    start = time.monotonic()
    with (out / (name + '.log')).open('w') as log:
        p = subprocess.run(cmd, cwd=build, env=env, stdout=log, stderr=subprocess.STDOUT)
    results[name] = {'argv': cmd, 'rc': p.returncode, 'wall': time.monotonic() - start}
    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    if p.returncode:
        raise SystemExit(p.returncode)

run('compile', compile_cmd)
results['object_sha256'] = digest(obj)
shutil.copyfile(base / 'llvm-build/lib/libLLVMCodeGen.a', archive)
member = obj.name
members = subprocess.run(['ar', 't', str(archive)], capture_output=True, text=True, check=True).stdout.splitlines()
if members.count(member) != 1:
    raise SystemExit('expected exactly one existing codegen TU member')
run('archive', ['ar', 'rcs', str(archive), str(obj)])
results['archive_sha256'] = digest(archive)
run('link', link_cmd)
results['llc_sha256'] = digest(exe)
for name, info in receipt['inputs'].items():
    if digest(base / name) != info['sha256']:
        raise SystemExit('preserved input changed: ' + name)
(out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
print('LLVM_INCREMENTAL_LINKED ' + str(exe) + ' sha256=' + results['llc_sha256'])
