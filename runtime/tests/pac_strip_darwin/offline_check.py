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
    namespace['RESULT']['tools'] = tools
    check('absolute-binding', lambda: tools)
    check('single-relative-cache-path', lambda: recipe.bind_tools({**env, 'SCCACHE_PATH': 'sccache'}), True)
    check('single-missing-cache-path', lambda: recipe.bind_tools({**env, 'SCCACHE_PATH': str(tools_dir / 'absent')}), True)
    check('missing-reader-member', lambda: recipe.bind_tools({**env, 'PAC1481_LLVM_BIN': str(root / 'missing-collection')}), True)
    # One member missing in the otherwise correct installed collection.
    (tools_dir / 'llvm-readobj').unlink()
    check('one-reader-missing', lambda: recipe.bind_tools(env), True)
    (tools_dir / 'llvm-readobj').write_text((tools_dir / 'llvm-nm').read_text())
    (tools_dir / 'llvm-readobj').chmod(0o700)
    check('child-env-and-both-configure-argv', lambda: {'env': recipe.configure_env(env, tools),
        'off': list(map(str, namespace['configure'](root / 'off', False))),
        'on': list(map(str, namespace['configure'](root / 'on', True)))})
    tree = root / 'small-build'
    tree.mkdir()
    (tree / 'flags.make').write_text('real formed input')
    (tree / 'failed.o').write_bytes(b'object-record')
    original_copy = recipe.shutil.copy2
    def failed_copy(src, dst):
        raise RuntimeError('controlled preservation failure')
    recipe.shutil.copy2 = failed_copy
    check('preservation-failure-retains-original', lambda: recipe.preserve_then_delete(tree, root / 'failed-preservation'), True)
    assert (tree / 'flags.make').read_text() == 'real formed input' and (tree / 'failed.o').exists()
    recipe.shutil.copy2 = original_copy
    check('preservation-success-before-delete', lambda: recipe.preserve_then_delete(tree, root / 'retained'))
    assert not tree.exists()
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
    (root / 'sentinel.json').write_text(json.dumps({'calls': external, 'marker_exists': (root / 'external-marker').exists()}))
