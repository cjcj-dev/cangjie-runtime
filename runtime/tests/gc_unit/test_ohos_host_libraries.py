#!/usr/bin/env python3
"""Exercise the real runtime configure entry; run on the native Linux host.

The complete -> missing -> restored order deliberately reuses one CMake cache:
removing a shim after a successful configure must invalidate admission too.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    shims = output / 'shims'
    shims.mkdir(exist_ok=True)
    libc = Path(subprocess.check_output(
        ['clang++', '-print-file-name=libc.so.6'], text=True).strip()).resolve()
    if not libc.is_file():
        raise RuntimeError(f'host libc unavailable: {libc}')
    env = dict(os.environ, LD_LIBRARY_PATH=str(shims))
    command = ['cmake', '-S', str(source / 'runtime'),
               '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=clang',
               '-DCMAKE_CXX_COMPILER=clang++', '-DCMAKE_AR_PATH=ar',
               '-DCMAKE_C_COMPILER_LAUNCHER=ccache',
               '-DCMAKE_CXX_COMPILER_LAUNCHER=ccache',
               '-DCMAKE_ASM_COMPILER_LAUNCHER=ccache',
               '-DCJ_SDK_VERSION=0.0.1', '-DDISABLE_VERSION_CHECK=1']
    results = {}
    for case in ('complete', 'missing', 'restored', 'default'):
        shim = shims / 'libc.so'
        if case in ('complete', 'restored'):
            shutil.copyfile(libc, shim)
        else:
            shim.unlink(missing_ok=True)
        host = case != 'default'
        build = output / ('host-build' if host else 'default-build')
        run = subprocess.run(command + ['-B', str(build),
                             '-DMRT_GC_UNIT_OHOS_HOST=' + ('ON' if host else 'OFF')],
                             env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, timeout=180)
        (output / (case + '.log')).write_text(run.stdout)
        if case == 'missing':
            passed = run.returncode != 0 and 'OHOS_HOST_LIBRARY_MISSING libc.so:' in run.stdout
        elif host:
            passed = run.returncode == 0 and 'OHOS_HOST_LIBRARY_OK libc.so' in run.stdout
        else:
            passed = run.returncode == 0 and 'OHOS_HOST_LIBRARY_' not in run.stdout
        results[case] = dict(rc=run.returncode, passed=passed)
        print(f'OHOS_CONFIGURE_ASSERT case={case} rc={run.returncode} '
              f'status={"PASS" if passed else "FAIL"}', flush=True)
    paths = ['runtime/config.cmake', 'runtime/src/Signal/SignalStack.cpp',
             'runtime/build/cmake/CheckOHOSHostLibraries.cmake']
    hashes = {p: hashlib.sha256((source / p).read_bytes()).hexdigest()
              for p in paths if (source / p).exists()}
    (output / 'result.json').write_text(json.dumps(
        dict(source_sha256=hashes, cases=results), indent=2) + '\n')
    return int(not all(r['passed'] for r in results.values()))


if __name__ == '__main__':
    raise SystemExit(main())
