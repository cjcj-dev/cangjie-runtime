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
        old = 'set(CMAKE_C_COMPILER "$ENV{OHOS_PUBLIC_SDK}/llvm/bin/clang${EXECUTABLE_EXTENSION}")'
        new = 'set(CMAKE_C_COMPILER "$ENV{OHOS_ROUTE_CONTROL_SDK}/llvm/bin/clang${EXECUTABLE_EXTENSION}")'
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
    tracked = subprocess.check_output(['git', 'ls-files', '-z'], cwd=repo).decode().split('\0')
    for relative in filter(None, tracked):
        dest = source / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(repo / relative, dest)
    if arm == 'restored':
        path = source / 'runtime/CMakeLists.txt'
        original = path.read_bytes()
        mutate(source, 'producer-cut')
        path.write_bytes(original)
        assert path.read_bytes() == (repo / 'runtime/CMakeLists.txt').read_bytes()
    else:
        mutate(source, arm)
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
    with (out / (arm + '.log')).open('w') as log:
        log.write(json.dumps(command) + '\n'); log.flush()
        configured = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        observed = None
        if configured.returncode == 0:
            observed = subprocess.run([sys.executable, str(observer), '--build', str(build),
                                       '--sdk', str(sdk), '--arch', arch, '--configured-only',
                                       '--output', str(out / (arm + '.json'))],
                                      stdout=log, stderr=subprocess.STDOUT).returncode
    expected = []
    if arch == 'x86_64' and arm in ('producer-cut', 'consumer-cut'):
        expected = ['cjthread.sdk_route']
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
    (out / (arm + '-result.json')).write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record), flush=True)
    return record


subprocess.run(['uptime'], check=True)
with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
    results = list(pool.map(run, ['candidate', 'producer-cut', 'consumer-cut', 'rename-cut', 'restored']))
subprocess.run(['uptime'], check=True)
(out / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
raise SystemExit(0 if all(r['valid'] for r in results) else 1)
