#!/usr/bin/env python3
"""Check the Windows CJThread install handoff on a Linux build host.

1. Run the real build_cjthread_windows.bat (copied into --work) under wine's
   cmd.exe with argv-recording cmake/mingw32-make stubs: both target branches
   must forward the install prefix (argument 7) and target arch (argument 8),
   run a checked install, and never pass empty arguments.
2. Evaluate the real runtime/CMakeLists.txt build_cjthread_windows.bat call
   sites against a recorder: arguments 6/7/8 keep their positions.
3. Replay the recorded child configure on a CJThread-shaped child using the
   real install rules, install it through the real parent install(SCRIPT),
   and apply the native_runtime.mjs cache/manifest checks.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

STUB = r'''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    const char *name = strstr(argv[0], "mingw32-make") ? "MAKE" : "CMAKE";
    FILE *f = fopen(getenv("CJ_STUB_LOG"), "a");
    int install = 0;
    fprintf(f, "%s", name);
    for (int i = 1; i < argc; i++) {
        fprintf(f, "\x1f%s", argv[i]);
        if (strcmp(argv[i], "install") == 0) install = 1;
    }
    fprintf(f, "\n");
    fclose(f);
    const char *rc = getenv("CJ_STUB_INSTALL_RC");
    return install && rc ? atoi(rc) : 0;
}
'''
PREFIX_LINUX = 'install root'
TUPLE = 'windows_x86_64_cjnative'


def win(path):
    return 'Z:' + str(path).replace('/', '\\')


def linux(value):
    return re.sub(r'^[Zz]:', '', value).replace('\\', '/')


def run(command, log, **kwargs):
    with log.open('w') as out:
        out.write(repr([str(x) for x in command]) + '\n')
        out.flush()
        return subprocess.run([str(x) for x in command], stdout=out,
                              stderr=subprocess.STDOUT, timeout=600, **kwargs).returncode


def bat_case(work, stubs, bat, name, args, install_rc=None):
    case = work / ('bat-' + name)
    case.mkdir()
    log = case / 'argv.log'
    log.touch()
    env = dict(os.environ, WINEDEBUG='-all', WINEPATH=win(stubs), CJ_STUB_LOG=win(log),
               CJTHREAD_BUILD_PATH=win(case / 'cjthread-build'), CANGJIE_BUILD_JOBS='3')
    if install_rc is not None:
        env['CJ_STUB_INSTALL_RC'] = str(install_rc)
    rc = run(['wine', 'cmd', '/c', win(bat)] + args, case / 'wine.log', env=env, cwd=case)
    calls = [line.split('\x1f') for line in log.read_text().splitlines()]
    return rc, calls


def check_bat(work, source, record):
    checks = record['checks']
    tree = work / 'bat-tree/runtime/build'
    tree.mkdir(parents=True)
    bat = tree / 'build_cjthread_windows.bat'
    shutil.copyfile(source / 'build/build_cjthread_windows.bat', bat)
    stubs = work / 'stubs'
    stubs.mkdir()
    (stubs / 'stub.c').write_text(STUB)
    subprocess.run(['x86_64-w64-mingw32-gcc', '-O2', '-o', stubs / 'cmake.exe', stubs / 'stub.c'], check=True)
    shutil.copyfile(stubs / 'cmake.exe', stubs / 'mingw32-make.exe')
    prefix = win(work / PREFIX_LINUX).replace('\\', '/')
    other = ' -DRUNTIME_OUTPUT_ROOT=Z:/cj/output -DCJ_HANDOFF_EXTRA=1'
    record['bat_prefix'] = prefix
    for target in ('windows_x86_64', 'ohos_aarch64_cangjie'):
        rc, calls = bat_case(work, stubs, bat, target,
                             ['-p', target, 'Release', 'STATIC', 'cj', other, prefix, TUPLE])
        record['bat_' + target] = {'rc': rc, 'calls': calls}
        cmake = [c[1:] for c in calls if c[0] == 'CMAKE']
        make = [c[1:] for c in calls if c[0] == 'MAKE']
        argv = cmake[0] if len(cmake) == 1 else []
        checks[f'{target}_rc0'] = rc == 0
        checks[f'{target}_prefix_forwarded'] = argv.count('-DCMAKE_INSTALL_PREFIX=' + prefix) == 1
        checks[f'{target}_target_arch_forwarded'] = argv.count('-DTARGET_ARCH=' + TUPLE) == 1
        checks[f'{target}_build_type_forwarded'] = '-DCMAKE_BUILD_TYPE=Release' in argv
        checks[f'{target}_definitions_split'] = all(a in argv for a in other.split())
        checks[f'{target}_no_empty_argument'] = bool(argv) and '' not in argv
        checks[f'{target}_build_then_install'] = make == [['-j3'], ['install', '-j3']]
    rc, calls = bat_case(work, stubs, bat, 'direct_cli', ['-p', 'windows_x86_64', 'Release', 'STATIC'])
    record['bat_direct_cli'] = {'rc': rc, 'calls': calls}
    argv = next((c[1:] for c in calls if c[0] == 'CMAKE'), [])
    checks['direct_cli_rc0'] = rc == 0
    checks['direct_cli_no_prefix_or_arch'] = bool(argv) and not any(
        a.startswith(('-DCMAKE_INSTALL_PREFIX', '-DTARGET_ARCH')) for a in argv)
    checks['direct_cli_no_empty_argument'] = bool(argv) and '' not in argv
    checks['direct_cli_skips_install'] = [c[1:] for c in calls if c[0] == 'MAKE'] == [['-j3']]
    rc, calls = bat_case(work, stubs, bat, 'install_fails',
                         ['-p', 'windows_x86_64', 'Release', 'STATIC', 'cj', other, prefix, TUPLE], 41)
    record['bat_install_fails'] = {'rc': rc, 'calls': calls}
    checks['install_failure_propagates'] = rc == 41 and calls and calls[-1][1:2] == ['install']
    return next(c[1:] for c in record['bat_windows_x86_64']['calls'] if c[0] == 'CMAKE')


def check_call_sites(work, source, record):
    checks = record['checks']
    text = (source / 'CMakeLists.txt').read_text(encoding='utf-8')
    sites = re.findall(r'execute_process\(COMMAND "\$\{CMAKE_CURRENT_SOURCE_DIR\}/build/'
                       r'build_cjthread_windows\.bat".*?\)', text, re.S)
    checks['call_sites_found'] = len(sites) == 4
    fake = work / 'call-sites'
    (fake / 'build').mkdir(parents=True)
    recorder = fake / 'build/build_cjthread_windows.bat'
    recorder.write_text('#!/bin/sh\nfor a in "$@"; do printf "%s\\037" "$a"; done >> "$CJ_SITE_LOG"\n'
                        'printf "\\n" >> "$CJ_SITE_LOG"\n')
    recorder.chmod(0o755)
    cases = {'normal': (' -DRUNTIME_OUTPUT_ROOT=/cj/out -DA=1', '/cj/install root', TUPLE),
             'empty_target_arch': (' -DRUNTIME_OUTPUT_ROOT=/cj/out', '/cj/install', ''),
             'empty_definitions': ('', '/cj/install', TUPLE)}
    for case, (other, prefix, arch) in cases.items():
        log = fake / f'{case}.log'
        script = fake / f'{case}.cmake'
        script.write_text(f'set(CMAKE_CURRENT_SOURCE_DIR "{fake}")\nset(CMAKE_BUILD_TYPE Release)\n'
                          f'set(CJTHREAD_SAN_PRARM cj)\nset(OTHER_DEFINITIONS "{other}")\n'
                          f'set(CMAKE_INSTALL_PREFIX "{prefix}")\nset(TARGET_ARCH "{arch}")\n'
                          + '\n'.join(sites) + '\n')
        rc = subprocess.run(['cmake', '-P', script], env=dict(os.environ, CJ_SITE_LOG=str(log))).returncode
        rows = [line.split('\x1f')[:-1] for line in log.read_text().splitlines()] if log.exists() else []
        record['call_site_' + case] = {'rc': rc, 'argv': rows}
        checks[f'call_sites_{case}'] = rc == 0 and len(rows) == len(sites) and all(
            len(r) == 8 and r[5:8] == [other, prefix, arch] for r in rows)


def check_install(work, source, argv, record):
    checks = record['checks']
    defs = {a.split('=', 1)[0][2:]: a.split('=', 1)[1] for a in argv if a.startswith('-D') and '=' in a}
    prefix = Path(linux(defs['CMAKE_INSTALL_PREFIX'])) if 'CMAKE_INSTALL_PREFIX' in defs else None
    rules = (source / 'src/CJThread/src/CMakeLists.txt').read_text(encoding='utf-8')
    installs = [re.search(rf'^install\(TARGETS {n} ARCHIVE DESTINATION [^)]*\)$', rules, re.M)
                for n in ('cangjie-thread', 'cangjie-aio')]
    parent_rule = re.search(r'^if \(BUILD_CJTHREAD OR EXISTS .*?^endif\(\)$',
                            (source / 'CMakeLists.txt').read_text(encoding='utf-8'), re.S | re.M)
    checks['real_install_rules_found'] = all(installs) and bool(parent_rule)
    if not checks['real_install_rules_found']:
        return
    child, parent = work / 'child-src', work / 'parent-src'
    child.mkdir()
    parent.mkdir()
    (child / 'probe.c').write_text('int cj_handoff_probe(void) { return 0; }\n')
    (child / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.16)\nproject(CJThreadHandoff C)\n'
        'add_library(cangjie-thread STATIC probe.c)\nadd_library(cangjie-aio STATIC probe.c)\n'
        + '\n'.join(m.group(0) for m in installs) + '\n')
    (parent / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.16)\nproject(RuntimeHandoff NONE)\nset(BUILD_CJTHREAD ON)\n'
        + parent_rule.group(0) + '\n')
    build = work / 'parent-build'
    child_build = build / 'cjthread-build'
    keep = [f'-D{k}={linux(v) if k == "CMAKE_INSTALL_PREFIX" else v}' for k, v in defs.items()
            if k in ('CMAKE_INSTALL_PREFIX', 'TARGET_ARCH', 'CMAKE_BUILD_TYPE')]
    rc = run(['cmake', '-S', child, '-B', child_build] + keep, work / 'child-configure.log')
    rc = rc or run(['cmake', '--build', child_build], work / 'child-build.log')
    cache = lambda d: dict(re.findall(r'^(\w+):[^=]*=(.*)$', (d / 'CMakeCache.txt').read_text(), re.M))
    child_cache = cache(child_build) if rc == 0 else {}
    in_work = Path(child_cache.get('CMAKE_INSTALL_PREFIX', '/')).resolve().is_relative_to(work)
    checks['child_prefix_inside_work'] = in_work
    if rc == 0 and in_work and record['checks'].get('windows_x86_64_build_then_install'):
        rc = run(['cmake', '--build', child_build, '--target', 'install'], work / 'child-install.log')
    rc = rc or run(['cmake', '-S', parent, '-B', build, '-DCMAKE_INSTALL_PREFIX=' + str(work / PREFIX_LINUX),
                    '-DCMAKE_BUILD_TYPE=Release'], work / 'parent-configure.log')
    rc = rc or run(['cmake', '-P', build / 'cmake_install.cmake'], work / 'parent-install.log')
    record['install_rc'] = rc
    checks['install_rc0'] = rc == 0
    if rc:
        return
    parent_cache = cache(build)
    record['caches'] = {'parent': parent_cache, 'child': child_cache}
    # native_runtime.mjs:110, :111 and :112-113.
    checks['parent_prefix_matches_child'] = Path(parent_cache['CMAKE_INSTALL_PREFIX']).resolve() == \
        Path(child_cache.get('CMAKE_INSTALL_PREFIX', '')).resolve()
    checks['child_target_arch'] = child_cache.get('TARGET_ARCH') == TUPLE
    checks['build_types'] = all(c.get('CMAKE_BUILD_TYPE', '').lower() == 'release'
                                for c in (parent_cache, child_cache))
    # native_runtime.mjs:114-118.
    manifest_file = child_build / 'install_manifest.txt'
    manifest = [str(Path(p).resolve()) for p in manifest_file.read_text().split('\n') if p] \
        if manifest_file.exists() else []
    record['manifest'] = manifest
    for name in ('thread', 'aio'):
        archive = (work / PREFIX_LINUX / 'lib' / TUPLE / f'libcangjie-{name}.a').resolve()
        checks[f'manifest_{name}'] = str(archive) in manifest and archive.is_file()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path, help='runtime directory')
    parser.add_argument('--work', required=True, type=Path)
    args = parser.parse_args()
    source, work = args.source.resolve(), args.work.resolve()
    if work == source or source in work.parents:
        parser.error('--work must be outside the source directory')
    work.mkdir(parents=True, exist_ok=False)
    record = {'source': str(source), 'checks': {}}
    try:
        argv = check_bat(work, source, record)
        check_call_sites(work, source, record)
        check_install(work, source, argv, record)
    finally:
        (work / 'record.json').write_text(json.dumps(record, indent=2) + '\n')
    failed = [k for k, v in record['checks'].items() if not v]
    for key, value in record['checks'].items():
        print(('PASS ' if value else 'FAIL ') + key)
    print('HANDOFF_FAILS=%d' % len(failed))
    raise SystemExit(1 if failed else 0)


if __name__ == '__main__':
    main()
