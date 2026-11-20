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


def directory_identity(directory):
    files = {path.name: sha256(path) for path in sorted(directory.iterdir()) if path.is_file()}
    return hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()


ENTRY_MAGIC = 'cjcj-sdk-entry-v1'


def launcher_entry(sdk, launcher):
    """Entity and search directories named by a cjcj native-launcher entry.

    Returns None when the launcher has no sibling .entry (legacy direct ELF).
    The launcher stub itself is shared by cjc/llc/opt, so its hash alone does
    not identify the compiler or LLVM entity it executes.
    """
    resolved = launcher.resolve(strict=True)
    entry = resolved.with_name(resolved.name + '.entry')
    if not entry.is_file():
        return None
    lines = entry.read_text().splitlines()
    if len(lines) < 5 or lines[0] != ENTRY_MAGIC:
        raise ValueError('LANGUAGE_SDK_ENTRY_INVALID')
    entity = (sdk / lines[2]).resolve(strict=True)
    search = [line for line in lines[5:] if line]
    if not entity.is_relative_to(sdk) or any(Path(line).is_absolute() or '..' in Path(line).parts for line in search):
        raise ValueError('LANGUAGE_SDK_ENTRY_INVALID')
    return {'entry': entry, 'entity': entity, 'search': search}


def compiler_host_dirs(sdk):
    """The compiler-execution host: legacy host/compiler or the launcher's .compiler-exec dirs."""
    legacy = sdk / 'host/compiler'
    if legacy.is_dir():
        return [legacy], True
    launcher = launcher_entry(sdk, sdk / 'bin/cjc')
    dirs = [sdk / line for line in launcher['search'] if line.startswith('.compiler-exec/')] if launcher else []
    if not dirs or not all(directory.is_dir() for directory in dirs):
        raise ValueError('LANGUAGE_COMPILER_HOST_INCOMPATIBLE')
    return dirs, False


def host_set_identity(sdk, dirs):
    files = {str(path.relative_to(sdk)): sha256(path)
             for directory in dirs for path in sorted(directory.iterdir()) if path.is_file()}
    return hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()


def sdk_identity(sdk):
    compiler = sdk / 'bin/cjc'
    resolved = compiler.resolve(strict=True)
    if not resolved.is_relative_to(sdk) or not os.access(compiler, os.X_OK):
        raise ValueError('LANGUAGE_SDK_COMPILER_INVALID')
    identity = {'sdk': str(sdk), 'cjc': sha256(compiler)}
    for component in ('llc', 'opt'):
        identity[component] = sha256(sdk / f'third_party/llvm/bin/{component}')
    for component, launcher in (('cjc', compiler), ('llc', sdk / 'third_party/llvm/bin/llc'),
                                ('opt', sdk / 'third_party/llvm/bin/opt')):
        target = launcher_entry(sdk, launcher)
        if target:
            identity[f'{component}_entry'] = sha256(target['entry'])
            identity[f'{component}_entity'] = sha256(target['entity'])
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
    host_dirs, legacy_host = compiler_host_dirs(sdk)
    if legacy_host:
        host_identity = directory_identity(host)
        host_bound = directory_identity(sdk / 'host/compiler') == host_identity
    else:
        host_identity = host_set_identity(sdk, host_dirs)
        host_bound = host in [directory.resolve(strict=True) for directory in host_dirs]
    if host_hash != proof['compiler_host'] or host_identity != proof['compiler_host_set'] or not host_bound:
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
                'compiler_host_set': host_identity, 'qualified_components': proof['components'],
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
