#!/usr/bin/env python3
"""Cut actual OHOS configure edges and observe their generated compilation routes."""
import concurrent.futures
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

repo = Path.cwd()
arch = sys.argv[1]
sdk = Path(os.environ['PUBLIC_NATIVE'])
root = Path(os.environ['RUNNER_TEMP']) / ('ohos-controls-' + arch)
out = repo / 'ohos-evidence' / 'controls'
root.mkdir(parents=True, exist_ok=True)
out.mkdir(parents=True, exist_ok=True)
alternative = root / 'alternate-native'
shutil.copytree(sdk, alternative, symlinks=False)
base = '10965c352184f366d9e46fc75341905724bf40d5'
with (out / 'baseline-fetch.log').open('w') as log:
    subprocess.run(['git', 'fetch', '--no-tags', '--depth=1', 'origin', base],
                   cwd=repo, stdout=log, stderr=subprocess.STDOUT, check=True)
observer = repo / 'runtime/tests/test_ohos_public_sdk.py'
(out / 'observer.sha256').write_text(hashlib.sha256(observer.read_bytes()).hexdigest() + '\n')


def mutate(source, arm):
    if arm == 'producer-cut':
        path = source / 'runtime/CMakeLists.txt'
        before = path.read_text()
        old = 'execute_process(COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/build/build_cjthread.sh" -p ohos_x86_64_cangjie'
        new = 'execute_process(COMMAND "${CMAKE_COMMAND}" -E env "OHOS_PUBLIC_SDK=$ENV{OHOS_ROUTE_CONTROL_SDK}" bash "${CMAKE_CURRENT_SOURCE_DIR}/build/build_cjthread.sh" -p ohos_x86_64_cangjie'
    elif arm == 'consumer-cut':
        path = source / 'runtime/build/cmake/toolchain/ohos_x86_64_cangjie.cmake'
        before = path.read_text()
        old = 'set(CMAKE_CXX_COMPILER "$ENV{OHOS_PUBLIC_SDK}/llvm/bin/clang++${EXECUTABLE_EXTENSION}")'
        new = 'set(CMAKE_CXX_COMPILER "$ENV{OHOS_ROUTE_CONTROL_SDK}/llvm/bin/clang++${EXECUTABLE_EXTENSION}")'
    elif arm == 'buildpy-cut':
        path = source / 'runtime/build.py'
        before = path.read_text()
        old = 'os.path.abspath(args.ohos_public_sdk) if args.ohos_public_sdk else ""'
        new = 'os.environ["OHOS_ROUTE_CONTROL_SDK"] if args.target == "ohos-x86_64" else (' + old + ')'
    elif arm == 'strip-cut':
        path = source / 'runtime/CMakeLists.txt'
        before = path.read_text()
        old = 'set(STRIP_PROGRAM "${OHOS_PUBLIC_SDK}/llvm/bin/llvm-strip")'
        new = old + '\n            if (OHOS_FLAG MATCHES 2)\n                set(STRIP_PROGRAM "$ENV{OHOS_ROUTE_CONTROL_SDK}/llvm/bin/llvm-strip")\n            endif()'
    elif arm == 'rename-cut':
        path = source / 'runtime/config.cmake'
        before = path.read_text()
        old = '    set(OHOS_LIB "${_ohos_ndk_lib}")'
        new = old + '\n    if (OHOS_FLAG MATCHES 2)\n        configure_file("${_ohos_llvm_lib}/libc++.so" "${CMAKE_BINARY_DIR}/libstdc++.so" COPYONLY)\n    endif()'
    else:
        return
    if before.count(old) != 1:
        raise RuntimeError('Cut anchor changed: ' + arm)
    after = before.replace(old, new)
    path.write_text(after)
    relative = path.relative_to(source).as_posix()
    (out / (arm + '.diff')).write_text(''.join(difflib.unified_diff(
        before.splitlines(True), after.splitlines(True), 'a/' + relative, 'b/' + relative)))


def run(arm):
    started = time.monotonic()
    source = root / arm
    # Only tracked source is copied; no generated product or sibling arm is reused.
    source.mkdir()
    if arm == 'baseline':
        archive = subprocess.Popen(['git', 'archive', base], cwd=repo, stdout=subprocess.PIPE)
        unpacked = subprocess.run(['tar', '-x', '-C', str(source)], stdin=archive.stdout)
        archive.stdout.close()
        if archive.wait() or unpacked.returncode:
            raise RuntimeError('Baseline archive failed')
    else:
        tracked = subprocess.check_output(['git', 'ls-files', '-z'], cwd=repo).decode().split('\0')
        for relative in filter(None, tracked):
            dest = source / relative
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(repo / relative, dest)
    if arm in ('restored', 'buildpy-restored'):
        path = source / ('runtime/build.py' if arm == 'buildpy-restored' else 'runtime/CMakeLists.txt')
        original = path.read_bytes()
        mutate(source, 'buildpy-cut' if arm == 'buildpy-restored' else 'producer-cut')
        path.write_bytes(original)
        assert path.read_bytes() == (repo / path.relative_to(source)).read_bytes()
    else:
        mutate(source, arm)
    identities = {}
    for relative in ('runtime/CMakeLists.txt', 'runtime/config.cmake', 'runtime/build.py',
                     'runtime/build/cmake/toolchain/ohos_x86_64_cangjie.cmake',
                     'runtime/build/cmake/toolchain/ohos_aarch64_cangjie.cmake',
                     'runtime/src/CJThread/CMakeLists.txt'):
        identities[relative] = hashlib.sha256((source / relative).read_bytes()).hexdigest()
    (out / (arm + '-source.json')).write_text(json.dumps(identities, indent=2) + '\n')
    env = os.environ.copy()
    env.pop('OHOS_ROOT', None)
    env.pop('OHOS_PUBLIC_SDK', None)
    env['OHOS_ROUTE_CONTROL_SDK'] = str(alternative)
    env['CMAKE_EXPORT_COMPILE_COMMANDS'] = 'ON'
    build = source / 'configured'
    command = ['cmake', '-S', str(source / 'runtime'), '-B', str(build),
               '-DCMAKE_BUILD_TYPE=Release', '-DOHOS_FLAG=' + ('2' if arch == 'x86_64' else '1'),
               '-DOHOS_PUBLIC_SDK=' + str(sdk), '-DCMAKE_INSTALL_PREFIX=' + str(source / 'install'),
               '-DCOPYGC_FLAG=1', '-DDOPRA_FLAG=1', '-DRUNTIME_TRACE_FLAG=0',
               '-DCJ_SDK_VERSION=0.0.1', '-DDISABLE_VERSION_CHECK=1']
    if arch == 'aarch64':
        command += ['-DRUNTIME_FORWARD_PTRAUTH_CFI=1', '-DRUNTIME_BACKWARD_PTRAUTH_CFI=1']
    if arm.startswith('buildpy-'):
        command = [sys.executable, str(source / 'runtime/build.py'), 'build', '-t', 'release',
                   '--target', 'ohos-' + arch, '--ohos-public-sdk', str(sdk), '-v', '0.0.1']
        build = source / 'runtime/CMakebuild'
    with (out / (arm + '.log')).open('w') as log:
        log.write(json.dumps(command) + '\n'); log.flush()
        configured = subprocess.run(command, cwd=source / 'runtime', env=env, stdout=log, stderr=subprocess.STDOUT)
        observed = None
        if configured.returncode == 0:
            observed = subprocess.run([sys.executable, str(observer), '--build', str(build),
                                       '--sdk', str(sdk), '--arch', arch,
                                       '--output', str(out / (arm + '.json'))]
                                      + ([] if arm.startswith('buildpy-') else ['--configured-only']),
                                      stdout=log, stderr=subprocess.STDOUT).returncode
    expected = []
    if arm == 'baseline' and arch == 'aarch64':
        expected = ['cjthread.sdk_route', 'runtime.strip_route', 'no_runtime_library_rename']
    if arch == 'x86_64' and arm in ('producer-cut', 'consumer-cut'):
        expected = ['cjthread.sdk_route']
    elif arch == 'x86_64' and arm == 'buildpy-cut':
        expected = ['runtime.sdk_route', 'cjthread.sdk_route', 'runtime.strip_route']
    elif arch == 'x86_64' and arm == 'strip-cut':
        expected = ['runtime.strip_route']
    elif arch == 'x86_64' and arm == 'rename-cut':
        expected = ['no_runtime_library_rename']
    failures = None
    observation = out / (arm + '.json')
    if observation.exists():
        failures = [c['name'] for c in json.loads(observation.read_text())['checks'] if not c['pass']]
    record = {'arm': arm, 'arch': arch, 'configure_rc': configured.returncode,
              'observer_rc': observed, 'failures': failures, 'expected_failures': expected,
              'wall': round(time.monotonic() - started, 2)}
    record['valid'] = (configured.returncode == 0 and failures == expected
                       and observed == (1 if expected else 0))
    if arm == 'baseline' and arch == 'x86_64':
        record['valid'] = configured.returncode != 0
        record['note'] = 'Original configure failure reproduced; not counted as assertion-level red evidence.'
    (out / (arm + '-result.json')).write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record), flush=True)
    return record


subprocess.run(['uptime'], check=True)
with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
    results = list(pool.map(run, ['baseline', 'candidate', 'producer-cut', 'consumer-cut', 'rename-cut', 'strip-cut', 'restored', 'buildpy-cut', 'buildpy-restored']))
subprocess.run(['uptime'], check=True)
(out / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
raise SystemExit(0 if all(r['valid'] for r in results) else 1)
