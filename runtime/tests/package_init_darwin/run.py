#!/usr/bin/env python3
"""Native product ABI regression, with independent per-entry product mutations.

The fault arms reassemble one product TU using its compile_commands recipe and
relink the full dylib with its generated link.txt. No MCC implementation is
substituted by the test. All arms execute the same linked native caller.
"""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(sys.argv[1]).resolve()
BUILD = ROOT / 'runtime/CMakebuild'
NAMES = ('Begin', 'Complete', 'Fail', 'Abort')
OUT.mkdir(parents=True, exist_ok=True)


def run(command, log, cwd=ROOT, env=None, timeout=120):
    start = time.monotonic()
    with log.open('w') as stream:
        stream.write(shlex.join(map(str, command)) + '\n')
        stream.flush()
        p = subprocess.run(command, cwd=cwd, env=env, stdout=stream,
                           stderr=subprocess.STDOUT, timeout=timeout)
    log.with_suffix(log.suffix + '.rc').write_text(str(p.returncode) + '\n')
    print(f'{log.name}: rc={p.returncode} wall={time.monotonic()-start:.2f}', flush=True)
    return p.returncode


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


assert platform.system() == 'Darwin', 'native Darwin execution required'
run(['uptime'], OUT / 'uptime-before.txt')
entries = json.loads((BUILD / 'compile_commands.json').read_text())
compile_entry, = [e for e in entries if e['file'].endswith('/CalleeSavedStub.S')
                  and 'cangjie-runtime.dir' in e['command']]
source = Path(compile_entry['file'])
compile_cmd = shlex.split(compile_entry['command'])
object_path = (Path(compile_entry['directory']) / compile_cmd[compile_cmd.index('-o') + 1]).resolve()
link_cwd = BUILD / 'src'
link_cmd = shlex.split((link_cwd / 'CMakeFiles/cangjie-runtime.dir/link.txt').read_text())
product = (link_cwd / link_cmd[link_cmd.index('-o') + 1]).resolve()
bounds, = list(BUILD.rglob('libboundscheck.dylib'))
# Retain exact recipes and product sources, not just pass markers.
(OUT / 'compile-command.json').write_text(json.dumps(compile_entry, indent=2))
(OUT / 'link-command.txt').write_text(shlex.join(link_cmd) + '\n')
(OUT / 'source.S').write_text(source.read_text())
llvm_nm = shutil.which('llvm-nm')
if not llvm_nm:
    candidates = list(Path('/opt/homebrew/opt/llvm/bin').glob('llvm-nm')) + list(Path('/usr/local/opt/llvm/bin').glob('llvm-nm'))
    llvm_nm = str(candidates[0]) if candidates else None
assert llvm_nm, 'llvm-nm --defined-only required'
assert run([llvm_nm, '--defined-only', str(product)], OUT / 'defined.txt') == 0
symbols = {line.split()[-1] for line in (OUT / 'defined.txt').read_text().splitlines() if line.split()}
for name in NAMES:
    assert '_CJ_MCC_PackageInit' + name in symbols, name
    assert '_MCC_PackageInit' + name in symbols, name
assert '_CJ_MCC_NewObject' in symbols, 'positive symbol control'

# Native C++ consumes product headers only; all implementation comes from dylib.
includes = [ROOT / 'runtime/src', ROOT / 'runtime/src/Heap', ROOT / 'runtime/include',
            ROOT / 'runtime/src/Loader/BinaryFile',
            ROOT / 'runtime/src/CJThread/src/runtime/schedule/include',
            ROOT / 'runtime/third_party/third_party_bounds_checking_function/include']
# Installed/generated public CJThread headers are under output/**/include.
includes += list((ROOT / 'runtime/output').glob('**/include'))
caller = OUT / 'caller'
command = ['clang++', '-std=c++17', '-O0', '-g', '-fno-rtti', '-pthread']
command += ['-I' + str(p) for p in includes]
command += [str(Path(__file__).with_name('caller.cpp')), str(Path(__file__).with_name('caller.S')),
            '-L' + str(product.parent), '-L' + str(bounds.parent),
            '-Wl,-rpath,' + str(product.parent), '-lcangjie-runtime', '-lboundscheck', '-o', str(caller)]
assert run(command, OUT / 'caller-build.log') == 0
caller_sha = sha(caller)

# Each wrong routing changes one original product macro invocation only.
cuts = dict(Begin='MRT_GetThreadLocalData', Complete='MCC_PackageInitFail',
            Fail='MCC_PackageInitComplete', Abort='MRT_GetThreadLocalData')


def arm(name):
    directory = OUT / name
    directory.mkdir()
    target = directory / product.name
    shutil.copy2(bounds, directory / bounds.name)
    cmd = link_cmd.copy()
    cmd[cmd.index('-o') + 1] = str(target)
    export_cut = name.removeprefix('export-') if name.startswith('export-') else None
    if name in cuts or export_cut:
        original = source.read_text()
        if export_cut:
            needle = f'    .global _CJ_MCC_PackageInit{export_cut}\n_CJ_MCC_PackageInit{export_cut}:\n'
            replacement_text = ''
        else:
            needle = '    CalleeSavedRegistersStub MCC_PackageInit' + name
            replacement_text = '    CalleeSavedRegistersStub ' + cuts[name]
        assert original.count(needle) == 1
        changed = original.replace(needle, replacement_text)
        replacement = directory / source.name
        replacement.write_text(changed)
        import difflib
        relative = source.relative_to(ROOT)
        (directory / 'cut.diff').write_text(''.join(difflib.unified_diff(
            original.splitlines(True), changed.splitlines(True),
            fromfile='a/' + str(relative), tofile='b/' + str(relative))))
        obj = directory / 'CalleeSavedStub.S.o'
        cc = compile_cmd.copy()
        cc[cc.index('-o') + 1] = str(obj)
        cc[cc.index(str(source))] = str(replacement)
        assert run(cc, directory / 'assemble.log', Path(compile_entry['directory'])) == 0
        matched = 0
        for i, token in enumerate(cmd):
            if not token.startswith('-') and (link_cwd / token).resolve() == object_path:
                cmd[i] = str(obj)
                matched += 1
        assert matched == 1, 'must replace the actual linked product object'
    assert run(cmd, directory / 'link.log', link_cwd) == 0
    hashes = dict(caller=caller_sha, runtime=sha(target), boundscheck=sha(directory / bounds.name))
    (directory / 'sha256.json').write_text(json.dumps(hashes, indent=2))
    if export_cut:
        assert run([llvm_nm, '--defined-only', str(target)], directory / 'defined.txt') == 0
        defined = {line.split()[-1] for line in (directory / 'defined.txt').read_text().splitlines() if line.split()}
        missing = {entry for entry in NAMES if '_CJ_MCC_PackageInit' + entry not in defined}
        link_caller = command.copy()
        link_caller[link_caller.index('-L' + str(product.parent))] = '-L' + str(directory)
        link_caller[link_caller.index('-o') + 1] = str(directory / 'caller')
        rc = run(link_caller, directory / 'caller-link.log')
        diagnostic = (directory / 'caller-link.log').read_text()
        precise = missing == {export_cut} and rc != 0 and ('_CJ_MCC_PackageInit' + export_cut) in diagnostic
        return dict(arm=name, hashes=hashes, missing=sorted(missing), link_rc=rc, precise=precise,
                    scope='symbol/link negative control only; not runtime behaviour evidence')
    env = dict(os.environ, DYLD_LIBRARY_PATH=str(directory))
    results = {}
    for test in NAMES:
        log = directory / (test + '.log')
        rc = run([str(caller), test], log, env=env, timeout=30)
        output = log.read_text()
        passed = (rc == 70 and 'phase=1 result=4' in output) if test == 'Abort' else (
            rc == 0 and f'PACKAGE_INIT_TARGET {test} executed=1 pass=1' in output)
        # Abort's target assertion is the externally observed product termination.
        if test == 'Abort':
            with log.open('a') as stream:
                stream.write(f'PACKAGE_INIT_TARGET Abort executed=1 pass={int(passed)} rc={rc}\n')
        results[test] = dict(rc=rc, passed=passed)
    expected = {name} if name in cuts else set()
    failed = {test for test, result in results.items() if not result['passed']}
    return dict(arm=name, hashes=hashes, results=results, precise=(failed == expected))


# Immutable build inputs; only the selected assembly object and output differ.
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
    results = list(pool.map(arm, ['green', 'restored', *NAMES, *('export-' + name for name in NAMES)]))
(OUT / 'results.json').write_text(json.dumps(results, indent=2))
run(['uptime'], OUT / 'uptime-after.txt')
assert all(r['precise'] for r in results), 'target-only runtime assertion failures required'
assert results[0]['hashes'] == results[1]['hashes'], 'restored must match green byte for byte'
assert all(r['hashes']['runtime'] != results[0]['hashes']['runtime'] for r in results[2:])
print('Darwin PackageInit: four native entry assertions; four precise cuts; restored identity verified')
