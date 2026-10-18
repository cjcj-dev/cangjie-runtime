#!/usr/bin/env python3
"""Validate current installed inputs; record bytes, never approved tuples."""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
from std_runtime_colour import symbols, colour_symbols


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


def elf(path):
    with path.open('rb') as stream:
        header = stream.read(20)
    if len(header) != 20 or header[:4] != b'\x7fELF' or header[5] != 1:
        raise ValueError(f'LANGUAGE_ELF_INVALID {path}')
    return header[4], header[5], struct.unpack('<H', header[18:20])[0]


def tree_identity(root, pattern='*'):
    files = {str(p.relative_to(root)): sha256(p) for p in sorted(root.rglob(pattern)) if p.is_file()}
    if not files:
        raise ValueError(f'LANGUAGE_INPUT_EMPTY {root}')
    return hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()



def shared_library_identity(root):
    # Loader inputs only. Gate status, diagnostics and other output files may
    # share this directory (notably the default C3 host=target layout).
    files = {str(p.relative_to(root)): sha256(p) for p in sorted(root.rglob('*'))
             if p.is_file() and re.search(r'\.so(?:\.[^/]*)?$', p.name)}
    return hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()


def execution_library_inputs(sdk, host, tuple_name):
    # Native entry recipes replace LD_LIBRARY_PATH. Bind their actual SDK
    # search directories as well as the direct ELF SDK layout. This includes
    # LLVM's shared objects, not only the opt/llc entry and retained ELF.
    directories = {host, sdk / 'lib' / tuple_name, sdk / 'runtime/lib' / tuple_name,
                   sdk / 'third_party/llvm/lib', sdk / 'tools/lib'}
    for relative in ('bin/cjc', 'third_party/llvm/bin/llc', 'third_party/llvm/bin/opt'):
        entry = (sdk / relative).resolve(strict=True)
        recipe = Path(str(entry) + '.entry')
        if not recipe.is_file():
            continue
        for member in recipe.read_text().splitlines()[5:]:
            if not member or Path(member).is_absolute() or any(p in ('', '.', '..') for p in member.split('/')):
                raise ValueError('LANGUAGE_LIBRARY_RECIPE_ESCAPE')
            directory = (sdk / member).resolve()
            if not directory.is_relative_to(sdk):
                raise ValueError('LANGUAGE_LIBRARY_RECIPE_ESCAPE')
            directories.add(directory)
    result = {}
    for directory in sorted(directories):
        # The optional host fallback can be external for an unwrapped compiler.
        key = str(directory.relative_to(sdk)) if directory.is_relative_to(sdk) else 'compiler-host'
        result[key] = shared_library_identity(directory)
    return result


def compiler_input(sdk, host):
    compiler = (sdk / 'bin/cjc').resolve(strict=True)
    if not compiler.is_relative_to(sdk) or not os.access(compiler, os.X_OK):
        raise ValueError('LANGUAGE_SDK_COMPILER_INVALID')
    if os.environ.get('CJC') and Path(os.environ['CJC']).resolve() != compiler:
        raise ValueError('LANGUAGE_SDK_CJC_CONFLICT')
    recipe = Path(str(compiler) + '.entry')
    recipe_hash = None
    if recipe.is_file():
        lines = recipe.read_text().splitlines()
        if len(lines) < 6 or lines[0] != 'cjcj-sdk-entry-v1' or lines[1] != '1':
            raise ValueError('LANGUAGE_COMPILER_RECIPE_INVALID')
        members = [lines[2], *lines[5:]]
        for member in members:
            if not member or Path(member).is_absolute() or any(x in ('.', '..', '') for x in member.split('/')):
                raise ValueError('LANGUAGE_COMPILER_RECIPE_ESCAPE')
        selected = next(((sdk / member).resolve() for member in lines[5:]
                         if (sdk / member / 'libcangjie-runtime.so').is_file()), None)
        if selected != host:
            raise ValueError('LANGUAGE_COMPILER_RUNTIME_CONFLICT')
        compiler = (sdk / lines[2]).resolve(strict=True)
        if not compiler.is_relative_to(sdk):
            raise ValueError('LANGUAGE_COMPILER_RECIPE_ESCAPE')
        recipe_hash = sha256(recipe)
    manifest = json.loads((sdk / 'compiler-inputs.json').read_text())
    if not re.fullmatch('[0-9a-f]{40}', manifest.get('commit', '')):
        raise ValueError('LANGUAGE_COMPILER_SOURCE_MISSING')
    if manifest.get('compilerSha256') != sha256(compiler):
        raise ValueError('LANGUAGE_COMPILER_SOURCE_MISMATCH')
    return compiler, {'cjc': sha256(compiler), 'entry': sha256(sdk / 'bin/cjc'),
                      'recipe': recipe_hash, 'source': manifest['commit']}


def backend_input(sdk, component):
    entry = (sdk / f'third_party/llvm/bin/{component}').resolve(strict=True)
    if not entry.is_relative_to(sdk):
        raise ValueError('LANGUAGE_BACKEND_ESCAPE')
    recipe = Path(str(entry) + '.entry')
    binary = entry
    identity = {'entry': sha256(entry), 'recipe': None}
    if recipe.is_file():
        lines = recipe.read_text().splitlines()
        if len(lines) < 6 or lines[0] != 'cjcj-sdk-entry-v1' or not lines[1].isdigit():
            raise ValueError('LANGUAGE_BACKEND_RECIPE_INVALID')
        ascent = int(lines[1])
        if ascent > 32:
            raise ValueError('LANGUAGE_BACKEND_RECIPE_INVALID')
        root = entry.parent
        for _ in range(ascent):
            root = root.parent
        if root != sdk or Path(lines[2]).is_absolute() or any(p in ('', '.', '..') for p in lines[2].split('/')):
            raise ValueError('LANGUAGE_BACKEND_ESCAPE')
        binary = (root / lines[2]).resolve(strict=True)
        if not binary.is_relative_to(sdk):
            raise ValueError('LANGUAGE_BACKEND_ESCAPE')
        identity['recipe'] = sha256(recipe)
    identity['binary'] = sha256(binary)
    return binary, identity


def admit():
    sdk = required('GC_UNIT_LANGUAGE_SDK')
    host = required('GC_UNIT_CJC_RUNTIME_LIB_DIR')
    target = required('GCV2_RUNTIME_LIB_DIR')
    plain = required('GC_UNIT_COLOUR_HOST_RUNTIME')
    tuple_name = os.environ.get('GC_UNIT_RUNTIME_TUPLE', 'linux_x86_64_cjnative')
    if tuple_name not in ('linux_x86_64_cjnative', 'linux_aarch64_cjnative'):
        raise ValueError('LANGUAGE_PLATFORM_UNSUPPORTED')
    expected = (2, 1, 62 if tuple_name == 'linux_x86_64_cjnative' else 183)
    compiler, language = compiler_input(sdk, host)
    files = [compiler, host / 'libcangjie-runtime.so', host / 'libboundscheck.so', plain,
             target / 'libcangjie-runtime.so', target / 'libboundscheck.so']
    for component in ('llc', 'opt'):
        file, backend = backend_input(sdk, component)
        files.append(file)
        language[component] = backend['binary']
        language[component + '_entry'] = backend
    for file in files:
        if elf(file) != expected:
            raise ValueError(f'LANGUAGE_PLATFORM_MISMATCH {file}')
    for name in ('libcangjie-runtime.so', 'libboundscheck.so'):
        if sha256(sdk / 'runtime/lib' / tuple_name / name) != sha256(target / name):
            raise ValueError(f'LANGUAGE_TARGET_PAIR_CONFLICT {name}')
    core = sdk / 'lib' / tuple_name / 'libcangjie-std-core.a'
    exports = colour_symbols(target / 'libcangjie-runtime.so', plain)
    references = symbols(core)
    if not references & exports or not references & {'CJ_MCC_ReadStaticRef', 'CJ_MCC_ReadRefField'}:
        raise ValueError('LANGUAGE_STD_RUNTIME_COLOUR_MISMATCH')
    language.update(std=tree_identity(sdk / 'lib' / tuple_name, '*.a'), std_core=sha256(core),
                    modules=tree_identity(sdk / 'modules' / tuple_name, '*.cjo'))
    # Paths are execution addresses, not cache identity: a byte-identical unpack
    # remains valid. Every input which changes generated code is still bound.
    return {'language': language, 'compiler_host': sha256(host / 'libcangjie-runtime.so'),
            'compiler_host_set': shared_library_identity(host), 'platform': tuple_name,
            'execution_libraries': execution_library_inputs(sdk, host, tuple_name),
            'admission': sha256(Path(__file__)),
            'colour_check': sha256(Path(__file__).with_name('std_runtime_colour.py')),
            'colour_host': sha256(plain),
            'target_runtime': sha256(target / 'libcangjie-runtime.so'),
            'target_boundscheck': sha256(target / 'libboundscheck.so')}


def verify_loads(directory):
    target = required('GCV2_RUNTIME_LIB_DIR')
    host = required('GC_UNIT_CJC_RUNTIME_LIB_DIR')
    logs = [('finalizer_trigger.build.log', host), ('finalizer_trigger.run.log', target),
            ('phase_entry_trigger.build.log', host), ('phase_entry_minor.run.log', target),
            ('phase_entry_major.run.log', target)]
    for name, expected in logs:
        text = (directory / name).read_text()
        events = re.findall(r'^\s*(\d+):\s*calling init:\s*(\S+)', text, re.M)
        runtime_pids = {pid for pid, file in events if file.endswith('/libcangjie-runtime.so')}
        if not runtime_pids:
            raise ValueError(f'LANGUAGE_ACTUAL_LOAD_MISSING {name}')
        for pid in runtime_pids:
            for library in ('libcangjie-runtime.so', 'libboundscheck.so'):
                loaded = [file for owner, file in events if owner == pid and file.endswith('/' + library)]
                if not loaded or any(Path(file).resolve() != (expected / library).resolve() for file in loaded):
                    raise ValueError(f'LANGUAGE_ACTUAL_LOAD_MISMATCH log={name} pid={pid} library={library} paths={loaded}')
    print('LANGUAGE_ACTUAL_LOAD_ASSERT_PASS logs=5', file=sys.stderr)


def configuration():
    output = required('GCV2_RUNTIME_OUTPUT_ROOT')
    target = required('GCV2_RUNTIME_LIB_DIR')
    fields = {}
    for line in (output / 'runtime-build-config.txt').read_text().splitlines():
        key, value = line.split('=', 1)
        if key in fields:
            raise ValueError(f'LANGUAGE_CONFIGURATION_DUPLICATE {key}')
        fields[key] = value
    if not re.fullmatch('[a-z0-9._+-]+', fields['CONFIG_ID']) or not re.fullmatch('[a-f0-9]{64}', fields['CONFIG_SIGNATURE_SHA256']):
        raise ValueError('LANGUAGE_CONFIGURATION_MISSING')
    for field, file in [('RUNTIME_SHA256','libcangjie-runtime.so'),('BOUNDSCHECK_SHA256','libboundscheck.so')]:
        if fields[field] != sha256(target / file):
            raise ValueError(f'LANGUAGE_CONFIGURATION_PAIR_MISMATCH {file}')
    return fields['CONFIG_ID'], fields['CONFIG_SIGNATURE_SHA256']


def main():
    try:
        if sys.argv[1:] == ['--configuration']:
            print(*configuration()); return 0
        if len(sys.argv) == 3 and sys.argv[1] == '--verify-runs':
            verify_loads(Path(sys.argv[2])); return 0
        identity = admit()
        if len(sys.argv) == 3 and sys.argv[1] == '--verify-status':
            lines = Path(sys.argv[2]).read_text().splitlines()
            rows = [line.split('=', 1)[1] for line in lines if line.startswith('TOOLCHAIN_IDENTITY=')]
            if len(rows) != 1 or json.loads(rows[0]) != identity:
                raise ValueError('LANGUAGE_CACHE_INPUT_CHANGED')
        print(json.dumps(identity, sort_keys=True))
    except (OSError, ValueError, KeyError, StopIteration) as error:
        print(f'LANGUAGE_ADMISSION_NOT_RUN reason={error}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
