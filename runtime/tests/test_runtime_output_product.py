#!/usr/bin/env python3
"""Exercise configuration isolation through the actual runtime CMake entry.

Run on the build host. --work must be an isolated directory outside the source
checkout. The same recipe, toolchain path and build directory serve all arms;
only the selected input changes. Product builds preserve both SOs immediately.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--work', required=True, type=Path)
    parser.add_argument('--mode', choices=['toolchain', 'environment', 'initial-cache', 'arbitrary', 'override', 'indirect'], default='toolchain')
    parser.add_argument('--build', action='store_true')
    parser.add_argument('--jobs', default='16')
    args = parser.parse_args()
    if not args.build:
        parser.error('--build is required: published identity exists only after a product build')
    source, work = args.source.resolve(), args.work.resolve()
    if work == source or source in work.parents:
        parser.error('--work must be outside the source checkout')
    work.mkdir(parents=True, exist_ok=True)
    build = work / 'build'
    policy = work / 'policy.cmake'
    records = []
    unknown = 'PRODUCT_INPUT_' + os.urandom(8).hex()
    env = dict(os.environ, GC_UNIT_GATE_SKIP='1')

    def run(label, command, arm):
        log = arm / (label + '.log')
        with log.open('w') as output:
            rc = subprocess.run(command, cwd=source, env=env, stdout=output,
                                stderr=subprocess.STDOUT).returncode
        print(f'PRODUCT_LAYOUT_STEP arm={arm.name} step={label} rc={rc}', flush=True)
        return rc

    for name, value in [('off', '0'), ('on', '1'), ('restored', '0')]:
        arm = work / name
        arm.mkdir(exist_ok=True)
        if build.exists():
            shutil.rmtree(build)
        env['RUNTIME_LAYOUT_POLICY'] = value
        if args.mode == 'toolchain':
            policy.write_text(f'set(DISABLE_VERSION_CHECK {value})\n')
        elif args.mode in ('environment', 'override'):
            # File contents stay constant: hashing the toolchain alone cannot
            # distinguish the effective ordinary values in these arms.
            policy.write_text('set(DISABLE_VERSION_CHECK "$ENV{RUNTIME_LAYOUT_POLICY}")\n')
        elif args.mode == 'arbitrary':
            policy.write_text(f'set({unknown} "$ENV{{RUNTIME_LAYOUT_POLICY}}")\n'
                              f'add_compile_definitions({unknown}=${{{unknown}}})\n')
        elif args.mode == 'indirect':
            # A compiler-probe project has no runtime target. The policy belongs
            # to the real product graph, and is applied after its targets exist.
            policy.write_text('get_property(_layout_probe GLOBAL PROPERTY IN_TRY_COMPILE)\n'
                              'if(NOT _layout_probe)\n'
                              '  cmake_language(DEFER CALL set_property TARGET cangjie-runtime PROPERTY CUSTOM_POLICY "$ENV{RUNTIME_LAYOUT_POLICY}")\n'
                              '  add_compile_definitions(INDIRECT_POLICY=$<TARGET_PROPERTY:cangjie-runtime,CUSTOM_POLICY>)\n'
                              'endif()\n')
        else:
            policy.write_text(f'set(DISABLE_VERSION_CHECK {value} CACHE INTERNAL "" FORCE)\n')
        command = ['cmake', '-S', str(source), '-B', str(build), '-G', 'Unix Makefiles',
                   '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=clang',
                   '-DCMAKE_CXX_COMPILER=clang++', '-DCMAKE_ASM_COMPILER=clang',
                   '-DCJ_SDK_VERSION=0.0.1', '-DCOPYGC_FLAG=1']
        if args.mode == 'initial-cache':
            command += ['-C', str(policy)]
        elif args.mode == 'override':
            command += ['-DCMAKE_USER_MAKE_RULES_OVERRIDE=' + str(policy)]
        else:
            command += ['-DCMAKE_TOOLCHAIN_FILE=' + str(policy)]
        record = {'arm': name, 'value': value, 'command': command,
                  'input_mtime': policy.stat().st_mtime, 'toolchain_sha256': sha(policy)}
        rc = run('configure', command, arm)
        record['configure_rc'] = rc
        if rc:
            raise RuntimeError(f'{name}: configure failed; see {arm}/configure.log')
        build_command = ['cmake', '--build', str(build), '--target', 'publish_runtime_output', '--parallel', args.jobs]
        record['build_rc'] = run('build', build_command, arm)
        if record['build_rc']:
            raise RuntimeError(f'{name}: build failed; see {arm}/build.log')
        cache = (build / 'CMakeCache.txt').read_text()
        config = re.search(r'^CANGJIE_RUNTIME_CONFIG_ID:INTERNAL=(.*)$', cache, re.M)[1]
        root = source / 'output/temp' / config
        manifest = dict(line.split('=', 1) for line in (root / 'runtime-build-config.txt').read_text().splitlines())
        lib = Path(manifest['LIB_DIR'])
        record.update(id=config, root=str(root), lib_dir=str(lib), inputs_sha256=sha(root / 'runtime-build-inputs.txt'))
        for file in ['runtime-build-config.txt', 'runtime-build-inputs.txt', 'runtime-product-hashes.json']:
            shutil.copy2(root / file, arm / file)
        before = {str(path.relative_to(root)): sha(path) for path in root.rglob('*') if path.is_file()}
        record['repeat_build_rc'] = run('repeat-build', build_command, arm)
        assert record['repeat_build_rc'] == 0, 'repeat build failed'
        repeated_cache = (build / 'CMakeCache.txt').read_text()
        repeated_id = re.search(r'^CANGJIE_RUNTIME_CONFIG_ID:INTERNAL=(.*)$', repeated_cache, re.M)[1]
        repeated_inputs = source / 'output/temp' / repeated_id / 'runtime-build-inputs.txt'
        record.update(repeat_id=repeated_id, repeat_inputs_sha256=sha(repeated_inputs))
        shutil.copy2(repeated_inputs, arm / 'repeat-build-inputs.txt')
        after = {str(path.relative_to(root)): sha(path) for path in root.rglob('*') if path.is_file()}
        record['publication_before'] = before
        record['publication_after'] = after
        print(f'PRODUCT_IDEMPOTENCE_ASSERT arm={name} first={config} repeat={repeated_id}', flush=True)
        assert config == repeated_id, 'identical build inputs changed identity'
        assert before == after, 'repeat publication changed directory bytes'
        if args.mode == 'override' and name == 'off':
            published_so = lib / 'libcangjie-runtime.so'
            original_bytes = published_so.read_bytes()
            try:
                published_so.write_bytes(original_bytes + b'publication-collision-control')
                changed_sha = sha(published_so)
                record['collision_rc'] = run('publication-collision',
                    ['python3', str(source / 'build/publish_runtime_output.py'),
                     '@' + str(build / 'runtime-publish-args.txt')], arm)
                print(f'PRODUCT_COLLISION_ASSERT rc={record["collision_rc"]}', flush=True)
                assert record['collision_rc'] == 2, 'publication accepted different bytes for the same identity'
                assert 'identity collision' in (arm / 'publication-collision.log').read_text()
                assert sha(published_so) == changed_sha, 'collision replaced the existing publication'
            finally:
                published_so.write_bytes(original_bytes)
            record['collision_restored_rc'] = run('publication-collision-restored',
                ['python3', str(source / 'build/publish_runtime_output.py'),
                 '@' + str(build / 'runtime-publish-args.txt')], arm)
            assert record['collision_restored_rc'] == 0
        commands = json.loads((build / 'compile_commands.json').read_text())
        semantic = next(entry for entry in commands if entry['file'].endswith('/CjSemanticVersion.cpp'))
        record['product_compile_command'] = semantic
        (arm / 'compile-command.json').write_text(json.dumps(semantic, indent=2))
        compile_text = semantic.get('command', ' '.join(semantic.get('arguments', [])))
        if args.mode == 'indirect':
            assert f'-DINDIRECT_POLICY={value}' in compile_text, 'indirect property did not enter product compilation'
        elif args.mode == 'arbitrary':
            assert f'-D{unknown}={value}' in compile_text, 'arbitrary input did not enter product compilation'
        else:
            assert ('-DDISABLE_VERSION_CHECK' in compile_text) == (value == '1'), 'policy did not enter product compilation'
        print(f'PRODUCT_COMPILE_INPUT_ASSERT arm={name} value={value} id={config}', flush=True)
        if args.build:
            record['captured_at'] = time.time()
            for file in ['libcangjie-runtime.so', 'libboundscheck.so']:
                shutil.copy2(lib / file, arm / file)
                record[file] = sha(arm / file)
                assert record[file] == sha(lib / file)
            record['nm_rc'] = run('nm', ['nm', '--defined-only', str(arm / 'libcangjie-runtime.so')], arm)
            assert record['nm_rc'] == 0
            record['IsCorePackage'] = sum('IsCorePackage' in line for line in (arm / 'nm.log').read_text().splitlines())
            if args.mode not in ('arbitrary', 'indirect'):
                assert (record['IsCorePackage'] > 0) == (value == '0'), 'product symbol does not reflect version policy'
            print(f'PRODUCT_SYMBOL_ASSERT arm={name} IsCorePackage={record["IsCorePackage"]}', flush=True)
            stamp = subprocess.check_output(['strings', str(lib / 'libcangjie-runtime.so')], text=True)
            record['lineage'] = sorted(set(re.findall(r'CJRT-COMMIT:[^\s]+', stamp)))
            record['resolver_rc'] = run('resolver', ['bash', str(source / 'build/resolve_runtime_output.sh'), str(source), config], arm)
            assert record['resolver_rc'] == 0
            selected = Path((arm / 'resolver.log').read_text().strip())
            assert selected == lib.resolve(), 'resolver selected another directory'
            for file in ['libcangjie-runtime.so', 'libboundscheck.so']:
                assert sha(selected / file) == record[file], 'resolver selected another product'
        records.append(record)
        (work / 'records.json').write_text(json.dumps(records, indent=2))
        if name == 'on':
            print('PRODUCT_ISOLATION_ASSERT off=' + records[0]['id'] + ' on=' + config, flush=True)
            assert records[0]['id'] != config, 'different product configurations share an identity'
            if args.build:
                for file in ['libcangjie-runtime.so', 'libboundscheck.so']:
                    assert sha(Path(records[0]['lib_dir']) / file) == records[0][file], 'ON overwrote OFF product'
    print('PRODUCT_RESTORATION_ASSERT off=' + records[0]['id'] + ' restored=' + records[2]['id'], flush=True)
    assert records[0]['id'] == records[2]['id'], 'restoring input did not restore identity'
    if args.build:
        for file in ['libcangjie-runtime.so', 'libboundscheck.so']:
            assert records[0][file] == records[2][file], 'restored product bytes differ'
    print(f'PRODUCT_LAYOUT_OK mode={args.mode} build={args.build} N=3', flush=True)


if __name__ == '__main__':
    main()
