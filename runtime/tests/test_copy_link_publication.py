#!/usr/bin/env python3
"""Exercise the real ARM product publisher and gc_unit link entry.

The cut removes only the internal object from the final test link. It must
restore the original undefined-reference signature, not fail provenance checks.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=['default', 'testable', 'gcunit'], required=True)
    parser.add_argument('--arm', choices=['candidate', 'cut', 'restored'], required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    build = evidence / 'build'
    records = []
    def run(name, command, env=None):
        start = time.monotonic()
        with (evidence / (name + '.log')).open('w') as log:
            log.write('COMMAND=' + repr([str(x) for x in command]) + '\n')
            log.flush()
            rc = subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, env=env).returncode
        records.append({'name': name, 'rc': rc, 'wall': time.monotonic() - start})
        (evidence / 'results.json').write_text(json.dumps(records, indent=2))
        print(f'{name}: rc={rc} wall={records[-1]["wall"]:.1f}', flush=True)
        return rc
    run('identity', ['git', '-C', source, 'rev-parse', 'HEAD'])
    run('uptime-before', ['uptime'])
    env = dict(os.environ, GC_UNIT_GATE_SKIP='1', GC_UNIT_JOBS=str(os.cpu_count()),
               CANGJIE_BUILD_JOBS=str(os.cpu_count()), CCACHE_DIR=str(evidence / 'ccache'),
               CCACHE_BASEDIR=str(source), CCACHE_NOHASHDIR='1')
    maps = ' '.join(f'-f{kind}-prefix-map={source}=/usr/src/cangjie-runtime'
                    for kind in ('file', 'debug', 'macro'))
    env.update(CFLAGS=maps, CXXFLAGS=maps, ASMFLAGS=maps)
    command = ['cmake', '-S', source, '-B', build, '-DCMAKE_BUILD_TYPE=Release',
               '-DCOPYGC_FLAG=1', '-DDOPRA_FLAG=1', '-DRUNTIME_TRACE_FLAG=1',
               '-DCJ_SDK_VERSION=0.0.1', '-DDISABLE_VERSION_CHECK=1',
               '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++', '-DCMAKE_AR_PATH=ar',
               '-DCMAKE_C_COMPILER_LAUNCHER=ccache', '-DCMAKE_CXX_COMPILER_LAUNCHER=ccache',
               '-DCMAKE_ASM_COMPILER_LAUNCHER=ccache',
               '-DMRT_TESTABLE_INTERNALS=' + ('OFF' if args.profile == 'default' else 'ON'),
               '-DMRT_GC_UNIT_TESTS=' + ('ON' if args.profile == 'gcunit' else 'OFF')]
    assert run('configure', command, env) == 0
    assert run('build', ['cmake', '--build', build, '--target', 'cangjie-runtime', '-j', os.cpu_count()], env) == 0
    cache = dict(line.split('=', 1) for line in (build / 'CMakeCache.txt').read_text().splitlines()
                 if '=' in line and not line.startswith('//'))
    root = Path(cache['OUTPUT_TEMP_PATH:INTERNAL'])
    copied = evidence / 'publication'
    shutil.copytree(root, copied)
    library = next(copied.glob('lib/*/libcangjie-runtime.so')).parent
    inputs = json.loads((copied / 'runtime-build-inputs.txt').read_text())
    obj = copied / inputs['internal_test_objects']['copy_disjoint_words']['path']
    commands = json.loads((build / 'compile_commands.json').read_text())
    assert len([x for x in commands if x['file'].endswith('/Copy_aarch64.S')]) == 1
    run('object-symbols', ['readelf', '-Ws', obj])
    products = [obj, library / 'libcangjie-runtime.so', library / 'libboundscheck.so']
    (evidence / 'products.sha256').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + str(p) + '\n' for p in products))
    env.update(GCV2_RUNTIME_LIB_DIR=str(library), GCV2_RUNTIME_OUTPUT_ROOT=str(copied),
               LD_LIBRARY_PATH=str(library), GC_UNIT_OUT=str(evidence / 'standalone'))
    if args.profile == 'gcunit':
        cmake = source / 'tests/gc_unit/CMakeLists.txt'
        original = cmake.read_text()
        if args.arm == 'cut':
            cmake.write_text(original.replace('target_sources(cj_gc_unit PRIVATE $<TARGET_OBJECTS:RuntimeCopy>)', '# internal object deliberately disconnected'))
        try:
            rc = run('cmake-link', ['cmake', '--build', build, '--target', 'cj_gc_unit', '-j', os.cpu_count()], env)
        finally:
            cmake.write_text(original)
        elf = build / 'tests/gc_unit/cj_gc_unit'
        link_log = evidence / 'cmake-link.log'
    else:
        runner = source / 'tests/gc_unit/run_standalone.sh'
        original = runner.read_text()
        if args.arm == 'cut':
            runner.write_text(original.replace('"${MAIN_OBJECTS[@]}" "${COPY_OBJECTS[@]}"', '"${MAIN_OBJECTS[@]}"'))
        try:
            rc = run('standalone', ['bash', runner], env)
        finally:
            runner.write_text(original)
        elf = evidence / 'standalone/cj_gc_unit'
        link_log = evidence / 'standalone.log'
    if args.arm == 'cut':
        log = link_log.read_text()
        assert rc != 0 and 'undefined reference' in log and 'MRT_CopyDisjointWords' in log
        if args.profile != 'gcunit':
            assert 'main_rc=1 publication_rc=0' in log
        assert not elf.exists(), 'cut must not execute a stale ELF'
        print('COPY_LINK_CUT_SIGNATURE_CONFIRMED assertions=NOT_RUN', flush=True)
    else:
        assert elf.is_file(), 'real entry did not link its main ELF'
        (evidence / 'test-elf.sha256').write_text(hashlib.sha256(elf.read_bytes()).hexdigest() + '\n')
        run('elf-symbols', ['nm', '--defined-only', elf])
        for test in ['atomic_copy_preserves_bounds_and_offset', 'atomic_copy_adjacent_ranges']:
            assert run(test, [elf, '--gtest_filter=ZUtils.' + test], env) == 0
        print('COPY_LINK_AND_CONTENT_CONFIRMED', flush=True)
    run('uptime-after', ['uptime'])


if __name__ == '__main__':
    main()
