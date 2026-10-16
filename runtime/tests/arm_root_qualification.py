#!/usr/bin/env python3
"""One bounded official ARM default-product qualification; never runs the suite."""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import resource
import shutil
import subprocess
import sys
import time

TESTS = (
    'ZRootTask.YoungCarrierPublishesOnlyYoung',
    'ZRootTask.YoungCarrierParallelDispatch',
    'ZRootTask.OldCarrierPublishesBothGenerations',
    'ZRootTask.OldCarrierParallelDispatch',
)
FILTER = ':'.join(TESTS)


def validate(sha, selected):
    if not re.fullmatch(r'[0-9a-f]{40}', sha):
        raise ValueError('candidate must be a full SHA')
    if selected != FILTER:
        raise ValueError('filter must be exactly the ordered four-test manifest')
    return TESTS


def verify_result(name, cut, rc, log):
    red = cut and name.endswith('ParallelDispatch')
    expected = (1, 0, 1) if red else (1, 1, 0)
    counts = tuple(map(int, re.findall(r'\[========\] (\d+) tests: (\d+) passed, (\d+) failed', log)[-1]))
    if counts != expected or (rc != 0) != red:
        raise ValueError(f'unexpected result {name}: rc={rc} counts={counts}')
    if f'[  RUN   ] {name}' not in log:
        raise ValueError('missing exact RUN')
    budget = re.search(r'CARRIER_WORKER_BUDGET requested=(\d+) cpu=(\d+) heap=(\d+) heuristic=(\d+) young=(\d+) old=(\d+) conc=(\d+) young_max=(\d+) old_max=(\d+)', log)
    if not budget:
        raise ValueError('missing real budget observation')
    values = tuple(map(int, budget.groups()))
    requested = 3 if name.endswith('ParallelDispatch') else 1
    capacity = 1 if cut else requested
    if values != (requested, 4, 512 * 1024 * 1024, 1, capacity, capacity, capacity, capacity, capacity):
        raise ValueError(f'input/capacity not applicable: {values}')
    if red:
        # This is a fixture-budget rejection, never a product root-result red.
        if 'EXPECT_EQ failed: ZYoungGCThreads (==1) vs workers (==3)' not in log or 'CARRIER_ROOT_TASK_TARGET' in log:
            raise ValueError('budget cut did not stop at the exact budget assertion')
        if f'[  FAIL  ] {name}' not in log or f'GC_UNIT_OTHER_VM_OKIDOKI {name}' in log:
            raise ValueError('invalid failure/sentinel result')
    else:
        active = f'CARRIER_WORKER_ACTIVE requested={requested} max={requested} active={requested}'
        if active not in log or f'[  PASS  ] {name}' not in log or f'GC_UNIT_OTHER_VM_OKIDOKI {name}' not in log:
            raise ValueError('missing active/PASS/child completion')
        result = re.search(r'CARRIER_ROOT_TASK_TARGET executed=1 young_only=(\d+) workers=(\d+).*armed_before=1 guard_matches=1 young_published=(\d+) old_published=(\d+) local=0', log)
        young_only = name.startswith('ZRootTask.Young')
        if not result or int(result[1]) != int(young_only) or int(result[2]) != requested or int(result[3]) == 0 or (int(result[4]) == 0) != young_only:
            raise ValueError('root result assertions not observed')
    return {'rc': rc, 'counts': counts, 'budget': values, 'root_result': 'NOT_REACHED_BUDGET_REJECTION' if red else 'PASS'}


def qualify(args):
    source = Path(__file__).resolve().parents[2]
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    records = []
    def run(name, command, env=None):
        start = time.monotonic()
        with (evidence / (name + '.log')).open('w') as log:
            log.write('COMMAND=' + repr(list(map(str, command))) + '\n')
            log.flush()
            result = subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, env=env)
        records.append(dict(name=name, rc=result.returncode, wall=time.monotonic()-start))
        (evidence / 'commands.json').write_text(json.dumps(records, indent=2))
        print(f'{name} rc={result.returncode}', flush=True)
        return result.returncode
    def need(name, command, env=None):
        rc = run(name, command, env)
        if rc:
            raise RuntimeError(f'{name} failed rc={rc}; dependent actions NOT_RUN')
    def identity(name, files):
        (evidence / (name + '.sha256')).write_text(''.join(
            hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + str(p) + '\n' for p in files))
        (evidence / (name + '-times.json')).write_text(json.dumps(
            {str(p): dict(size=p.stat().st_size, mtime_ns=p.stat().st_mtime_ns) for p in files}, indent=2))
    validate(args.candidate, args.filter)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    need('source-sha', ['git', '-C', source, 'rev-parse', 'HEAD'])
    actual = (evidence / 'source-sha.log').read_text().splitlines()[-1]
    if actual != args.candidate:
        raise ValueError('checkout differs from fixed candidate SHA')
    domain = dict(machine=platform.machine(), affinity=sorted(os.sched_getaffinity(0)),
                  load=os.getloadavg(), core=resource.getrlimit(resource.RLIMIT_CORE),
                  heap_bytes=512*1024*1024, candidate=actual)
    (evidence / 'domain.json').write_text(json.dumps(domain, indent=2))
    # Historical official input: four actual CPUs and a 512MiB heap produce
    # one default concurrent worker. Do not force affinity or alter assertions.
    if domain['machine'] != 'aarch64' or len(domain['affinity']) != 4 or any(k.startswith('cj') for k in os.environ):
        raise ValueError('NOT_APPLICABLE: historical CPU/heap input not satisfied')
    need('uptime-before', ['uptime'])
    env = dict(os.environ, GC_UNIT_GATE_SKIP='1', GC_UNIT_BUILD_ONLY='1',
               GC_UNIT_JOBS=str(len(domain['affinity'])))
    build = evidence / 'build'
    need('configure', ['cmake', '-S', source / 'runtime', '-B', build,
         '-DCMAKE_BUILD_TYPE=Release', '-DCOPYGC_FLAG=1', '-DDOPRA_FLAG=1',
         '-DRUNTIME_TRACE_FLAG=1', '-DCJ_SDK_VERSION=0.0.1', '-DDISABLE_VERSION_CHECK=1',
         '-DCMAKE_C_COMPILER=/usr/bin/clang', '-DCMAKE_CXX_COMPILER=/usr/bin/clang++',
         '-DCMAKE_AR_PATH=ar', '-DCMAKE_C_COMPILER_LAUNCHER=sccache',
         '-DCMAKE_CXX_COMPILER_LAUNCHER=sccache', '-DCMAKE_ASM_COMPILER_LAUNCHER=sccache',
         '-DMRT_TESTABLE_INTERNALS=OFF', '-DMRT_GC_UNIT_TESTS=OFF'], env)
    need('product-build', ['cmake', '--build', build, '--target', 'cangjie-runtime', '-j', len(domain['affinity'])], env)
    cache = (build / 'CMakeCache.txt').read_text()
    root = Path(re.search(r'^OUTPUT_TEMP_PATH:INTERNAL=(.*)$', cache, re.M)[1])
    library = next(root.glob('lib/*/libcangjie-runtime.so')).parent
    products = [library / 'libcangjie-runtime.so', library / 'libboundscheck.so']
    identity('product-at-publication', products)
    shutil.copytree(root, evidence / 'publication')
    shutil.copy2(build / 'CMakeCache.txt', evidence / 'CMakeCache.txt')
    shutil.copy2(build / 'compile_commands.json', evidence / 'product-compile-commands.json')
    library = evidence / 'publication' / library.relative_to(root)
    products = [library / p.name for p in products]
    env.update(GCV2_RUNTIME_LIB_DIR=str(library), GCV2_RUNTIME_OUTPUT_ROOT=str(evidence / 'publication'),
               LD_LIBRARY_PATH=str(library), GC_UNIT_OUT=str(evidence / 'standalone'))
    need('standalone-build-only', ['bash', source / 'runtime/tests/gc_unit/run_standalone.sh'], env)
    out = evidence / 'standalone'
    elf = out / 'cj_gc_unit'
    green = out / 'cj_gc_unit.green'
    shutil.copy2(elf, green)
    identity('green-at-link', [green] + products)
    need('enumerate', [green, '--gtest_list_tests'], env)
    registered = []
    suite = ''
    for line in (evidence / 'enumerate.log').read_text().splitlines():
        if line.endswith('.') and not line.startswith(' '):
            suite = line
        elif line.startswith('  ') and suite:
            registered.append(suite + line.strip())
    if any(registered.count(t) != 1 for t in TESTS):
        raise ValueError('four filters must each be registered exactly once')
    results = {}
    (evidence / 'selected.json').write_text(json.dumps(TESTS))
    def arm(label, selected_elf, cut=False):
        for name in TESTS:
            key = label + '-' + name
            rc = run(key, [selected_elf, '--gtest_filter=' + name], env)
            results[key] = verify_result(name, cut, rc, (evidence / (key + '.log')).read_text())
            (evidence / 'targets.json').write_text(json.dumps(results, indent=2))
    # First item is the representative real input, included in the four-item arm.
    arm('green', green)
    fixture = source / 'runtime/tests/gc_unit/test_verify_fail_close.cpp'
    original = fixture.read_text()
    start = original.index('void CheckCarrierMarkTask(')
    prefix, body = original[:start], original[start:]
    for line in ('    param.gcParam.youngGCThreadsSet = true;\n', '    param.gcParam.oldGCThreadsSet = true;\n'):
        if body.count(line) != 1:
            raise ValueError('budget cut source identity differs')
        body = body.replace(line, '')
    cut = prefix + body
    (evidence / 'cut.diff').write_text(''.join(difflib.unified_diff(original.splitlines(True), cut.splitlines(True),
        fromfile='a/runtime/tests/gc_unit/test_verify_fail_close.cpp', tofile='b/runtime/tests/gc_unit/test_verify_fail_close.cpp')))
    recipe = json.loads((out / 'main-build-recipe.json').read_text())
    rows = recipe['rows']
    matches = [r for r in rows if r[0] == 'main' and Path(r[2]) == fixture]
    if len(matches) != 1:
        raise ValueError('expected one fixture compilation')
    fixture.write_text(cut)
    try:
        need('cut-test-compile', recipe['compiler'] + recipe['flags'] + ['-c', fixture, '-o', matches[0][1]], env)
        link = recipe['compiler'] + recipe['flags'] + [r[1] for r in rows if r[0] == 'main']
        if recipe['copy_object']:
            link.append(recipe['copy_object'])
        link += ['-L' + str(library), '-Wl,-rpath,' + str(library), '-Wl,--exclude-libs,ALL',
                 '-lcangjie-runtime', '-lboundscheck', '-o', str(elf)]
        need('cut-test-link', link, env)
        identity('cut-at-link', [elf] + products)
        arm('cut-budget', elf, True)
    finally:
        fixture.write_text(original)
    identity('restored-reused', [green] + products)
    arm('restored', green)
    need('source-restored', ['git', '-C', source, 'diff', '--exit-code'], env)
    need('uptime-after', ['uptime'])
    (evidence / 'QUALIFICATION_DONE').write_text('12 exact filter invocations; budget red is not root red\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', required=True)
    parser.add_argument('--filter', required=True)
    parser.add_argument('--evidence', type=Path, default=Path('evidence'))
    parser.add_argument('--validate-only', action='store_true')
    args = parser.parse_args()
    validate(args.candidate, args.filter)
    if not args.validate_only:
        qualify(args)


if __name__ == '__main__':
    main()
