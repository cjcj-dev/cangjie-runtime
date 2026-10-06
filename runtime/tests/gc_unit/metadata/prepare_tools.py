#!/usr/bin/env python3
"""Narrow native tuple orchestration; no fixture targets and no SDK tool fallback."""
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import urllib.request

import platform_inputs as inputs

TOOL_SOURCE = 'bf788bfcb2f1b3172cebb395169b375d1656b91d'


def main():
    repo = Path(sys.argv[1]).resolve()
    inputs.checkout_identity(repo, TOOL_SOURCE)
    selected = os.environ['METADATA_PLATFORM']
    native = ('windows-x64' if platform.system() == 'Windows' else
              'macos-arm64' if platform.system() == 'Darwin' else 'linux-arm64')
    arches = ('amd64', 'x86_64') if native == 'windows-x64' else ('aarch64', 'arm64')
    if selected != native or platform.machine().lower() not in arches:
        raise ValueError('tool preparation requires the selected native official runner')
    if os.environ['GITHUB_REPOSITORY'] != 'cjcj-dev/cangjie-runtime':
        raise ValueError('same runtime repository producer required')
    configuration = json.loads(os.environ['TOOL_PREPARE_INPUTS'])
    for prefix, permitted in [('compiler', 'https://github.com/cjcj-dev/cangjie-compiler.git'),
                              ('flatbuffers', 'https://github.com/google/flatbuffers.git')]:
        if configuration[prefix + '_url'] != permitted or not re.fullmatch('[0-9a-f]{40}', configuration[prefix + '_sha']):
            raise ValueError('unapproved public fixed source: ' + prefix)
    sdk = configuration['base_sdk'][selected]
    if (not sdk['url'].startswith('https://github.com/') or '/releases/download/' not in sdk['url'] or
            not re.fullmatch('[0-9a-f]{64}', sdk['sha256'])):
        raise ValueError('real approved base SDK archive URL and digest required')
    root = Path(os.environ['TUPLE_ROOT']).resolve()
    root.mkdir(parents=True, exist_ok=True)
    archive = root / 'base-sdk.archive'
    with urllib.request.urlopen(sdk['url']) as response, archive.open('wb') as output:
        shutil.copyfileobj(response, output)
    base_digest = inputs.sha(archive)
    if base_digest != sdk['sha256']:
        raise ValueError('base SDK archive digest differs from approved input')
    env = dict(os.environ, LLVM_URL='https://github.com/cjcj-dev/cjcj-llvm.git', LLVM_SHA=inputs.PRODUCER,
               CANGJIE_COMPILER_URL=configuration['compiler_url'], CANGJIE_COMPILER_SHA=configuration['compiler_sha'],
               FLATBUFFERS_URL=configuration['flatbuffers_url'], FLATBUFFERS_SHA=configuration['flatbuffers_sha'],
               SCCACHE_PATH=shutil.which('sccache') or '')
    if not env['SCCACHE_PATH']:
        raise ValueError('C++ sccache launcher is required')
    def checked(argv):
        subprocess.run([str(x) for x in argv], cwd=repo, env=env, check=True)
    checked(['bash', 'ci/platform_tuples/fetch_sources.sh'])
    checked(['bash', 'ci/platform_tuples/build_tuple.sh'])
    # Normal dependency build in the SAME tuple CMake tree, not a second LLVM recipe.
    checked(['cmake', '--build', root / 'llvm-build', '--target', 'llvm-readobj', 'lld',
             '--parallel', str(os.cpu_count())])
    output = repo / 'fixed-toolchain' / os.environ['TUPLE_PLATFORM']
    suffix = '.exe' if native == 'windows-x64' else ''
    reader = root / 'llvm-build/bin' / ('llvm-readobj' + suffix)
    with reader.open('rb') as source, (output / 'llvm-readobj.gz').open('wb') as target:
        with gzip.GzipFile(filename='', mode='wb', fileobj=target, mtime=0) as stream:
            shutil.copyfileobj(source, stream)
    checked(['node', Path(__file__).with_name('package_tools.mjs'), repo, root, output, base_digest])
    receipt = dict(repository=os.environ['GITHUB_REPOSITORY'], run=os.environ['GITHUB_RUN_ID'],
                   attempt=os.environ['GITHUB_RUN_ATTEMPT'], producer_sha=os.environ['GITHUB_SHA'],
                   tools_source_sha=TOOL_SOURCE, llvm_sha=inputs.PRODUCER,
                   compiler_sha=configuration['compiler_sha'], flatbuffers_sha=configuration['flatbuffers_sha'],
                   paired_runtime_sha=subprocess.check_output(['git', '-C', str(root / 'paired-runtime'), 'rev-parse', 'HEAD'], text=True).strip(),
                   artifact='fixed-llvm-tools-' + os.environ['TUPLE_PLATFORM'], base_sdk_sha256=base_digest,
                   tuple_manifest_sha256=inputs.sha(output / 'llvm-tools.manifest'),
                   reader_manifest_sha256=inputs.sha(output / 'llvm-tools.packaged.manifest'))
    (output / 'producer.json').write_text(json.dumps(receipt, indent=2) + '\n')


if __name__ == '__main__':
    main()
