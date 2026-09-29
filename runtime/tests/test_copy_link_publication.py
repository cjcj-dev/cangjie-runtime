#!/usr/bin/env python3
"""Exercise the real ARM product publisher and gc_unit link entry.

The cut removes only the internal object from the final test link. It must
restore the original undefined-reference signature, not fail provenance checks.
"""
import argparse
import hashlib
import difflib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=['default', 'testable', 'gcunit'], required=True)
    parser.add_argument('--arm', choices=['baseline', 'candidate', 'cut', 'restored'], required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    source = args.source.resolve()
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
    env.update(GCV2_RUNTIME_LIB_DIR=str(library), GCV2_RUNTIME_OUTPUT_ROOT=str(copied),
               LD_LIBRARY_PATH=str(library), GC_UNIT_OUT=str(evidence / 'standalone'))
    commands = json.loads((build / 'compile_commands.json').read_text())
    copy_commands = [x for x in commands if x['file'].endswith('/Copy_aarch64.S')]
    assert len(copy_commands) == 1
    (evidence / 'copy-compile-command.json').write_text(json.dumps(copy_commands, indent=2))
    if args.arm == 'baseline':
        assert args.profile != 'gcunit', 'baseline reproducer is the reported standalone entry'
        (evidence / 'products.sha256').write_text(''.join(
            hashlib.sha256((library / name).read_bytes()).hexdigest() + '  ' + name + '\n'
            for name in ('libcangjie-runtime.so', 'libboundscheck.so')))
        rc = run('standalone', ['bash', source / 'tests/gc_unit/run_standalone.sh'], env)
        log = (evidence / 'standalone.log').read_text()
        assert rc != 0 and 'MRT_CopyDisjointWords' in log and 'undefined reference' in log
        assert 'main_rc=1 publication_rc=0' in log
        assert not (evidence / 'standalone/cj_gc_unit').exists()
        run('uptime-after', ['uptime'])
        print('COPY_BASELINE_LINK_FAILURE_CONFIRMED assertions=NOT_RUN', flush=True)
        return
    inputs = json.loads((copied / 'runtime-build-inputs.txt').read_text())
    obj = copied / inputs['internal_test_objects']['copy_disjoint_words']['path']
    # Exercise the actual publisher with a changed object input. The SO pair
    # remains identical; the publication identity must change with object bytes.
    publish = ['python3', source / 'build/publish_runtime_output.py',
               '@' + str(build / 'runtime-publish-args.txt')]
    changed = evidence / 'changed-Copy.o'
    changed.write_bytes(obj.read_bytes() + b'\0')
    assert run('publish-changed-object', publish + ['--copy-object', changed], env) == 0
    changed_cache = (build / 'CMakeCache.txt').read_text()
    assert 'OUTPUT_TEMP_PATH:INTERNAL=' + str(root) + '\n' not in changed_cache
    assert run('publish-restored-object', publish, env) == 0
    assert 'OUTPUT_TEMP_PATH:INTERNAL=' + str(root) + '\n' in (build / 'CMakeCache.txt').read_text()
    changed.unlink()
    run('object-symbols', ['readelf', '-Ws', obj])
    archive = next((build / 'runtime-staging/ar').glob('*/libBase.a'))
    member = subprocess.check_output(['ar', 'p', str(archive), 'Copy_aarch64.S.o'])
    assert hashlib.sha256(member).hexdigest() == hashlib.sha256(obj.read_bytes()).hexdigest()
    helper = source / 'tests/gc_unit/product_test_configuration.py'
    validate = ['python3', helper, source, library, copied, '--copy-object', '--compiler', 'clang++']
    assert run('identity-normal-relocated', validate) == 0
    assert run('identity-resolve-relocated', ['python3', helper, source, library, copied, '--resolve-root']) == 0
    detached = evidence / 'detached-pair'
    detached.mkdir()
    for name in ('libcangjie-runtime.so', 'libboundscheck.so'):
        shutil.copy2(library / name, detached / name)
    assert run('identity-detached-ambiguous', ['python3', helper, source, detached, detached, '--resolve-root']) == 2
    assert run('identity-detached-explicit', ['python3', helper, source, detached, copied,
                                            '--copy-object', '--compiler', 'clang++']) == 0
    shutil.rmtree(detached)
    original_bytes = obj.read_bytes()
    obj.unlink()
    assert run('identity-missing', validate) == 2
    obj.write_bytes(original_bytes + b'changed')
    assert run('identity-hash', validate) == 2
    obj.write_bytes(original_bytes)
    assert run('identity-compiler-target', validate[:-1] + ['clang++ --target=x86_64-linux-gnu']) == 2
    recipe = copied / 'runtime-build-inputs.txt'
    inventory = copied / 'runtime-product-hashes.json'
    recipe_bytes, inventory_bytes = recipe.read_bytes(), inventory.read_bytes()
    wrong_elf = bytearray(original_bytes)
    wrong_elf[18:20] = bytes([62, 0])
    obj.write_bytes(wrong_elf)
    digest = hashlib.sha256(wrong_elf).hexdigest()
    inputs['internal_test_objects']['copy_disjoint_words']['sha256'] = digest
    recipe.write_text(json.dumps(inputs))
    hashes = json.loads(inventory.read_text())
    hashes[str(obj.relative_to(copied))] = digest
    inventory.write_text(json.dumps(hashes))
    assert run('identity-object-architecture', validate) == 2
    obj.write_bytes(original_bytes)
    recipe.write_bytes(recipe_bytes)
    inventory.write_bytes(inventory_bytes)
    assert run('identity-restored', validate) == 0
    products = [obj, library / 'libcangjie-runtime.so', library / 'libboundscheck.so']
    (evidence / 'products.sha256').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + str(p) + '\n' for p in products))
    env.update(GCV2_RUNTIME_LIB_DIR=str(library), GCV2_RUNTIME_OUTPUT_ROOT=str(copied),
               LD_LIBRARY_PATH=str(library), GC_UNIT_OUT=str(evidence / 'standalone'))
    if args.profile == 'gcunit':
        cmake = source / 'tests/gc_unit/CMakeLists.txt'
        original = cmake.read_text()
        if args.arm == 'candidate':
            # Reproduce the old CMake interface: it propagates Base even when
            # the explicit Copy object is removed. Preserve the native recipe
            # and full symbol table before testing the corrected boundary.
            inherited = original.replace(
                '"$<TARGET_FILE:cangjie-runtime>" "$<TARGET_FILE:boundscheck>" pthread dl',
                'cangjie-runtime').replace(
                'target_sources(cj_gc_unit PRIVATE $<TARGET_OBJECTS:RuntimeCopy>)',
                '# explicit Copy object removed from old interface control')
            cmake.write_text(inherited)
            try:
                assert run('cmake-old-interface', ['cmake', '--build', build, '--target',
                           'cj_gc_unit', '-j', os.cpu_count()], env) == 0
                old_link = (build / 'tests/gc_unit/CMakeFiles/cj_gc_unit.dir/link.txt').read_text()
                (evidence / 'cmake-old-interface-command.log').write_text(old_link)
                assert 'libBase.a' in old_link
                old_elf = build / 'runtime-staging/bin/aarch64_Release/cj_gc_unit'
                assert run('cmake-old-interface-symbols', ['nm', '--defined-only', old_elf]) == 0
                assert 'MRT_CopyDisjointWords' in (evidence / 'cmake-old-interface-symbols.log').read_text()
            finally:
                cmake.write_text(original)
        if args.arm == 'cut':
            cmake.write_text(original.replace('target_sources(cj_gc_unit PRIVATE $<TARGET_OBJECTS:RuntimeCopy>)', '# internal object deliberately disconnected'))
            (evidence / 'cut.diff').write_text(''.join(difflib.unified_diff(original.splitlines(True), cmake.read_text().splitlines(True), fromfile='a/runtime/tests/gc_unit/CMakeLists.txt', tofile='b/runtime/tests/gc_unit/CMakeLists.txt')))
        try:
            rc = run('cmake-link', ['cmake', '--build', build, '--target', 'cj_gc_unit', '-j', os.cpu_count()], env)
        finally:
            cmake.write_text(original)
        elf = build / 'runtime-staging/bin/aarch64_Release/cj_gc_unit'
        link_log = evidence / 'cmake-link.log'
        shutil.copy2(build / 'tests/gc_unit/CMakeFiles/cj_gc_unit.dir/link.txt', evidence / 'cmake-link-command.log')
        assert 'libBase.a' not in (evidence / 'cmake-link-command.log').read_text()
        if elf.exists():
            run('cmake-elf-symbols', ['nm', '--defined-only', elf])
    else:
        runner = source / 'tests/gc_unit/run_standalone.sh'
        original = runner.read_text()
        if args.arm == 'cut':
            runner.write_text(original.replace('"${MAIN_OBJECTS[@]}" "${COPY_OBJECTS[@]}"', '"${MAIN_OBJECTS[@]}"'))
            (evidence / 'cut.diff').write_text(''.join(difflib.unified_diff(original.splitlines(True), runner.read_text().splitlines(True), fromfile='a/runtime/tests/gc_unit/run_standalone.sh', tofile='b/runtime/tests/gc_unit/run_standalone.sh')))
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
        assert run('elf-symbols', ['nm', '--defined-only', elf]) == 0
        assert re.search(r' t MRT_CopyDisjointWords$', (evidence / 'elf-symbols.log').read_text(), re.M)
        assert re.search(r' T main$', (evidence / 'elf-symbols.log').read_text(), re.M)
        for test in ['atomic_copy_preserves_bounds_and_offset', 'atomic_copy_adjacent_ranges']:
            assert run(test, [elf, '--gtest_filter=ZUtils.' + test], env) == 0
        print('COPY_LINK_AND_CONTENT_CONFIRMED', flush=True)
    if args.profile == 'gcunit':
        # External CMake must consume the relocated publication with the
        # original producer build temporarily inaccessible.
        top = source / 'CMakeLists.txt'
        original_top = top.read_text()
        original_unit = cmake.read_text()
        if args.arm == 'cut':
            cmake.write_text(original_unit.replace('target_sources(cj_gc_unit PRIVATE "${_copy_object}")', '# published object deliberately disconnected'))
        hidden_build = evidence / 'producer-build-held'
        build.rename(hidden_build)
        external = evidence / 'external-build'
        try:
            top.write_text('\n'.join([
                'cmake_minimum_required(VERSION 3.19)',
                'project(ExternalGcUnit LANGUAGES C CXX)',
                f'set(GCV2_RUNTIME_LIB_DIR "{library}")',
                f'set(GCV2_RUNTIME_OUTPUT_ROOT "{copied}")',
                'include_directories("${CMAKE_SOURCE_DIR}/third_party/third_party_bounds_checking_function/include")',
                'include_directories("${CMAKE_SOURCE_DIR}/src/CJThread/src/runtime/schedule/include")',
                f'include_directories("{copied}/include")',
                'add_subdirectory(tests/gc_unit)', '']))
            assert run('external-configure', ['cmake', '-S', source, '-B', external,
                        '-DCMAKE_CXX_COMPILER=clang++'], env) == 0
            external_rc = run('external-link', ['cmake', '--build', external, '--target',
                              'cj_gc_unit', '-j', os.cpu_count()], env)
            if args.arm == 'cut':
                log = (evidence / 'external-link.log').read_text()
                assert external_rc != 0 and 'undefined reference' in log and 'MRT_CopyDisjointWords' in log
                assert not (external / 'tests/gc_unit/cj_gc_unit').exists()
                run('uptime-after', ['uptime'])
                return
            assert external_rc == 0
            external_elf = external / 'tests/gc_unit/cj_gc_unit'
            (evidence / 'external-elf.sha256').write_text(hashlib.sha256(external_elf.read_bytes()).hexdigest())
            for test in ['atomic_copy_preserves_bounds_and_offset', 'atomic_copy_adjacent_ranges']:
                assert run('external-' + test, [external_elf, '--gtest_filter=ZUtils.' + test], env) == 0
        finally:
            top.write_text(original_top)
            cmake.write_text(original_unit)
            hidden_build.rename(build)
    run('uptime-after', ['uptime'])


if __name__ == '__main__':
    main()
