"""One bounded offline batch. Never launches a compiler/cache/configure/native program."""
import ast
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
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

# Test only the actual definitions, without importing the product executor.
source = Path(__file__).with_name('run.py').read_text()
module = ast.parse(source)
functions = [n for n in module.body if isinstance(n, ast.FunctionDef) and n.name in ('configure', 'lr_flow')]
namespace = {'re': re, 'RESULT': {'sdk_path': '/recorded/sdk'}}
exec(compile(ast.Module(body=functions, type_ignores=[]), 'actual-recipe-definitions', 'exec'), namespace)

def check(name, action, rejected=False):
    start = time.monotonic()
    try:
        result = action()
    except (RuntimeError, FileNotFoundError, KeyError) as error:
        if not rejected:
            raise
        result = {'rejected': str(error)}
    else:
        if rejected:
            raise AssertionError('expected target rejection: ' + name)
    records.append({'name': name, 'result': result, 'wall': time.monotonic() - start})
    (root / 'results.json').write_text(json.dumps(records, indent=2))

try:
    tools_dir = root / 'tools'
    tools_dir.mkdir()
    for name in ('sccache', 'llvm-nm', 'llvm-objdump', 'llvm-readobj'):
        p = tools_dir / name
        p.write_text('#!/bin/sh\necho FORBIDDEN >> "' + str(root / 'external-marker') + '"\nexit 99\n')
        p.chmod(0o700)
    env = {'SCCACHE_PATH': str(tools_dir / 'sccache'), 'PAC1481_LLVM_BIN': str(tools_dir)}
    tools = recipe.bind_tools(env)
    build = root / 'configured'
    cache = tools['sccache']['path']
    recipes = {}
    for domain, domain_root in [('top', build), ('child', build / 'cjthread-build')]:
        domain_root.mkdir(parents=True, exist_ok=True)
        (domain_root / 'CMakeCache.txt').write_text(
            'CMAKE_C_COMPILER_LAUNCHER:STRING=' + cache + '\n' +
            'CMAKE_CXX_COMPILER_LAUNCHER:STRING=' + cache + '\n' +
            'CANGJIE_COMPILER_CACHE:FILEPATH=' + cache + '\n')
        for lang in ('C', 'CXX'):
            target = domain_root / 'CMakeFiles' / (lang + '-owner.dir')
            target.mkdir(parents=True)
            path = target / 'build.make'
            path.write_text('\t' + cache + ' /recorded/compiler $(' + lang +
                '_FLAGS) -o CMakeFiles/' + target.name + '/input.o -c input.' +
                ('c' if lang == 'C' else 'cpp') + '\n')
            recipes[domain, lang] = path
    def consumed():
        value = recipe.verify_configured(build, tools)
        assert {r['language'] for r in value['top']['commands']} == {'C', 'CXX'}
        assert {r['language'] for r in value['child']['commands']} == {'C', 'CXX'}
        assert all('cjthread-build' not in r['file'] for r in value['top']['commands'])
        print('ASSERT_EXECUTED independent-domain-language-target')
        return value
    check('independent-top-child-positive', consumed)
    for (domain, lang), path in recipes.items():
        original = path.read_text()
        path.write_text(original.replace(cache + ' ', ''))
        def reject_domain(domain=domain):
            try:
                recipe.verify_configured(build, tools)
            except RuntimeError as error:
                assert str(error).startswith(domain + ' generated launcher consumption mismatch:')
                print('ASSERT_EXECUTED target-rejection ' + domain)
                raise
        check(domain + '-' + lang + '-single-domain-cut', reject_domain, True)
        path.write_text(original)
    check('independent-domain-restored', consumed)
    def small_tree(name, archive=False):
        tree = root / name
        tree.mkdir()
        (tree / 'flags.make').write_text('formed flags')
        (tree / 'failed.o').write_bytes(b'saved object bytes')
        if archive:
            child = tree / 'staging'
            child.mkdir()
            (child / 'libcjthread.a').write_bytes(b'recorded archive bytes; no compiler')
            (child / 'thread.h').write_text('saved header')
            (tree / 'runtime-cjthread-inputs.txt').write_text('staging/libcjthread.a\nstaging/thread.h\n')
            (tree / 'publisher-identity.json').write_text('recorded identity metadata')
        return tree
    def preserve_success(name, archive=False):
        tree = small_tree(name, archive)
        retained = root / (name + '-retained')
        value = recipe.preserve_then_delete(tree, retained)
        assert not tree.exists() and any(r.get('status') == 'MISSING' for r in value)
        assert (retained / 'failed.o').read_bytes() == b'saved object bytes'
        if archive:
            assert (retained / 'staging/libcjthread.a').read_bytes() == b'recorded archive bytes; no compiler'
            assert (retained / 'publisher-identity.json').read_text() == 'recorded identity metadata'
            assert (retained / 'runtime-cjthread-inputs.txt').exists()
        print('ASSERT_EXECUTED complete-preservation-and-MISSING')
        return value
    check('r-get-multiple-input-success-MISSING', lambda: preserve_success('minimal'))
    check('child-archive-identity-input-success', lambda: preserve_success('archive', True))
    original_copy = recipe.shutil.copy2
    def failed_copy(src, dst):
        raise RuntimeError('controlled preservation failure')
    tree = small_tree('copy-failure', True)
    recipe.shutil.copy2 = failed_copy
    check('archive-copy-failure-retains-tree', lambda: recipe.preserve_then_delete(tree, root / 'copy-failure-retained'), True)
    assert (tree / 'staging/libcjthread.a').exists()
    recipe.shutil.copy2 = original_copy
    tree = small_tree('lost-input', True)
    (tree / 'staging/libcjthread.a').unlink()
    check('declared-formed-input-loss-rejected', lambda: recipe.preserve_then_delete(tree, root / 'lost-input-retained'), True)
    assert tree.exists()
    def multi_arm():
        out = root / 'arms'
        out.mkdir()
        for name in ('first', 'second', 'third'):
            tree = out / name
            tree.mkdir()
            (tree / 'flags.make').write_text(name)
        def first_only(src, dst):
            if 'first' in Path(src).parts:
                raise RuntimeError('controlled first-arm copy failure')
            return original_copy(src, dst)
        recipe.shutil.copy2 = first_only
        result = {'first_error': 'original configure failure'}
        try:
            assert not recipe.preserve_arms(out, ('first', 'second', 'third'), result)
        finally:
            recipe.shutil.copy2 = original_copy
        assert result['first_error'] == 'original configure failure'
        assert result['preservation']['first']['status'] == 'FAILED_RETAINED'
        assert (out / 'first/flags.make').exists()
        assert all(result['preservation'][n]['status'] == 'COMPLETED' for n in ('second', 'third'))
        assert all(not (out / n).exists() for n in ('second', 'third'))
        print('ASSERT_EXECUTED multi-arm-independent-first-error')
        return result
    check('multi-arm-first-fails-others-preserved', multi_arm)
    positive = '0: a9bf7bfd stp x29, x30, [sp, #-16]!\n4: aa0003fe mov x30, x0\n8: d50320ff xpaclri\nc: aa1e03e0 mov x0, x30\n10: a8c17bfd ldp x29, x30, [sp], #16\n14: d65f03c0 ret'
    check('supported-reader-transport-only', lambda: namespace['lr_flow'](positive, True))
    check('broken-lr-restore-target', lambda: namespace['lr_flow'](positive.replace('ldp x29, x30', 'ldp x29, x28'), True), True)
    check('unsupported-branch-invalid', lambda: namespace['lr_flow'](positive.replace('mov x0, x30', 'b 0x14'), True), True)
    raw = Path(__file__).with_name('saved-real-lr.s').read_text()
    check('saved-real-assembly-format-invalid', lambda: namespace['lr_flow'](raw, True), True)
    assert not external and not (root / 'external-marker').exists()
    (root / 'summary.json').write_text(json.dumps({'status': 'PASS', 'n': len(records),
        'external_calls': external, 'external_marker': 'ABSENT', 'scope': 'offline transport only; Apple qualification NOT_RUN'}, indent=2))
finally:
    (root / 'completed.json').write_text(json.dumps({'n': len(records), 'records': records}, indent=2))
    (root / 'sentinel.json').write_text(json.dumps({'calls': external, 'marker_exists': (root / 'external-marker').exists()}))
