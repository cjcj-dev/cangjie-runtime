"""Single frozen R1-R3 offline batch: byte mirrors, no external product tools."""
import ast
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from types import SimpleNamespace
import recipe

root = Path(sys.argv[1]).resolve()
root.mkdir(exist_ok=False)
records = []
external = []

def sentinel(*args, **kwargs):
    external.append(str(args))
    raise RuntimeError('FORBIDDEN external invocation')
subprocess.Popen = subprocess.run = subprocess.check_output = sentinel
os.system = os.popen = sentinel

def audit(event, args):
    if event in ('subprocess.Popen', 'os.system', 'os.posix_spawn', 'os.exec', 'os.fork'):
        sentinel(event, args)
sys.addaudithook(audit)

def definitions(path, names, namespace):
    nodes = [n for n in ast.parse(path.read_text()).body
             if isinstance(n, ast.FunctionDef) and n.name in names]
    assert {n.name for n in nodes} == set(names)
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(path), 'exec'), namespace)

here = Path(__file__).parent
namespace = {'re': re, 'json': json, 'OUT': root}
definitions(here / 'run.py', ('function_block', 'lr_flow', 'instruction_checks'), namespace)
# Pure read-only identity generation from the real producer, not a copied schema.
producer = {'json': json, 'hashlib': hashlib, 'Path': Path, 're': re,
            'subprocess': subprocess}
definitions(here.parents[1] / 'build/publish_runtime_output.py', ('sha', 'canonical_json', 'generated_inputs'), producer)

def check(name, action, expected=None):
    start = time.monotonic()
    namespace["OUT"] = root / ("instructions-" + name)
    namespace["OUT"].mkdir()
    try:
        value = action()
    except RuntimeError as error:
        if expected is None or expected not in str(error):
            raise
        assert not (namespace['OUT'] / 'instruction-check.json').exists()
        value = {'target_rejection': str(error), 'success_file_absent': True}
    else:
        if expected is not None:
            raise AssertionError('expected target rejection: ' + name)
    print('ASSERT_EXECUTED ' + name, flush=True)
    records.append({'name': name, 'result': value, 'wall': time.monotonic() - start})
    (root / 'results.json').write_text(json.dumps(records, indent=2))

def put(tree, relative, data=b'byte mirror; NOT a compiled product'):
    path = tree / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path

def mirror(name):
    tree = root / name
    tree.mkdir()
    build = tree / 'build'
    staging = build / 'runtime-staging'
    archive = put(staging, 'lib/libcangjie-thread.a')
    header = put(staging, 'include/thread.h')
    rt = put(staging, 'lib/aarch64_release/libcangjie-runtime.dylib')
    bc = put(staging, 'lib/aarch64_release/libboundscheck.dylib')
    put(tree, 'install_aarch64/lib/darwin_aarch64_cjnative/libcangjie-thread.a')
    put(tree, 'install_aarch64/lib/darwin_aarch64_cjnative/libcangjie-aio.a')
    put(staging, 'ar/aarch64_release/libBase.a')
    put(staging, 'ar/aarch64_release/libUnwindStack.a')
    link = put(build, 'src/Base/CMakeFiles/Base.dir/link.txt', b'ar qc libBase.a input.o\n')
    put(build, 'runtime-link-inputs.txt', (str(link) + '\n').encode())
    put(build, 'runtime-cjthread-inputs.txt', (str(archive) + '\n' + str(header) + '\n').encode())
    put(build, 'compile_commands.json', b'[{"file":"input.cpp","command":"recorded -c input.cpp"}]\n')
    put(build, 'runtime-generated-inputs/Base-CXX.txt', b'definitions=\noptions=\n')
    put(build, 'compiler/CMakeCXXCompiler.cmake', b'set(CMAKE_CXX_COMPILER recorded)\n')
    args = SimpleNamespace(build=build, staging=staging, generator='Unix Makefiles',
        compiler_state=build / 'compiler', toolchain='', runtime=rt, boundscheck=bc)
    identity, paths = producer['generated_inputs'](args)
    text = producer['canonical_json'](identity)
    publication = tree / 'runtime/output/temp/mirror-config'
    put(publication, 'runtime-build-inputs.txt', text.encode())
    put(publication, 'runtime-build-config.txt', ('SCHEMA_VERSION=6\nCONFIG_SIGNATURE_SHA256=' +
        hashlib.sha256(text.encode()).hexdigest() + '\n').encode())
    put(publication, 'lib/aarch64_release/libcangjie-runtime.dylib', rt.read_bytes())
    put(publication, 'lib/aarch64_release/libboundscheck.dylib', bc.read_bytes())
    hashes = {str(p.relative_to(publication)): recipe.digest(p) for p in publication.rglob('*.dylib')}
    put(publication, 'runtime-product-hashes.json', producer['canonical_json'](hashes).encode())
    return tree

def preserve(name):
    tree = mirror(name)
    before = {str(p.relative_to(tree)): recipe.digest(p) for p in tree.rglob('*') if p.is_file()}
    retained = root / (name + '-inputs')
    result = {'first_error': 'recorded failure'}
    assert recipe.preserve_arms(root, (name,), result)
    assert result['preservation'][name]['status'] == 'COMPLETED' and not tree.exists()
    inventory = {r['path']: r['sha256'] for r in result['preservation'][name]['inventory'] if 'path' in r}
    required = [p for p in before if p.endswith(('.a', '.dylib')) or Path(p).name in
                ('runtime-build-inputs.txt', 'runtime-build-config.txt', 'runtime-product-hashes.json',
                 'runtime-cjthread-inputs.txt', 'runtime-link-inputs.txt')]
    assert all(inventory[p] == before[p] == recipe.digest(retained / p) for p in required)
    return {'required_hashes': {p: inventory[p] for p in required}, 'receipt': result}

def missing(name, relative):
    tree = mirror(name)
    (tree / relative).unlink()
    result = {'first_error': 'original configure failure'}
    assert not recipe.preserve_arms(root, (name,), result)
    assert tree.exists() and result['first_error'] == 'original configure failure'
    assert 'formed publisher input missing/outside tree:' in result['preservation_errors'][name]
    return result

def copy_failure():
    tree = mirror('copy-failure')
    original = recipe.shutil.copy2
    def fail(src, dst):
        if str(src).endswith('libBase.a'):
            raise RuntimeError('controlled Base archive copy failure')
        return original(src, dst)
    recipe.shutil.copy2 = fail
    result = {'first_error': 'original failure'}
    try:
        assert not recipe.preserve_arms(root, ('copy-failure',), result)
    finally:
        recipe.shutil.copy2 = original
    assert tree.exists() and (tree / 'build/runtime-staging/ar/aarch64_release/libBase.a').exists()
    assert 'controlled Base archive copy failure' in result['preservation_errors']['copy-failure']
    return result

def multi():
    for name in ('first', 'second', 'third'):
        mirror(name)
    (root / 'first/build/runtime-staging/lib/libcangjie-thread.a').unlink()
    result = {'first_error': 'original configure failure'}
    assert not recipe.preserve_arms(root, ('first', 'second', 'third'), result)
    assert result['first_error'] == 'original configure failure'
    assert result['preservation']['first']['status'] == 'FAILED_RETAINED'
    assert all(result['preservation'][n]['status'] == 'COMPLETED' for n in ('second', 'third'))
    return result

positive = '0: a9bf7bfd stp x29, x30, [sp, #-16]!\n4: aa0003fe mov x30, x0\n8: d50320ff xpaclri\nc: aa1e03e0 mov x0, x30\n10: a8c17bfd ldp x29, x30, [sp], #16\n14: d65f03c0 ret'
caller = '0: a9bf7bfd stp x29, x30, [sp, #-16]!\n4: 94000000 bl 0 <ptrauthstripinstpointer>\n8: a8c17bfd ldp x29, x30, [sp], #16\nc: d65f03c0 ret'

def instructions(callee=positive, target=caller, false_checker=False):
    (namespace['OUT'] / 'producer-disassembly.log').write_text('d503211f')
    (namespace['OUT'] / 'product-disassembly.log').write_text('0000 <isn2cstubframe>:\n' + target +
        '\n0100 <ptrauthstripinstpointer>:\n' + callee)
    original = namespace['lr_flow']
    if false_checker:
        namespace['lr_flow'] = lambda *args: False
    try:
        namespace['instruction_checks']()
    finally:
        namespace['lr_flow'] = original
    return json.loads((namespace['OUT'] / 'instruction-check.json').read_text())

try:
    check('caller-callee-positive', lambda: instructions(positive.replace('aa0003fe', 'fe 03 00 aa')))
    check('R3-overwritten-x0-add-rejected', lambda: instructions(positive.replace('10: a8', 'e: 910043e0 add x0, sp, #16\n10: a8')), 'strip result dataflow')
    check('R3-overwritten-x30-add-rejected', lambda: instructions(target='0: 910043fe add x30, sp, #16\n4: d65f03c0 ret'), 'actual return does not restore')
    check('w0-write-invalidates-x0', lambda: instructions(positive.replace('10: a8', 'e: 2a1f03e0 mov w0, wzr\n10: a8')), 'strip result dataflow')
    check('w30-write-invalidates-x30', lambda: instructions(target='0: 2a1f03fe mov w30, wzr\n4: d65f03c0 ret'), 'actual return does not restore')
    check('mov-invalidates-stale-address-base', lambda: instructions(target='0: a9bf7bfd stp x29, x30, [sp, #-16]!\n4: 910003fd mov x29, sp\n8: aa0003fd mov x29, x0\nc: f94007be ldr x30, [x29, #8]\n10: d65f03c0 ret'), 'actual return does not restore')
    check('nonwriting-cmp-preserves-tags', lambda: instructions(positive.replace('10: a8', 'e: eb00001f cmp x0, x0\n10: a8')))
    sp = '0: d10083ff sub sp, sp, #32\n4: a9007bfd stp x29, x30, [sp]\n8: aa0003fe mov x30, x0\nc: d50320ff xpaclri\n10: aa1e03e0 mov x0, x30\n14: a9407bfd ldp x29, x30, [sp]\n18: 910083ff add sp, sp, #32\n1c: d65f03c0 ret'
    check('legal-SP-sub-add-restores-LR-and-strip', lambda: instructions(sp))
    check('unsupported-control-flow-INVALID', lambda: instructions(positive.replace('mov x0, x30', 'b 0x14')), 'INVALID: unsupported LR control-flow')
    check('caller-consumes-checker-false', lambda: instructions(false_checker=True), 'INVALID: caller LR checker rejected')
    check('add-nonbase-invalidates-strip', lambda: instructions(positive.replace('10: a8', 'e: 91000400 add x0, x0, #1\n10: a8')), 'strip result dataflow')
    check('historical-clang-S-explicit-INVALID', lambda: instructions((here / 'saved-real-lr.s').read_text()), 'INVALID: no encoded return instruction')
    assert len(records) == 12
    assert not external
    (root / 'summary.json').write_text(json.dumps({'n': len(records), 'status': 'PASS',
        'scope': 'offline recipe only; byte mirrors, not product evidence', 'external_calls': external}, indent=2))
finally:
    (root / 'completed.json').write_text(json.dumps({'n': len(records), 'records': records}, indent=2))
    (root / 'sentinel.json').write_text(json.dumps({'calls': external, 'scope': 'only hooked paths; no coverage extrapolation'}))
