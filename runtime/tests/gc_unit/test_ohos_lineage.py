#!/usr/bin/env python3
"""Exercise the actual OHOS runner with an existing product SO and test ELF.

Run on kkk2: test_ohos_lineage.py SNAPSHOT GIT_CHECKOUT LIB_DIR ELF OUTPUT
The snapshot must be a real source export without .git; the checkout must be
an independent real Git checkout. No product or test executable is replaced.
"""
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import sys

snapshot, checkout, lib, elf, output = map(lambda x: Path(x).resolve(), sys.argv[1:])
output.mkdir(parents=True, exist_ok=True)
assert not (snapshot / '.git').exists(), 'snapshot must not contain .git'
head = subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip()
failed_git = output / 'failed-git'
failed_git.mkdir(exist_ok=True)
(failed_git / 'git').write_text('#!/bin/sh\nexit 128\n')
(failed_git / 'git').chmod(0o755)
status_git = output / 'status-git'
status_git.mkdir(exist_ok=True)
real_git = subprocess.check_output(['which', 'git'], text=True).strip()
(status_git / 'git').write_text(f'#!/bin/sh\ncase "$*" in *status*) exit 128;; esac\nexec "{real_git}" "$@"\n')
(status_git / 'git').chmod(0o755)
cases = [
    ('snapshot', snapshot, {}, 'absent', None),
    ('checkout', checkout, {}, 'present', head),
    ('git-failure', checkout, {'PATH': f'{failed_git}:{os.environ["PATH"]}'}, 'absent', None),
    ('status-failure', checkout, {'PATH': f'{status_git}:{os.environ["PATH"]}'}, 'present', head),
    ('source-override', snapshot, {'SOURCE_COMMIT': 'explicit-source', 'CJ_RUNTIME_COMMIT': 'secondary'}, 'absent', None),
    ('cj-override', snapshot, {'CJ_RUNTIME_COMMIT': 'secondary'}, 'absent', None),
]
def run_case(case):
    name, tree, extra, git_state, git_head = case
    out = output / name
    out.mkdir(exist_ok=True)
    env = dict(os.environ)
    for key in ('SOURCE_COMMIT', 'CJ_RUNTIME_COMMIT', 'GC_UNIT_OHOS_HOST_ALLOW_MISSING_POST_DISPATCH'):
        env.pop(key, None)
    env.update(MRT_GC_UNIT_OHOS_HOST='1', GCV2_RUNTIME_LIB_DIR=str(lib),
               GCV2_RUNTIME_OUTPUT_ROOT=str(lib.parent.parent),
               GC_UNIT_OHOS_HOST_TEST_ELF=str(elf), GC_UNIT_OUT=str(out))
    env.update(extra)
    with (out / 'run.log').open('w') as log:
        rc = subprocess.call(['bash', str(tree / 'runtime/tests/gc_unit/run_standalone.sh')], env=env, stdout=log, stderr=subprocess.STDOUT)
    (out / 'run.rc').write_text(f'{rc}\n')
    try:
        # This assertion is the regression target: all three real filters ran,
        # and the runner propagated their actual result instead of Git's 128.
        receipt = out / 'ohos_host.receipt'
        assert rc == 0 and receipt.exists(), f'filters completed: rc={rc}, receipt={receipt.exists()}'
        text = receipt.read_text()
        for key in ('MAJOR', 'POST', 'EMPTY'):
            assert f'FILTER_{key}=PASS\n' in text, f'{key} did not pass'
        lineage = (out / 'ohos_host_lineage.txt').read_text()
        assert f'SOURCE_GIT={git_state}\n' in lineage, lineage
        if git_head:
            assert f'SOURCE_GIT_HEAD={git_head}\n' in lineage, lineage
        if name == 'status-failure':
            assert 'SOURCE_STATUS_RC=128\n' in lineage, lineage
        if name == 'source-override':
            assert 'SOURCE_COMMIT=explicit-source\n' in lineage, lineage
        if name == 'cj-override':
            assert 'SOURCE_COMMIT=secondary\n' in lineage, lineage
        if name == 'snapshot':
            assert 'SOURCE_PRODUCT_IDENTITY=src-' in lineage, lineage
            assert 'SOURCE_COMMIT_ORIGIN=product-declared\n' in lineage, lineage
        print(f'PASS {name}: filters completed; lineage assertions reached', flush=True)
    except AssertionError as error:
        print(f'FAIL {name}: {error}', flush=True)
        return name
    return None

with ThreadPoolExecutor(max_workers=len(cases)) as pool:
    failures = [name for name in pool.map(run_case, cases) if name]
print(f'cases={len(cases)} failures={len(failures)} names={failures}', flush=True)
sys.exit(bool(failures))
