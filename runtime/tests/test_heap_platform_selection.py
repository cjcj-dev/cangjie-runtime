#!/usr/bin/env python3
"""Configure the real source/Heap CMake entries and check their OS selection.

This is a configure-only test: it needs CMake and a host C++ compiler, not an
Apple SDK. It proves source/header routing, not that an iOS runtime links/runs.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess

CASES = {
    'linux': ({}, 'linux'),
    'android': ({'ANDROID_FLAG': 1}, 'linux'),
    'ohos': ({'OHOS_FLAG': 1}, 'linux'),
    'windows': ({'WINDOWS_FLAG': 1}, 'windows'),
    'macos': ({'MACOS_FLAG': 1}, 'bsd'),
    'ios-device': ({'IOS_FLAG': 1}, 'bsd'),
    'ios-simulator-arm64': ({'IOS_SIMULATOR_FLAG': 1}, 'bsd'),
    'ios-simulator-x86_64': ({'IOS_SIMULATOR_FLAG': 2}, 'bsd'),
}


def check_case(source, work, name, flags, expected):
    root = work / name
    root.mkdir(parents=True)
    # BUILD_RUNTIME is off only to avoid unrelated platform SDK dependencies.
    # Both OS routing entries are executed verbatim; Heap is a real product
    # target and its SOURCES/INCLUDE_DIRECTORIES are the assertion inputs.
    (root / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.19)
project(HeapPlatformSelection LANGUAGES CXX)
set(BUILD_RUNTIME OFF)
set(BUILD_DEMANGLE OFF)
add_subdirectory("${RUNTIME_SOURCE}/src" runtime-src)
add_subdirectory("${RUNTIME_SOURCE}/src/Heap" heap)
get_directory_property(shared DIRECTORY "${RUNTIME_SOURCE}/src" INCLUDE_DIRECTORIES)
get_target_property(headers Heap INCLUDE_DIRECTORIES)
get_target_property(sources Heap SOURCES)
get_directory_property(definitions DIRECTORY "${RUNTIME_SOURCE}/src/Heap" COMPILE_DEFINITIONS)
file(WRITE "${CMAKE_BINARY_DIR}/selection.txt" "${shared}\\n${headers}\\n${sources}\\n${definitions}\\n")
''')
    command = ['cmake', '-S', str(root), '-B', str(root / 'build'),
               '-DRUNTIME_SOURCE=' + str(source)]
    command += ['-D' + key + '=' + str(flags.get(key, 0)) for key in
                ('MACOS_FLAG', 'IOS_FLAG', 'IOS_SIMULATOR_FLAG',
                 'WINDOWS_FLAG', 'ANDROID_FLAG', 'OHOS_FLAG')]
    result = subprocess.run(command, capture_output=True, text=True)
    (root / 'configure.log').write_text(result.stdout + result.stderr)
    record = {'case': name, 'command': command, 'configure_rc': result.returncode}
    if result.returncode:
        record['error'] = 'configure failed before assertions'
        return record
    shared, headers, sources, definitions = (root / 'build/selection.txt').read_text().splitlines()
    os_dirs = lambda value: {Path(p).name for p in value.split(';') if '/z/os/' in p}
    selected = set(sources.split(';'))
    backing = {p for p in selected if 'zPhysicalMemoryBacking_' in p}
    large = {p for p in selected if 'zLargePages_' in p}
    numa = {p for p in selected if 'zNUMA_' in p}
    virtual = {p for p in selected if 'zVirtualMemoryManager_' in p}
    record['observed'] = dict(shared=shared, headers=headers, sources=sources, definitions=definitions)
    record['checks'] = {
        'shared-header': os_dirs(shared) == {expected},
        'heap-header': os_dirs(headers) == {expected},
        'backing-source': backing == {'z/zPhysicalMemoryBacking_' + expected + '.cpp'},
        'large-pages-source': large == {'z/zLargePages_' + expected + '.cpp'},
        'numa-source': numa == {'z/zNUMA_' + expected + '.cpp'},
        'virtual-memory-source': virtual == {'z/zVirtualMemoryManager_' + ('windows' if expected == 'windows' else 'posix') + '.cpp'},
        'mount-source': ('z/zMountPoint_linux.cpp' in selected) == (expected == 'linux'),
        'linux-definition': ('LINUX' in definitions.split(';')) == (expected == 'linux'),
    }
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='runtime directory')
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    source, work = args.source.resolve(), args.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    with ThreadPoolExecutor(max_workers=len(CASES)) as pool:
        futures = [pool.submit(check_case, source, work, name, *case) for name, case in CASES.items()]
        records = [f.result() for f in futures]
    identity = {str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in (source / 'src/CMakeLists.txt', source / 'src/Heap/CMakeLists.txt', Path(__file__).resolve())}
    (work / 'results.json').write_text(json.dumps({'identity': identity, 'results': records}, indent=2) + '\n')
    failures = 0
    for record in records:
        if 'checks' not in record:
            print('ERROR', record['case'], record['error'])
            failures += 1
        for name, ok in record.get('checks', {}).items():
            print('PASS' if ok else 'FAIL', record['case'], name)
            failures += not ok
    print(f'CASES={len(records)} FAILURES={failures}')
    return int(failures != 0)


if __name__ == '__main__':
    raise SystemExit(main())
