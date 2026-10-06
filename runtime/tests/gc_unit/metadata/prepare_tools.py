#!/usr/bin/env python3
"""Narrow native tuple orchestration; no fixture targets and no SDK tool fallback."""
import gzip
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import urllib.request

import platform_inputs as inputs

TOOL_SOURCE = inputs.TOOL_SOURCE



def approved_sdk_inputs(repo):
    """Derive URLs and digests from the already identity-checked fixed provider."""
    provider = (repo / 'build/lib/release-component-provenance.mjs').as_uri()
    script = """
      const {baseSdkDownload, RELEASE_HOST_TOOLCHAIN} = await import(process.argv[1]);
      const platforms = {'linux-arm64': 'linux-aarch64', 'windows-x64': 'windows-x64',
                         'macos-arm64': 'darwin-arm64'};
      const base_sdk = Object.fromEntries(Object.entries(platforms).map(([key, platform]) => {
        const {url, sha256} = baseSdkDownload(platform, RELEASE_HOST_TOOLCHAIN);
        return [key, {url, sha256}];
      }));
      console.log(JSON.stringify({base_sdk}));
    """
    return json.loads(subprocess.check_output(
        ['node', '--input-type=module', '-e', script, provider], cwd=repo, text=True))


def validate_sdk_input(repo, selected, sdk):
    expected = approved_sdk_inputs(repo)['base_sdk'][selected]
    if sdk != expected:
        raise ValueError('base SDK input differs from fixed approved provider URL and digest')
    return expected


def tuple_environment(repo, environment):
    pins = inputs.fields(repo / 'ci/llvm_pin.env')
    env = dict(inputs.paired_environment(environment), LLVM_URL='https://github.com/cjcj-dev/cjcj-llvm.git', LLVM_SHA=inputs.PRODUCER,
               CANGJIE_COMPILER_URL=pins['CANGJIE_COMPILER_URL'], CANGJIE_COMPILER_SHA=pins['CANGJIE_COMPILER_SHA'],
               FLATBUFFERS_URL=pins['FLATBUFFERS_URL'], FLATBUFFERS_SHA=pins['FLATBUFFERS_SHA'],
               SCCACHE_PATH=shutil.which('sccache') or '')
    return env


def pair_receipt(root, environment, logs):
    env = inputs.paired_environment(environment)
    head = inputs.checkout_identity(root / 'paired-runtime', inputs.PAIRED_RUNTIME, logs=logs)
    return dict(paired_runtime_sha=head, paired_runtime_mode=env['CJCJ_LLVM_RUNTIME_MODE'],
                paired_runtime_url=env['CJCJ_LLVM_RUNTIME_URL'])


def main():
    repo = Path(sys.argv[1]).resolve()
    # Keep --sdk-inputs usable before native tuple configuration is supplied.
    log_root = os.environ.get('TUPLE_ROOT')
    if log_root is None:
        log_root = Path(os.environ.get('RUNNER_TEMP', str(repo.parent))) / 'metadata-tool-sources'
    logs = Path(os.path.abspath(log_root)) / 'logs'
    inputs.checkout_identity(repo, TOOL_SOURCE, role='tool', logs=logs)
    if sys.argv[2:] == ['--sdk-inputs']:
        print(json.dumps(approved_sdk_inputs(repo), indent=2))
        return
    selected = os.environ['METADATA_PLATFORM']
    native = ('windows-x64' if platform.system() == 'Windows' else
              'macos-arm64' if platform.system() == 'Darwin' else 'linux-arm64')
    arches = ('amd64', 'x86_64') if native == 'windows-x64' else ('aarch64', 'arm64')
    if selected != native or platform.machine().lower() not in arches:
        raise ValueError('tool preparation requires the selected native official runner')
    if os.environ['GITHUB_REPOSITORY'] != 'cjcj-dev/cangjie-runtime':
        raise ValueError('same runtime repository producer required')
    configuration = json.loads(os.environ['TOOL_PREPARE_INPUTS'])
    pins = inputs.fields(repo / 'ci/llvm_pin.env')
    sdk = validate_sdk_input(repo, selected, configuration['base_sdk'][selected])
    inputs.paired_environment(os.environ)  # Reject a wrong pair before SDK I/O.
    root = Path(os.environ['TUPLE_ROOT']).resolve()
    root.mkdir(parents=True, exist_ok=True)
    archive = root / 'base-sdk.archive'
    with urllib.request.urlopen(sdk['url']) as response, archive.open('wb') as output:
        shutil.copyfileobj(response, output)
    base_digest = inputs.sha(archive)
    if base_digest != sdk['sha256']:
        raise ValueError('base SDK archive digest differs from approved input')
    env = tuple_environment(repo, os.environ)
    if not env['SCCACHE_PATH']:
        raise ValueError('C++ sccache launcher is required')
    def checked(argv):
        subprocess.run([str(x) for x in argv], cwd=repo, env=env, check=True)
    checked(['bash', 'ci/platform_tuples/fetch_sources.sh'])
    checked(['bash', 'ci/platform_tuples/build_tuple.sh'])
    paired = pair_receipt(root, env, logs)
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
                   compiler_sha=pins['CANGJIE_COMPILER_SHA'], flatbuffers_sha=pins['FLATBUFFERS_SHA'],
                   **paired,
                   artifact='fixed-llvm-tools-' + os.environ['TUPLE_PLATFORM'], base_sdk_sha256=base_digest,
                   tuple_manifest_sha256=inputs.sha(output / 'llvm-tools.manifest'),
                   reader_manifest_sha256=inputs.sha(output / 'llvm-tools.packaged.manifest'))
    (output / 'producer.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2), flush=True)


if __name__ == '__main__':
    main()
