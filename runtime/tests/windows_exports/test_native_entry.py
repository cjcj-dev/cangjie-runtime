#!/usr/bin/env python3
"""Run the pinned native Windows production entry and retain export evidence."""
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
    parser.add_argument('--source', type=Path, required=True, help='runtime repository')
    parser.add_argument('--production-entry', type=Path, required=True, help='cjcj repository')
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    assert os.name == 'nt', 'this acceptance entry requires native Windows'
    source, entry, evidence = (p.resolve() for p in (args.source, args.production_entry, args.evidence))
    evidence.mkdir(parents=True, exist_ok=True)
    runtime = source / 'runtime'
    build = runtime / 'CMakebuild'
    entry_script = entry / 'ci/platform_matrix/build_runtime.mjs'
    revision = subprocess.check_output(['git', '-C', source, 'rev-parse', 'HEAD'], text=True).strip()
    entry_revision = subprocess.check_output(['git', '-C', entry, 'rev-parse', 'HEAD'], text=True).strip()
    assert entry_revision == '34ade075158dce9bb380297cd0c8e1d785a035d8'
    env = dict(os.environ, RUNTIME_SOURCE=str(source), RUNTIME_REF=revision,
               RUNTIME_TARGET='windows-x86_64', GC_UNIT_GATE_SKIP='1')
    env['PATH'] = 'C:/msys64/mingw64/bin;C:/msys64/usr/bin;' + env['PATH']
    record = {'runtime_ref': revision, 'entry_ref': entry_revision,
              'entry_sha256': sha(entry_script), 'test_sha256': sha(Path(__file__)),
              'generator_sha256': sha(runtime / 'build/generate_windows_exports.py'),
              'jobs': os.cpu_count()}
    start = time.monotonic()
    with (evidence / 'production-entry.log').open('w') as log:
        rc = subprocess.run(['cmd', '/c', 'npx', '--yes', 'zx@8', str(entry_script)],
                            cwd=entry, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
    record['production_entry'] = {'rc': rc, 'wall': time.monotonic() - start}
    # Capture the actual linker's output and installed/staged products before
    # reporting failure, so a partial build remains diagnosable.
    products = list((entry / '.platform-ci/runtime-install').rglob('libcangjie-runtime.dll'))
    products += list(build.rglob('libboundscheck.dll'))
    keep = evidence / 'keep'
    keep.mkdir(exist_ok=True)
    record['products'] = {}
    for index, product in enumerate(products):
        saved = keep / f'{index}-{product.name}'
        shutil.copy2(product, saved)
        record['products'][str(product)] = sha(saved)
    for product in (build / 'windows_x86_64_exports.raw.def',
                    runtime / 'src/windows_x86_64_exports.def',
                    runtime / 'src/windows_export_references.json'):
        if product.is_file():
            shutil.copy2(product, evidence / product.name)
    raw = evidence / 'windows_x86_64_exports.raw.def'
    if raw.is_file():
        record['raw_sha256'] = sha(raw)
    (evidence / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'WINDOWS_PRODUCTION_ENTRY_ASSERT rc={rc} wall={record["production_entry"]["wall"]:.3f}', flush=True)
    assert rc == 0, 'complete cjcj production entry failed'
    assert any(p.name == 'libcangjie-runtime.dll' for p in products), 'installed runtime DLL missing'
    assert any(p.name == 'libboundscheck.dll' for p in products), 'linked boundscheck DLL missing'
    assert raw.is_file(), 'actual linker export definition missing'


if __name__ == '__main__':
    main()
