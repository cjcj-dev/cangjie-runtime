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
    parser.add_argument('--mode', choices=['toolchain', 'environment', 'initial-cache', 'arbitrary'], default='toolchain')
    parser.add_argument('--build', action='store_true')
    parser.add_argument('--jobs', default='16')
    args = parser.parse_args()
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
        elif args.mode == 'environment':
            # File contents stay constant: hashing the toolchain alone cannot
            # distinguish the effective ordinary values in these arms.
            policy.write_text('set(DISABLE_VERSION_CHECK "$ENV{RUNTIME_LAYOUT_POLICY}")\n')
        elif args.mode == 'arbitrary':
            policy.write_text(f'set({unknown} "$ENV{{RUNTIME_LAYOUT_POLICY}}")\n'
                              f'add_compile_definitions({unknown}=${{{unknown}}})\n')
        else:
            policy.write_text(f'set(DISABLE_VERSION_CHECK {value} CACHE INTERNAL "" FORCE)\n')
        command = ['cmake', '-S', str(source), '-B', str(build), '-G', 'Unix Makefiles',
                   '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=clang',
                   '-DCMAKE_CXX_COMPILER=clang++', '-DCMAKE_ASM_COMPILER=clang',
                   '-DCJ_SDK_VERSION=0.0.1', '-DCOPYGC_FLAG=1']
        command += ['-C', str(policy)] if args.mode == 'initial-cache' else ['-DCMAKE_TOOLCHAIN_FILE=' + str(policy)]
        record = {'arm': name, 'value': value, 'command': command,
                  'input_mtime': policy.stat().st_mtime, 'toolchain_sha256': sha(policy)}
        rc = run('configure', command, arm)
        record['configure_rc'] = rc
        if rc:
            raise RuntimeError(f'{name}: configure failed; see {arm}/configure.log')
        cache = (build / 'CMakeCache.txt').read_text()
        config = re.search(r'^CANGJIE_RUNTIME_CONFIG_ID:INTERNAL=(.*)$', cache, re.M)[1]
        root = source / 'output/temp' / config
        manifest = dict(line.split('=', 1) for line in (root / 'runtime-build-config.txt').read_text().splitlines())
        lib = Path(manifest['LIB_DIR'])
        record.update(id=config, root=str(root), lib_dir=str(lib), inputs_sha256=sha(root / 'runtime-build-inputs.txt'))
        for file in ['runtime-build-config.txt', 'runtime-build-inputs.txt']:
            shutil.copy2(root / file, arm / file)
        record['repeat_configure_rc'] = run('repeat-configure', command, arm)
        assert record['repeat_configure_rc'] == 0, 'repeat configure failed'
        repeated_cache = (build / 'CMakeCache.txt').read_text()
        repeated_id = re.search(r'^CANGJIE_RUNTIME_CONFIG_ID:INTERNAL=(.*)$', repeated_cache, re.M)[1]
        repeated_inputs = source / 'output/temp' / repeated_id / 'runtime-build-inputs.txt'
        record.update(repeat_id=repeated_id, repeat_inputs_sha256=sha(repeated_inputs))
        shutil.copy2(repeated_inputs, arm / 'repeat-build-inputs.txt')
        print(f'PRODUCT_IDEMPOTENCE_ASSERT arm={name} first={config} repeat={repeated_id}', flush=True)
        assert config == repeated_id, 'identical configure inputs changed identity'
        assert record['inputs_sha256'] == record['repeat_inputs_sha256'], 'identical configure inputs changed snapshot'
        commands = json.loads((build / 'compile_commands.json').read_text())
        semantic = next(entry for entry in commands if entry['file'].endswith('/CjSemanticVersion.cpp'))
        record['product_compile_command'] = semantic
        (arm / 'compile-command.json').write_text(json.dumps(semantic, indent=2))
        compile_text = semantic.get('command', ' '.join(semantic.get('arguments', [])))
        if args.mode == 'arbitrary':
            assert f'-D{unknown}={value}' in compile_text, 'arbitrary input did not enter product compilation'
        else:
            assert ('-DDISABLE_VERSION_CHECK' in compile_text) == (value == '1'), 'policy did not enter product compilation'
        print(f'PRODUCT_COMPILE_INPUT_ASSERT arm={name} value={value} id={config}', flush=True)
        if args.build:
            record['build_rc'] = run('build', ['cmake', '--build', str(build), '--parallel', args.jobs], arm)
            if record['build_rc']:
                raise RuntimeError(f'{name}: build failed; see {arm}/build.log')
            record['captured_at'] = time.time()
            for file in ['libcangjie-runtime.so', 'libboundscheck.so']:
                shutil.copy2(lib / file, arm / file)
                record[file] = sha(arm / file)
                assert record[file] == sha(lib / file)
            record['nm_rc'] = run('nm', ['nm', '--defined-only', str(arm / 'libcangjie-runtime.so')], arm)
            assert record['nm_rc'] == 0
            record['IsCorePackage'] = sum('IsCorePackage' in line for line in (arm / 'nm.log').read_text().splitlines())
            if args.mode != 'arbitrary':
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
