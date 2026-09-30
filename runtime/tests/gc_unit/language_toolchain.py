#!/usr/bin/env python3
"""Admission and cache identity for explicitly selected managed SDKs."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1048576), b''):
            digest.update(block)
    return digest.hexdigest()


def required(name):
    value = os.environ.get(name)
    if not value:
        raise ValueError(f'{name}_MISSING')
    return Path(value).resolve(strict=True)


def sdk_identity(sdk):
    compiler = sdk / 'bin/cjc'
    resolved = compiler.resolve(strict=True)
    if not resolved.is_relative_to(sdk) or not os.access(compiler, os.X_OK):
        raise ValueError('LANGUAGE_SDK_COMPILER_INVALID')
    identity = {'sdk': str(sdk), 'cjc': sha256(compiler)}
    for component in ('llc', 'opt'):
        identity[component] = sha256(sdk / f'third_party/llvm/bin/{component}')
    archives = {str(path.relative_to(sdk)): sha256(path) for path in
                sorted((sdk / 'lib/linux_x86_64_cjnative').glob('libcangjie-std-*.a'))}
    if 'lib/linux_x86_64_cjnative/libcangjie-std-core.a' not in archives:
        raise ValueError('LANGUAGE_SDK_STD_MISSING')
    identity['std'] = hashlib.sha256(json.dumps(archives, sort_keys=True).encode()).hexdigest()
    identity['std_core'] = archives['lib/linux_x86_64_cjnative/libcangjie-std-core.a']
    identity['runtime'] = sha256(sdk / 'runtime/lib/linux_x86_64_cjnative/libcangjie-runtime.so')
    return identity


def admit():
    build = required('GC_UNIT_BUILD_SDK')
    sdk = required('GC_UNIT_LANGUAGE_SDK')
    host = required('GC_UNIT_CJC_RUNTIME_LIB_DIR')
    target = required('GCV2_RUNTIME_LIB_DIR')
    qualification = required('GC_UNIT_LANGUAGE_QUALIFICATION')
    checker = required('GC_UNIT_COLOUR_CHECKER')
    colour_host = required('GC_UNIT_COLOUR_HOST_RUNTIME')
    if os.environ.get('CJC') and Path(os.environ['CJC']).resolve() != (sdk / 'bin/cjc').resolve():
        raise ValueError('LANGUAGE_SDK_CJC_CONFLICT')
    approved = Path(__file__).with_name('language_toolchain_qualification.json')
    if sha256(qualification) != sha256(approved):
        raise ValueError('LANGUAGE_QUALIFICATION_UNKNOWN')
    proof = json.loads(qualification.read_text())
    language = sdk_identity(sdk)
    for name, expected in proof['language'].items():
        if language[name] != expected:
            raise ValueError(f'LANGUAGE_TOOLCHAIN_INCOMPATIBLE component={name}')
    for relative, expected in proof['components'].items():
        if sha256(sdk / relative) != expected:
            raise ValueError(f'LANGUAGE_TOOLCHAIN_INCOMPATIBLE component={relative}')
    host_hash = sha256(host / 'libcangjie-runtime.so')
    if host_hash != proof['compiler_host']:
        raise ValueError('LANGUAGE_COMPILER_HOST_INCOMPATIBLE')
    if sha256(checker) != proof['colour_checker'] or sha256(colour_host) != proof['colour_host']:
        raise ValueError('LANGUAGE_COLOUR_REFERENCE_INCOMPATIBLE')
    result = subprocess.run([sys.executable, str(checker), '--colour-runtime',
                             str(target / 'libcangjie-runtime.so'), '--host-runtime',
                             str(colour_host), '--std-colour',
                             str(sdk / 'lib/linux_x86_64_cjnative/libcangjie-std-core.a')],
                            capture_output=True, text=True)
    if result.returncode or result.stdout.strip() != '1':
        raise ValueError('LANGUAGE_TOOLCHAIN_INCOMPATIBLE colour=' + result.stderr.strip())
    build_compiler = build / 'bin/cjc'
    if not os.access(build_compiler, os.X_OK):
        raise ValueError('BUILD_SDK_COMPILER_INVALID')
    identity = {'build': sdk_identity(build), 'language': language,
                'compiler_host': host_hash, 'compiler_host_dir': str(host),
                'qualification': sha256(qualification), 'checker': sha256(checker),
                'colour_host': sha256(colour_host),
                'admission': sha256(Path(__file__)),
                'target_runtime': sha256(target / 'libcangjie-runtime.so'),
                'target_boundscheck': sha256(target / 'libboundscheck.so')}
    return identity


def main():
    try:
        print(json.dumps(admit(), sort_keys=True))
    except (OSError, ValueError, KeyError) as error:
        print(f'LANGUAGE_ADMISSION_NOT_RUN reason={error}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
