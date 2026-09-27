#!/usr/bin/env python3
"""Native real-llc -> return stub -> Handle root -> returned-reference experiment."""
import base64
import concurrent.futures
import difflib
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
OUT = Path(os.environ['RETURN_PAIR_OUT']).resolve()
SOURCE = Path(sys.argv[1]).resolve() / 'runtime'
APPLE = platform.system() == 'Darwin'
CPU = 'aarch64' if platform.machine() in ('arm64', 'aarch64') else 'x86_64'
TARGET = CPU + ('-macos' if APPLE else '-windows')
JOBS = str(os.cpu_count() or 1)
# The checkout uses Git for Windows with CRLF conversion; MSYS Git has its
# own global configuration. Compare with the same checkout policy on both reads.
GIT = ['git'] if APPLE else ['git', '-c', 'core.autocrlf=true']
OUT.mkdir(parents=True, exist_ok=True)


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def run(cmd, log, cwd=None, env=None, timeout=3600):
    start = time.monotonic()
    with log.open('w') as output:
        output.write('COMMAND '+repr(list(map(str, cmd)))+'\n'); output.flush()
        try:
            rc = subprocess.run(list(map(str, cmd)), cwd=cwd, env=env,
                                stdout=output, stderr=subprocess.STDOUT, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            rc = 124
        except FileNotFoundError as error:
            output.write(str(error)+'\n')
            rc = 127
    record = {'rc': rc, 'wall': time.monotonic()-start, 'log': str(log)}
    log.with_suffix('.json').write_text(json.dumps(record))
    print(f'{log}: rc={rc} wall={record["wall"]:.1f}', flush=True)
    return rc


manifest = json.loads((HERE/'objects.json').read_text())
if manifest['ir_sha256'] != sha(HERE/'pair.ll'):
    raise RuntimeError('IR differs from the actual llc input')
objects = {}
for row in manifest['objects']:
    if row['target'] != TARGET:
        continue
    obj = OUT/(row['arm']+'.o')
    obj.write_bytes(base64.b64decode(row['base64'], validate=True))
    if sha(obj) != row['sha256'] or row['rc'] != 0:
        raise RuntimeError('LLVM object identity mismatch')
    objects[row['arm']] = obj
if set(objects) != {'candidate', 'cut-producer'}:
    raise RuntimeError('target objects missing')
(OUT/'producer-identity.json').write_text(json.dumps(manifest, indent=2))
if run(GIT + ['-C', SOURCE.parent, 'rev-parse', 'HEAD'], OUT/'runtime-head.log') != 0:
    raise RuntimeError('runtime source identity command did not execute')
if run(GIT + ['-C', SOURCE.parent, 'diff', '--exit-code'], OUT/'runtime-source-before.log') != 0:
    raise RuntimeError('runtime checkout differs from its declared source before the experiment')
run(['uptime'], OUT/'uptime-before.log')
source_consumer = SOURCE/'src/Mutator/MutatorManager.cpp'
source_before = sha(source_consumer)


def build(arm):
    home = OUT/arm; home.mkdir()
    tree = home/'runtime'
    shutil.copytree(SOURCE, tree, ignore=shutil.ignore_patterns('output', 'CMakebuild', '__pycache__'))
    consumer = tree/'src/Mutator/MutatorManager.cpp'
    before = consumer.read_text()
    if arm == 'cut-consumer':
        call = '        StackFrameCursor::CollectReturnRegisterRoots(mutator->GetUnwindContext().frameInfo, named);'
        if before.count(call) != 1:
            raise RuntimeError('consumer knife no longer matches')
        after = before.replace(call, '        (void)named; // controlled consumer disconnection')
        consumer.write_text(after)
        (home/'cut.diff').write_text(''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
            fromfile='a/runtime/src/Mutator/MutatorManager.cpp', tofile='b/runtime/src/Mutator/MutatorManager.cpp')))
    env = os.environ.copy()
    env.update(CANGJIE_BUILD_JOBS=JOBS, CMAKE_BUILD_PARALLEL_LEVEL=JOBS, GC_UNIT_GATE_SKIP='1')
    if APPLE:
        command = [sys.executable, 'build.py', 'build', '--target', 'native', '--build-type', 'release', '--prefix', home/'install']
        rc = run(command, home/'build.log', tree, env)
    else:
        command = ['cmake', '-S', tree, '-B', tree/'CMakebuild', '-G', 'Ninja', '-DWINDOWS_FLAG=1',
                   '-DCOPYGC_FLAG=1', '-DDOPRA_FLAG=1', '-DCMAKE_BUILD_TYPE=Release', '-DRUNTIME_TRACE_FLAG=1',
                   '-DCJ_SDK_VERSION=0.0.1', '-DDISABLE_VERSION_CHECK=1', '-DCMAKE_C_COMPILER=clang',
                   '-DCMAKE_CXX_COMPILER=clang++', '-DCMAKE_AR_PATH=llvm-ar', '-DCMAKE_INSTALL_PREFIX='+str(home/'install')]
        rc = run(command, home/'configure.log', tree, env)
        if rc == 0:
            rc = run(['cmake', '--build', tree/'CMakebuild', '--parallel', JOBS], home/'build.log', tree, env)
    row = {'arm': arm, 'build_rc': rc, 'jobs': JOBS, 'tree': str(tree)}
    raw_exports = tree/'CMakebuild/windows_x86_64_exports.raw.def'
    if raw_exports.is_file():
        shutil.copy2(raw_exports, home/raw_exports.name)
        row['raw_exports_sha256'] = sha(raw_exports)
    if rc == 0:
        leaf = 'libcangjie-runtime.dylib' if APPLE else '*cangjie-runtime.dll'
        libs = sorted((tree/'output').rglob(leaf)) if APPLE else sorted(tree.rglob(leaf))
        lib = libs[0]
        product = home/'product'; product.mkdir()
        for f in lib.parent.iterdir():
            if f.is_file() and f.suffix in ('.dylib', '.dll', '.a'):
                shutil.copy2(f, product/f.name)
        headers = list((tree/'output').glob('temp/*/include'))
        row.update(library=str(product/lib.name), sha256=sha(product/lib.name),
                   headers=str(headers[0]) if headers else '')
        nm = shutil.which('llvm-nm') or shutil.which('nm')
        run([nm, '-U' if APPLE and Path(nm).name == 'nm' else '--defined-only', product/lib.name], home/'symbols.log')
        run(['file', product/lib.name], home/'file.log')
    (home/'product.json').write_text(json.dumps(row, indent=2))
    return row


with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
    arms = list(pool.map(build, ['candidate', 'cut-consumer']))
(OUT/'build-results.json').write_text(json.dumps(arms, indent=2))
if any(a['build_rc'] != 0 for a in arms):
    sys.exit(2)
green, cut = arms
tree = Path(green['tree']); product = Path(green['library']).parent
incs = ['src', 'src/Loader/BinaryFile', 'src/Heap', 'src/Heap/z/os/'+('bsd' if APPLE else 'windows'),
        'src/CJThread/src/runtime/schedule/include', 'include',
        'third_party/third_party_bounds_checking_function/include', 'src/os/Windows']
common = ['clang++', '-std=gnu++17', '-O0', '-g', '-pthread', '-fno-rtti', '-fno-omit-frame-pointer',
          '-fvisibility-inlines-hidden'] + ['-I'+str(tree/i) for i in incs]
if green['headers']:
    common += ['-I'+green['headers']]
common += [HERE/'pair.cpp', HERE/'bridge.S', '-L'+str(product), '-lcangjie-runtime', '-lboundscheck']
if APPLE:
    common += ['-Wl,-rpath,'+str(product)]
executables = {}
for kind, obj in objects.items():
    exe = OUT/(kind+('-pair' if APPLE else '-pair.exe'))
    if run(common+[obj, '-o', exe], OUT/(kind+'-link.log')) != 0:
        sys.exit(3)
    executables[kind] = exe
def execute(spec):
    name, library, exe = spec
    destination = Path(library['library']).parent
    rundir = OUT/(name+"-run"); rundir.mkdir()
    local = rundir/exe.name
    shutil.copy2(exe, local)
    env = os.environ.copy()
    env['DYLD_LIBRARY_PATH'] = str(destination)
    env['PATH'] = str(destination)+os.pathsep+env['PATH']
    rc = run([local], OUT/(name+'-run.log'), env=env, timeout=30)
    if APPLE and name == 'candidate' and rc < 0:
        run(['lldb', '--batch', '-o', 'settings set target.disable-aslr false',
             '-o', 'run', '-k', 'thread backtrace all', '-k', 'register read',
             '-k', 'disassemble --frame', '--', local], OUT/'candidate-debug.log', env=env, timeout=120)
    text = (OUT/(name+'-run.log')).read_text()
    expected = 1 if name.startswith('cut') else 0
    target = [x for x in text.splitlines() if x.startswith('PAIR_RETURN_TARGET ')]
    valid = rc == expected and len(target) == 1 and ('pass='+str(1-expected)) in target[0]
    return {'arm': name, 'rc': rc, 'valid': valid, 'target': target,
                    'exe_sha256': sha(local), 'library_sha256': sha(Path(library['library']))}

with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    results = list(pool.map(execute, [('candidate', green, executables['candidate']),
                           ('cut-producer', green, executables['cut-producer']),
                           ('cut-consumer', cut, executables['candidate']),
                           ('restored', green, executables['candidate'])]))
(OUT/'run-results.json').write_text(json.dumps(results, indent=2))
unchanged = source_before == sha(source_consumer)
source_rc = run(GIT + ['-C', SOURCE.parent, 'diff', '--exit-code'], OUT/'runtime-source-unchanged.log')
(OUT/'runtime-source-identity.json').write_text(json.dumps({'before': source_before, 'after': sha(source_consumer), 'diff_rc': source_rc}))
run(['uptime'], OUT/'uptime-after.log')
run(['sccache', '--show-stats'], OUT/'sccache.log')
sys.exit(0 if unchanged and source_rc == 0 and all(r['valid'] for r in results) else 1)
