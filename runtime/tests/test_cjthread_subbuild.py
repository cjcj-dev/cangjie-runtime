#!/usr/bin/env python3
"""Check CJThread handoff through the real runtime CMake entry on a build host.

Each invocation needs a fresh --work outside --source (the runtime directory).
The failure case copies that source before adding a failing real build target.
--prebuilt uses a complete staging tree produced by a previous successful case.
This checks configure-time header/library production, not runtime execution.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--work', required=True, type=Path)
    parser.add_argument('--cwd', choices=['source', 'outside'], default='outside')
    parser.add_argument('--ohos-host', action='store_true')
    parser.add_argument('--custom-output', action='store_true')
    parser.add_argument('--prebuilt', type=Path)
    parser.add_argument('--fail-child', choices=['build', 'install'])
    args = parser.parse_args()
    source, work = args.source.resolve(), args.work.resolve()
    if work == source or source in work.parents:
        parser.error('--work must be outside the source directory')
    if args.prebuilt and args.fail_child:
        parser.error('prebuilt and failure cases must be separate')
    work.mkdir(parents=True, exist_ok=False)
    if args.fail_child:
        isolated = work / 'source'
        shutil.copytree(source, isolated, ignore=shutil.ignore_patterns(
            'output', 'build-*', 'build_standalone', '__pycache__'))
        source = isolated
    build = work / 'build'
    staging = build / 'runtime-staging'
    if args.prebuilt:
        shutil.copytree(args.prebuilt.resolve(), staging)
    before = {str(p.relative_to(staging)): sha(p)
              for p in staging.rglob('*') if p.is_file()}
    cj_cmake = source / 'src/CJThread/CMakeLists.txt'
    original = cj_cmake.read_bytes()
    if args.fail_child == 'build':
        injection = ('\nadd_custom_target(cjthread_failure ALL\n'
                     '  COMMAND ${CMAKE_COMMAND} -E echo CJTHREAD_INJECTED_BUILD_FAILURE\n'
                     '  COMMAND ${CMAKE_COMMAND} -E false VERBATIM)\n')
    elif args.fail_child == 'install':
        injection = ('\ninstall(CODE "message(FATAL_ERROR '
                     'CJTHREAD_INJECTED_INSTALL_FAILURE)")\n')
    else:
        injection = ''
    command = ['cmake', '-S', str(source), '-B', str(build),
               '-G', 'Unix Makefiles', '-DCMAKE_BUILD_TYPE=Release',
               '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++',
               '-DCMAKE_AR_PATH=ar', '-DCOPYGC_FLAG=1', '-DDOPRA_FLAG=1',
               '-DRUNTIME_TRACE_FLAG=1', '-DCJ_SDK_VERSION=0.0.1',
               '-DDISABLE_VERSION_CHECK=1',
               '-DCMAKE_C_COMPILER_LAUNCHER=ccache',
               '-DCMAKE_CXX_COMPILER_LAUNCHER=ccache',
               '-DCMAKE_ASM_COMPILER_LAUNCHER=ccache',
               '-DCMAKE_INSTALL_PREFIX=' + str(work / 'install'),
               '--trace-expand', '--trace-source=' + str(source / 'CMakeLists.txt')]
    if args.ohos_host:
        command += ['-DMRT_GC_UNIT_OHOS_HOST=ON', '-DMRT_TESTABLE_INTERNALS=ON']
    if args.custom_output:
        command += ['-DCMAKE_OUTPUT_DIRECTORY=' + str(work / 'requested-output')]
    if args.prebuilt:
        command += ['-DBUILD_CJTHREAD=OFF']
    cwd = source if args.cwd == 'source' else work
    env = dict(os.environ, CJTHREAD_BUILD_PATH=str(work / 'parent-path'),
               GC_UNIT_GATE_SKIP='1', CCACHE_DIR='/root/.ccache',
               CCACHE_BASEDIR=str(source.parent), CCACHE_NOHASHDIR='1')
    identity = {str(p.relative_to(source)): sha(p) for p in
                (source / 'CMakeLists.txt', source / 'build/build_cjthread.sh', cj_cmake)}
    record = {'command': command, 'cwd': str(cwd), 'source_identity': identity,
              'uptime_before': subprocess.check_output(['uptime'], text=True).strip(),
              'build_script': str(source / 'build/build_cjthread.sh'),
              'staging': str(staging), 'checks': {}}
    try:
        if injection:
            cj_cmake.write_bytes(original + injection.encode())
            record['injected_sha256'] = sha(cj_cmake)
            (work / 'injection.cmake').write_text(injection)
        start = time.monotonic()
        with (work / 'configure.log').open('w') as log:
            rc = subprocess.run(command, cwd=cwd, env=env, stdout=log,
                                stderr=subprocess.STDOUT).returncode
        record.update(configure_rc=rc, wall=time.monotonic() - start)
    finally:
        if injection:
            cj_cmake.write_bytes(original)
        record['restored_sha256'] = sha(cj_cmake)
    log = (work / 'configure.log').read_text()
    # Trace lines contain message arguments even when testing a restored older
    # caller. Match actual emitted lines, rather than mere source text.
    lines = log.splitlines()
    started = any(line.startswith('CJTHREAD BUILDING:') for line in lines)
    done = 'build cjthread done!!!' in lines
    checks = record['checks']
    checks['child_started'] = started == (not args.prebuilt)
    if args.fail_child:
        checks['child_failure_reached'] = f'CJTHREAD_INJECTED_{args.fail_child.upper()}_FAILURE' in log
        checks['failure_propagates'] = rc != 0 and 'CJThread build failed: 2' in log
        checks['no_false_success'] = not done
        checks['runtime_generation_stopped'] = not (build / 'Makefile').exists()
        restore = f'set(ENV{{CJTHREAD_BUILD_PATH}} {env["CJTHREAD_BUILD_PATH"]} )'
        checks['environment_restored_before_error'] = (
            restore in log and log.find('message(FATAL_ERROR CJThread build failed:') > log.index(restore))
        if args.fail_child == 'build':
            checks['install_not_started'] = not (build / 'cjthread-build/install_manifest.txt').exists()
    else:
        checks['configure_success'] = rc == 0
        checks['success_message'] = done == (not args.prebuilt)
        for name in ['include/schedule.h', 'include/cjthread_context.h',
                     'include/schedule_rename.h', 'lib/libcangjie-thread.a',
                     'lib/libcangjie-aio.a']:
            checks['staging_' + name] = (staging / name).is_file()
        checks['header_from_source'] = ((staging / 'include/schedule.h').is_file() and
            sha(staging / 'include/schedule.h') == sha(source / 'src/CJThread/src/runtime/schedule/include/schedule.h'))
        generated = build / 'runtime-generated-inputs/cangjie-runtime-CXX.txt'
        checks['runtime_consumes_staging'] = generated.is_file() and str(staging / 'include') in generated.read_text()
        if args.prebuilt:
            after = {str(p.relative_to(staging)): sha(p) for p in staging.rglob('*') if p.is_file()}
            checks['prebuilt_preserved'] = before == after
    record['artifacts'] = {str(p): sha(p) for p in staging.rglob('*') if p.is_file()}
    record['uptime_after'] = subprocess.check_output(['uptime'], text=True).strip()
    (work / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    for name, ok in checks.items():
        print(f'CJTHREAD_ASSERT name={name} result={"PASS" if ok else "FAIL"}', flush=True)
    return 0 if all(checks.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
