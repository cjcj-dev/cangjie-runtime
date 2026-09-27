#!/usr/bin/env python3
"""Build actual runtime products and run the same register-test executable against each arm."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

SOURCE = Path(__file__).resolve().parents[2]
TARGET = sys.argv[1]
OUT = Path(os.environ['RETURN_STUB_OUT']).resolve()
OUT.mkdir(parents=True, exist_ok=True)
APPLE = platform.system() == 'Darwin'
CROSS = TARGET.startswith('ios')
CPU = 'aarch64' if ('aarch64' in TARGET or (TARGET == 'native' and platform.machine() == 'arm64')) else 'x86_64'
ARCH = CPU + ('_ios' if CROSS else '_macos' if APPLE else '_windows')
JOBS = str(os.cpu_count() or 1)

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run(cmd, log, cwd=None, env=None):
    start = time.monotonic()
    with log.open('w') as output:
        output.write('COMMAND ' + repr([str(x) for x in cmd]) + '\n'); output.flush()
        proc = subprocess.run([str(x) for x in cmd], cwd=cwd, env=env, stdout=output, stderr=subprocess.STDOUT)
    print(f'{log.name}: rc={proc.returncode} wall={time.monotonic()-start:.1f}', flush=True)
    return proc.returncode

def identity(lib, arm):
    nm = shutil.which('llvm-nm') or shutil.which('nm')
    cmd = [nm, '--defined-only', lib]
    # Apple nm spells --defined-only as -U and retains local symbols.
    if APPLE and Path(nm).name == 'nm': cmd = [nm, '-U', lib]
    rc = run(cmd, arm/'defined.log')
    if rc: return rc
    symbols = {line.split()[-1] for line in (arm/'defined.log').read_text().splitlines()[1:] if line.split()}
    prefix = '_' if APPLE else ''
    checks = {name: prefix+name in symbols for name in ['CJ_MCC_HandleSafepoint', 'CJ_MCC_HandleReturnSafepoint']}
    (arm/'identity.json').write_text(json.dumps({'library':str(lib),'sha256':digest(lib),'checks':checks}, indent=2))
    for name, good in checks.items(): print(f'IDENTITY_ASSERT {ARCH} {name} {"PASS" if good else "FAIL"}', flush=True)
    run(['file',lib], arm/'file.log')
    return int(not all(checks.values()))

def build(kind):
    arm = OUT/kind; arm.mkdir(exist_ok=True)
    tree = arm/'runtime'
    shutil.copytree(SOURCE,tree,ignore=shutil.ignore_patterns('CMakebuild','output','build-*','__pycache__'),dirs_exist_ok=True)
    stub = tree/'src/arch'/ARCH/'HandleReturnSafepointStub.S'
    original = stub.read_text()
    text = original
    if kind == 'cut-wiring':
        cmake = tree/'src/CMakeLists.txt'
        before = cmake.read_text()
        line = '"arch/'+ARCH+'/HandleReturnSafepointStub.S"\n'
        after = before.replace(line,'',1)
        if after == before: raise RuntimeError('knife did not match')
        import difflib
        (arm/'cut.diff').write_text(''.join(difflib.unified_diff(before.splitlines(True),after.splitlines(True),fromfile='a/runtime/src/CMakeLists.txt',tofile='b/runtime/src/CMakeLists.txt')))
        cmake.write_text(after)
    if kind == 'cut-save':
        old,new = ('stp  x0, x1,','stp  x2, x1,') if CPU=='aarch64' else (('movq    %rax, -8(%rbp)','movq    %rcx, -8(%rbp)') if not APPLE else ('pushq  %rax','pushq  %rcx'))
        text = text.replace(old,new,1)
    elif kind == 'cut-restore':
        if CPU=='aarch64': text=text.replace('ldp  x0, x1,','ldp  x9, x1,',1)
        elif APPLE:
            index=text.rfind('popq  %rax'); text=text[:index]+text[index:].replace('popq  %rax','popq  %rcx',1)
        else: text=text.replace('movq    -8(%rbp), %rax','movq    -8(%rbp), %rcx',1)
    if kind.startswith('cut') and kind != 'cut-wiring':
        if text == original: raise RuntimeError('knife did not match')
        import difflib
        (arm/'cut.diff').write_text(''.join(difflib.unified_diff(original.splitlines(True),text.splitlines(True),fromfile='a/runtime/src/arch/'+ARCH+'/HandleReturnSafepointStub.S',tofile='b/runtime/src/arch/'+ARCH+'/HandleReturnSafepointStub.S')))
        stub.write_text(text)
    env=os.environ.copy()
    env.update(CANGJIE_BUILD_JOBS=JOBS,CMAKE_BUILD_PARALLEL_LEVEL=JOBS,GC_UNIT_GATE_SKIP='1',CCACHE_BASEDIR=str(tree),CCACHE_NOHASHDIR='1')
    maps=f'-ffile-prefix-map={tree}=/usr/src/cangjie-runtime -fdebug-prefix-map={tree}=/usr/src/cangjie-runtime -fmacro-prefix-map={tree}=/usr/src/cangjie-runtime'
    for flag in ('CFLAGS','CXXFLAGS','ASMFLAGS'): env[flag]=maps
    if APPLE:
        cmd=[sys.executable,'build.py','build','--target',TARGET,'--build-type','release','--prefix',str(arm/'install')]
        if CROSS:
            sdk='iphoneos' if TARGET=='ios-aarch64' else 'iphonesimulator'
            sdkroot=subprocess.check_output(['xcrun','--sdk',sdk,'--show-sdk-path'],text=True).strip()
            clang=subprocess.check_output(['xcrun','--sdk',sdk,'--find','clang'],text=True).strip()
            cmd+=['--target-toolchain',str(Path(clang).parent.parent),'--target-sysroot',sdkroot]
        rc=run(cmd,arm/'build.log',tree,env)
    else:
        cmd=['cmake','-S',tree,'-B',tree/'CMakebuild','-G','Ninja','-DWINDOWS_FLAG=1','-DCOPYGC_FLAG=1','-DDOPRA_FLAG=1','-DCMAKE_BUILD_TYPE=Release','-DRUNTIME_TRACE_FLAG=1','-DCJ_SDK_VERSION=0.0.1','-DDISABLE_VERSION_CHECK=1','-DCMAKE_C_COMPILER=clang','-DCMAKE_CXX_COMPILER=clang++','-DCMAKE_AR_PATH=llvm-ar','-DCMAKE_INSTALL_PREFIX='+str(arm/'install')]
        rc=run(cmd,arm/'configure.log',tree,env)
        if not rc: rc=run(['cmake','--build',tree/'CMakebuild','--parallel',JOBS],arm/'build.log',tree,env)
    (arm/'build.rc').write_text(str(rc))
    if rc: return {'kind':kind,'build_rc':rc}
    leaf='libcangjie-runtime.dylib' if APPLE else 'libcangjie-runtime.dll'
    libs=[p for p in (tree/'output').rglob(leaf) if p.is_file()]
    if not libs and not APPLE: libs=[p for p in tree.rglob('*cangjie-runtime.dll') if p.is_file()]
    if not libs: raise RuntimeError('product library missing')
    lib=sorted(libs)[0]
    identity_rc=identity(lib,arm)
    pair=arm/'product'; pair.mkdir(exist_ok=True)
    for f in lib.parent.iterdir():
        if f.is_file() and f.suffix in ['.dylib','.dll','.a']: shutil.copy2(f,pair/f.name)
    includes=list((tree/'output').glob('temp/*/include'))
    result={'kind':kind,'build_rc':rc,'identity_rc':identity_rc,'library':str(pair/lib.name),'sha256':digest(pair/lib.name),'tree':str(tree),'headers':str(includes[0]) if includes else ''}
    (arm/'product.json').write_text(json.dumps(result,indent=2))
    return result

arms=['green','cut-wiring'] if CROSS else ['green','cut-save','cut-restore','cut-wiring']
with concurrent.futures.ThreadPoolExecutor(max_workers=len(arms)) as pool:
    results=list(pool.map(build,arms))
# Removing the CMake wiring breaks the product on its real failure path: the stub
# also defines the unwind metadata symbol consumed by MachineFrame.cpp, so the
# library either fails to link with undefined return-safepoint symbols, or links
# and then fails the nm identity gate on precisely CJ_MCC_HandleReturnSafepoint.
# The plain CJ_MCC_HandleSafepoint symbol is the positive control that a healthy
# library was read. Either precise mode is platform-attributed; a generic build
# error is not.
def wiring_precise(r):
    kind=r['kind']
    if not r.get('build_rc'):
        checks=json.loads((OUT/kind/'identity.json').read_text())['checks']
        ok = r.get('identity_rc')==1 and checks=={'CJ_MCC_HandleSafepoint':True,'CJ_MCC_HandleReturnSafepoint':False}
        return ok, f'identity_rc={r.get("identity_rc")} checks={checks}'
    log=(OUT/kind/'build.log').read_text()
    import re
    undefined=re.findall(r'"(_?[A-Za-z]\w*)", referenced from',log)
    ret=[s for s in undefined if 'ReturnSafepoint' in s]
    ok = 'Undefined symbols' in log and len(undefined)>0 and undefined==ret
    return ok, f'build_rc={r.get("build_rc")} undefined={undefined}'
matrix_ok=True
for r in results:
    kind=r['kind']
    if kind=='cut-wiring':
        ok, detail = wiring_precise(r)
        r['wiring_expect_ok']=ok
        r['wiring_detail']=detail
        print(f'WIRING_ASSERT {ARCH} {detail} {"PASS" if ok else "FAIL"}',flush=True)
    elif r.get('build_rc'):
        r['wiring_expect_ok']=False
    else:
        checks=json.loads((OUT/kind/'identity.json').read_text())['checks']
        r['wiring_expect_ok']=r.get('identity_rc')==0 and all(checks.values())
    matrix_ok &= r['wiring_expect_ok']
(OUT/'build-results.json').write_text(json.dumps(results,indent=2))
if not matrix_ok: sys.exit(2)
if CROSS: sys.exit(0)
green=results[0]; tree=Path(green['tree']); pair=Path(green['library']).parent
exe=OUT/('registers.exe' if not APPLE else 'registers')
incs=['src','src/Loader/BinaryFile','src/Heap','src/Heap/z/os/'+('bsd' if APPLE else 'windows'),'src/CJThread/src/runtime/schedule/include','include','third_party/third_party_bounds_checking_function/include','src/os/Windows']
cmd=['clang++','-std=gnu++17','-O0','-g','-pthread','-fno-rtti','-fno-omit-frame-pointer']+['-I'+str(tree/p) for p in incs]
if green['headers']: cmd+=['-I'+green['headers']]
cmd += [SOURCE/'tests/return_safepoint/registers.cpp',SOURCE/('tests/return_safepoint/registers_'+CPU+'.S'),'-L'+str(pair),'-lcangjie-runtime','-lboundscheck','-o',exe]
if APPLE: cmd+=['-Wl,-rpath,'+str(pair)]
rc=run(cmd,OUT/'test-build.log')
if rc: sys.exit(rc)
(OUT/'test.sha256').write_text(digest(exe)+'\n')
run_results=[]
# The restored arm reuses the retained, byte-identical green product, not a later rebuild.
for r in [r for r in results if r['kind']!='cut-wiring']+[dict(green,kind='restored')]:
    arm=OUT/r['kind']; arm.mkdir(exist_ok=True)
    product=Path(r['library']).parent
    localexe=product/exe.name; shutil.copy2(exe,localexe)
    env=os.environ.copy(); env['DYLD_LIBRARY_PATH']=str(product); env['PATH']=str(product)+os.pathsep+env['PATH']
    rc=run([localexe],arm/'registers.log',env=env)
    lines=(arm/'registers.log').read_text().splitlines()
    failures=[line for line in lines if line.startswith('REGISTER_ASSERT ') and line.endswith('FAIL')]
    expected=1 if r['kind'].startswith('cut') else 0
    valid=rc==expected and len(failures)==expected and any(line.startswith('REGISTER_RESULT ') for line in lines)
    # A save/restore knife must affect precisely the first integer return register.
    if failures: valid &= failures[0].startswith('REGISTER_ASSERT '+('x0' if CPU=='aarch64' else 'rax')+' ')
    row={'arm':r['kind'],'rc':rc,'failures':failures,'valid':valid,'test_sha256':digest(localexe),'product_sha256':digest(Path(r['library']))}
    run_results.append(row)
(OUT/'run-results.json').write_text(json.dumps(run_results,indent=2))
sys.exit(0 if all(r['valid'] for r in run_results) else 1)
