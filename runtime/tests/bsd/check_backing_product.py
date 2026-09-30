#!/usr/bin/env python3
"""Exercise the native Darwin product dylib and independently relink fault arms."""
import concurrent.futures
import difflib
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
CASES = ('ctor_ok', 'ctor_fail', 'commit_ok', 'commit_fail',
         'uncommit_ok', 'uncommit_fail')
OUT.mkdir(parents=True, exist_ok=True)


def run(command, log, cwd=ROOT, env=None):
    start = time.monotonic()
    with log.open('w') as stream:
        stream.write(shlex.join(map(str, command)) + '\n')
        stream.flush()
        result = subprocess.run(command, cwd=cwd, env=env, stdout=stream,
                                stderr=subprocess.STDOUT, timeout=180)
    log.with_suffix(log.suffix + '.rc').write_text(str(result.returncode) + '\n')
    print(f'{log.name}: rc={result.returncode} wall={time.monotonic() - start:.2f}', flush=True)
    return result.returncode


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


assert platform.system() == 'Darwin', 'native Darwin execution required'
run(['uptime'], OUT / 'uptime-before.txt')
entries = json.loads((BUILD / 'compile_commands.json').read_text())
recipes = {}
for name in ('zPhysicalMemoryBacking_bsd.cpp', 'SysCall.cpp'):
    recipe, = [entry for entry in entries if entry['file'].endswith('/' + name)
               and 'cangjie-runtime.dir' in entry['command']]
    recipes[name] = recipe
(OUT / 'compile-commands.json').write_text(json.dumps(recipes, indent=2))
link_cwd = BUILD / 'src'
link_command = shlex.split((link_cwd / 'CMakeFiles/cangjie-runtime.dir/link.txt').read_text())
product = (link_cwd / link_command[link_command.index('-o') + 1]).resolve()
bounds, = list(BUILD.rglob('libboundscheck.dylib'))
(OUT / 'link-command.txt').write_text(shlex.join(link_command) + '\n')
reader = shutil.which('llvm-nm')
assert reader, 'llvm-nm --defined-only required'
assert run([reader, '--defined-only', str(product)], OUT / 'defined.txt') == 0
defined = (OUT / 'defined.txt').read_text()
assert 'ZPhysicalMemoryBacking' in defined
assert 'ReserveMemory' in defined
compile_command = shlex.split(recipes['zPhysicalMemoryBacking_bsd.cpp']['command'])
caller = OUT / 'caller'
command = ['clang++', '-std=c++14', '-pthread']
command += [token for token in compile_command if token.startswith(('-I', '-D'))]
command += [str(Path(__file__).with_name('backing_fail_main.cpp')),
            '-L' + str(product.parent), '-L' + str(bounds.parent),
            '-Wl,-rpath,' + str(product.parent), '-lcangjie-runtime', '-lboundscheck',
            '-o', str(caller)]
assert run(command, OUT / 'caller-build.log') == 0
caller_sha = sha(caller)
CUTS = {
    'reserve_normalize': ('SysCall.cpp', '        return nullptr;', '        return address;', 'ctor_fail'),
    'reserve_consume': ('zPhysicalMemoryBacking_bsd.cpp', '  if (_base == 0) {',
                        '  if (_base == static_cast<uintptr_t>(-1)) {', 'ctor_fail'),
    'commit_consume': ('zPhysicalMemoryBacking_bsd.cpp', '  if (res == MAP_FAILED) {',
                       '  if (res == nullptr) {', 'commit_fail'),
    'uncommit_consume': ('zPhysicalMemoryBacking_bsd.cpp', '    return 0;',
                         '    return length;', 'uncommit_fail'),
}


def arm(name):
    directory = OUT / name
    directory.mkdir()
    target = directory / product.name
    shutil.copy2(bounds, directory / bounds.name)
    link = link_command.copy()
    link[link.index('-o') + 1] = str(target)
    if name in CUTS:
        filename, old, new, expected = CUTS[name]
        recipe = recipes[filename]
        source = Path(recipe['file'])
        original = source.read_text()
        if name == 'commit_consume':
            prefix, body = original.split('bool ZPhysicalMemoryBacking::commit_inner', 1)
            assert old in body
            changed = prefix + 'bool ZPhysicalMemoryBacking::commit_inner' + body.replace(old, new, 1)
        else:
            assert original.count(old) == 1, (name, original.count(old))
            changed = original.replace(old, new)
        replacement = directory / filename
        replacement.write_text(changed)
        relative = source.relative_to(ROOT)
        (directory / 'cut.diff').write_text(''.join(difflib.unified_diff(
            original.splitlines(True), changed.splitlines(True),
            fromfile='a/' + str(relative), tofile='b/' + str(relative))))
        compile_cut = shlex.split(recipe['command'])
        object_path = (Path(recipe['directory']) / compile_cut[compile_cut.index('-o') + 1]).resolve()
        cut_object = directory / (filename + '.o')
        compile_cut[compile_cut.index('-o') + 1] = str(cut_object)
        compile_cut[compile_cut.index(str(source))] = str(replacement)
        assert run(compile_cut, directory / 'compile.log', Path(recipe['directory'])) == 0
        matched = 0
        for index, token in enumerate(link):
            if not token.startswith('-') and (link_cwd / token).resolve() == object_path:
                link[index] = str(cut_object)
                matched += 1
        assert matched == 1, 'replace exactly the actual linked product object'
        assert run(link, directory / 'link.log', link_cwd) == 0
    else:
        shutil.copy2(product, target)
    hashes = dict(caller=caller_sha, runtime=sha(target), boundscheck=sha(directory / bounds.name))
    (directory / 'sha256.json').write_text(json.dumps(hashes, indent=2))
    environment = dict(os.environ, DYLD_LIBRARY_PATH=str(directory), DYLD_PRINT_LIBRARIES='1')
    results = {}
    for case in CASES:
        log = directory / (case + '.log')
        rc = run([str(caller), case], log, env=environment)
        text = log.read_text()
        observed = ('CASE ' + case + ' ') in text if case.startswith('ctor_') else ('ASSERT ' + case + ' ') in text
        if case == 'commit_fail' and name != 'commit_consume':
            observed = observed and 'Failed to commit memory (' in text
        if case == 'uncommit_fail':
            observed = observed and 'Failed to uncommit memory (' in text
        assert str(target) in text, 'dyld must load this arm product'
        results[case] = dict(rc=rc, observed=observed)
    return dict(arm=name, hashes=hashes, results=results)


with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
    records = list(pool.map(arm, ('green', 'restored', *CUTS)))
(OUT / 'results.json').write_text(json.dumps(records, indent=2))
by_name = {record['arm']: record for record in records}
assert by_name['green']['hashes'] == by_name['restored']['hashes']
for record in records:
    expected_red = {CUTS[record['arm']][3]} if record['arm'] in CUTS else set()
    actual_red = {case for case, result in record['results'].items() if result['rc'] != 0}
    assert actual_red == expected_red, (record['arm'], actual_red, expected_red)
    assert all(result['observed'] for result in record['results'].values()), record['arm']
    if expected_red:
        assert record['hashes']['runtime'] != by_name['green']['hashes']['runtime']
        assert record['results'][next(iter(expected_red))]['rc'] == 1
    print(f"PRODUCT_BACKING arm={record['arm']} red={sorted(actual_red)} hashes={record['hashes']}")
run(['uptime'], OUT / 'uptime-after.txt')
