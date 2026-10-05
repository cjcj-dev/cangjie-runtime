#!/usr/bin/env python3
"""One native teardown build and finite baseline/candidate/cut/restored batch."""
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

BASE = 'fe7c402adfdf08ea9c28ad0246af95c163ebda18'
TEST = 'RuntimeWorkers.ActivePoolBeforeHarnessShutdown'
sys.path.insert(0, str(Path(__file__).resolve().parent / 'gc_unit'))
from check_teardown_records import verify


def validate(candidate, selected):
    if not re.fullmatch('[0-9a-f]{40}', candidate) or selected != TEST:
        raise ValueError('requires full candidate SHA and exact teardown filter')


def offline_controls(normal, evidence):
    """Only mutate retained real records; no targets or synthetic producer."""
    verify(normal)
    modifications = [
        ('missing-run', 'teardown.log', '[  RUN   ] ' + TEST, ''),
        ('missing-sentinel', 'teardown-exited.log', 'GC_UNIT_OTHER_VM_OKIDOKI ' + TEST, ''),
        ('missing-target', 'teardown-exited.log', 'ASSERT_TEARDOWN_BEFORE_SENTINEL samples=1 PASS', ''),
        ('missing-completion', 'teardown-live.log', 'TEARDOWN_CONSTRUCT_EXECUTED product_rc=0', ''),
        ('not-run', 'teardown-exited.rc', '0', '77'),
        ('timeout', 'teardown-exited.rc', '0', '124'),
        ('empty-samples', 'teardown-exited.log', 'samples=1 PASS', 'samples=0 PASS'),
        ('missing-held', 'teardown-exited.log', 'CONSTRUCT_HOLD_EXIT', 'REMOVED_HOLD'),
        ('missing-state', 'teardown-exited.log', 'TASK_STATE ', 'REMOVED_STATE '),
        ('missing-worker-set', 'teardown-exited.log', 'WORKER_SET ', 'REMOVED_WORKER_SET '),
        ('missing-regset', 'teardown-exited.log', 'REGSET ', 'REMOVED_REGSET '),
        ('missing-log', 'teardown-live.log', None, None),
    ]
    results = [{'name': 'normal', 'status': 'accepted', 'target_starts': 0}]
    root = evidence / 'offline-controls'
    for name, filename, old, new in modifications:
        directory = root / name
        shutil.copytree(normal, directory)
        path = directory / filename
        if old is None:
            path.unlink()
        else:
            original = path.read_text()
            if old not in original:
                raise ValueError('control source absent: ' + name)
            path.write_text(original.replace(old, new, 1) if filename.endswith('.rc') else original.replace(old, new))
        try:
            verify(directory)
        except (ValueError, OSError, KeyError, IndexError) as error:
            results.append({'name': name, 'status': 'rejected', 'reason': str(error), 'target_starts': 0})
        else:
            raise ValueError('control incorrectly accepted: ' + name)
    (evidence / 'offline-controls.json').write_text(json.dumps(results, indent=2))


def qualify(args):
    source = Path(__file__).resolve().parents[2]
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    records = []
    def run(name, command, env=None):
        begin = time.time()
        with (evidence / (name + '.log')).open('w') as log:
            log.write('COMMAND=' + repr(list(map(str, command))) + '\n')
            log.flush()
            proc = subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, env=env)
        record = dict(name=name, command=list(map(str, command)), cwd=os.getcwd(),
                      started=begin, ended=time.time(), rc=proc.returncode)
        records.append(record)
        (evidence / 'commands.json').write_text(json.dumps(records, indent=2))
        (evidence / (name + '.rc')).write_text(str(proc.returncode) + '\n')
        print(name + ' rc=' + str(proc.returncode), flush=True)
        return proc.returncode
    def need(name, command, env=None):
        rc = run(name, command, env)
        if rc:
            raise RuntimeError(f'{name} rc={rc}; dependent actions NOT_RUN')
    def identity(name, paths):
        (evidence / (name + '.sha256')).write_text(''.join(
            hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + str(p) + '\n' for p in paths))
    validate(args.candidate, args.filter)
    need('source-sha', ['git', '-C', source, 'rev-parse', 'HEAD'])
    if (evidence / 'source-sha.log').read_text().splitlines()[-1] != args.candidate:
        raise ValueError('checkout differs from fixed candidate')
    machine = platform.machine()
    if machine not in ('x86_64', 'aarch64'):
        raise ValueError('unsupported native runner')
    (evidence / 'domain.json').write_text(json.dumps(dict(
        candidate=args.candidate, machine=machine, kernel=platform.release(),
        glibc=os.confstr('CS_GNU_LIBC_VERSION'), affinity=sorted(os.sched_getaffinity(0)),
        core=resource.getrlimit(resource.RLIMIT_CORE), load=os.getloadavg(),
        runner={k: os.environ.get(k) for k in ('RUNNER_OS', 'RUNNER_ARCH', 'ImageOS', 'ImageVersion', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT')}), indent=2))
    need('uptime-before', ['uptime'])
    env = dict(os.environ, GC_UNIT_GATE_SKIP='1', GC_UNIT_BUILD_ONLY='1',
               GC_UNIT_JOBS=str(len(os.sched_getaffinity(0))))
    build = evidence / 'build'
    need('configure', ['cmake', '-S', source / 'runtime', '-B', build,
         '-DCMAKE_BUILD_TYPE=Release', '-DCOPYGC_FLAG=1', '-DDOPRA_FLAG=1',
         '-DRUNTIME_TRACE_FLAG=1', '-DCJ_SDK_VERSION=0.0.1', '-DDISABLE_VERSION_CHECK=1',
         '-DCMAKE_C_COMPILER=/usr/bin/clang', '-DCMAKE_CXX_COMPILER=/usr/bin/clang++',
         '-DCMAKE_AR_PATH=ar', '-DCMAKE_C_COMPILER_LAUNCHER=sccache',
         '-DCMAKE_CXX_COMPILER_LAUNCHER=sccache', '-DCMAKE_ASM_COMPILER_LAUNCHER=sccache',
         '-DMRT_TESTABLE_INTERNALS=OFF', '-DMRT_GC_UNIT_TESTS=OFF'], env)
    need('product-build', ['cmake', '--build', build, '--target', 'cangjie-runtime', '-j', len(os.sched_getaffinity(0))], env)
    cache = (build / 'CMakeCache.txt').read_text()
    root = Path(re.search(r'^OUTPUT_TEMP_PATH:INTERNAL=(.*)$', cache, re.M)[1])
    libraries = list(root.glob('lib/*/libcangjie-runtime.so'))
    if len(libraries) != 1:
        raise ValueError('expected one product publication')
    library = libraries[0].parent
    identity('product-at-link', [library / 'libcangjie-runtime.so', library / 'libboundscheck.so'])
    shutil.copytree(root, evidence / 'publication')
    library = evidence / 'publication' / library.relative_to(root)
    env.update(GCV2_RUNTIME_LIB_DIR=str(library), GCV2_RUNTIME_OUTPUT_ROOT=str(evidence / 'publication'),
               LD_LIBRARY_PATH=str(library), GC_UNIT_OUT=str(evidence / 'standalone'))
    need('standalone-build-only', ['bash', source / 'runtime/tests/gc_unit/run_standalone.sh'], env)
    if 'GC_UNIT_BUILD_ONLY_DONE tests_executed=0' not in (evidence / 'standalone-build-only.log').read_text():
        raise ValueError('build-only branch not observed')
    elf = evidence / 'standalone/cj_gc_unit'
    artifacts = [elf, library / 'libcangjie-runtime.so', library / 'libboundscheck.so']
    identity('elf-at-link', artifacts)
    need('elf-headers', ['readelf', '-h', '-l', elf])
    need('elf-symbols', ['nm', '--defined-only', '-C', elf])
    need('elf-imports', ['nm', '-u', '-C', elf])
    symbol = [line.split()[0] for line in (evidence / 'elf-symbols.log').read_text().splitlines()
              if line.split(maxsplit=2)[-1] == 'MapleRuntime::GcUnit::CompleteTestRun(int)']
    if len(symbol) != 1:
        raise ValueError('completion symbol not unique')
    address = int(symbol[0], 16)
    need('completion-text', ['objdump', '-d', '--start-address='+str(address), '--stop-address='+str(address+128), elf])
    scripts = source / 'runtime/tests/gc_unit'
    def arm(label, runner, cut=False, legacy=False):
        identity(label+'-inputs', artifacts)
        directory = evidence / label
        rc = run(label, ['bash', runner, elf, library, directory], env)
        expected = 1 if cut else 0
        if rc != expected:
            raise RuntimeError(f'{label}: unexpected integration rc={rc}, expected={expected}; dependents NOT_RUN')
        result = verify(directory, cut=cut, legacy=legacy)
        (evidence / (label+'-results.json')).write_text(json.dumps(result, indent=2))
    if machine == 'x86_64':
        baseline = evidence / 'baseline-scripts'
        baseline.mkdir()
        for name in ('run_other_vm_teardown.sh', 'check_other_vm_teardown.py', 'check_teardown_exit.py'):
            content = subprocess.check_output(['git', '-C', source, 'show', BASE+':runtime/tests/gc_unit/'+name])
            (baseline / name).write_bytes(content)
        arm('baseline', baseline / 'run_other_vm_teardown.sh', legacy=True)
    arm('candidate', scripts / 'run_other_vm_teardown.sh')
    classifier = scripts / 'check_other_vm_teardown.py'
    original = classifier.read_bytes()
    before = original.decode()
    old = ' and state not in ("Z", "X")'
    if before.count(old) != 1:
        raise ValueError('classifier cut identity mismatch')
    after = before.replace(old, '')
    (evidence / 'cut.diff').write_text(''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
        fromfile='a/runtime/tests/gc_unit/check_other_vm_teardown.py', tofile='b/runtime/tests/gc_unit/check_other_vm_teardown.py')))
    classifier.write_text(after)
    try:
        arm('cut', scripts / 'run_other_vm_teardown.sh', cut=True)
    finally:
        classifier.write_bytes(original)
    if classifier.read_bytes() != original:
        raise ValueError('classifier restoration mismatch')
    arm('restored', scripts / 'run_other_vm_teardown.sh')
    need('source-restored', ['git', '-C', source, 'diff', '--exit-code'])
    if machine == 'x86_64':
        offline_controls(evidence / 'candidate', evidence)
    need('sccache-stats', ['sccache', '--show-stats'])
    need('uptime-after', ['uptime'])
    (evidence / 'QUALIFICATION_DONE').write_text('real three-process baseline/candidate/cut/restored records qualified\n')


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
