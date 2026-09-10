#!/usr/bin/env python3
"""Atomically publish the pair produced by the real runtime link entry.

Identity uses generated compiler/linker inputs, compiler/toolchain state and
CJThread's actual input bytes. The linked pair's hashes additionally bind the
identity to source provenance, so a later source revision cannot overwrite a
previous publication of the same configuration.
"""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canonical_json(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True) + '\n'


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, fromfile_prefix_chars='@')
    for name in ['source', 'build', 'staging', 'runtime', 'boundscheck', 'compiler-state']:
        parser.add_argument('--' + name, required=True, type=Path)
    for name in ['label', 'library-subdir', 'generator', 'make-program']:
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--toolchain', default='')
    parser.add_argument('--testable', default='0')
    parser.add_argument('--ohos', default='0')
    parser.add_argument('--gate', type=Path)
    return parser.parse_args()


def generated_inputs(args):
    def normalize(text):
        # These paths describe the private workspace, not a product option.
        text = text.replace(str(args.staging), '<STAGING>')
        text = text.replace(str(args.build), '<BUILD>')
        return re.sub(r'::@\([^)]*\)', '::@directory', text)

    def text_input(path):
        return normalize(path.read_text())

    commands_path = args.build / 'compile_commands.json'
    if not commands_path.is_file():
        raise RuntimeError('generated compile_commands.json is required for runtime publication')
    commands = json.loads(commands_path.read_text())
    if not commands:
        raise RuntimeError('generated compile command database is empty')
    generated = args.build / 'runtime-generated-inputs'
    exports = sorted(generated.glob('*.txt'))
    if not exports:
        raise RuntimeError('generation-time target input exports are missing')
    # Consume native generated link commands, including global/config-specific
    # linker flags. This needs no File API query or second configure pass.
    if args.generator == 'Ninja':
        result = subprocess.run([args.make_program, '-C', str(args.build), '-t', 'commands'],
                                capture_output=True, text=True, check=True)
        links = {'ninja-commands': normalize(result.stdout)}
    else:
        paths = [Path(line) for line in (args.build / 'runtime-link-inputs.txt').read_text().splitlines() if line]
        links = {normalize(str(path)): text_input(path) for path in paths}
    if not links:
        raise RuntimeError('generated link command inputs are missing')
    compiler_files = sorted(args.compiler_state.glob('CMake*Compiler.cmake'))
    if not compiler_files:
        raise RuntimeError('persisted compiler state is missing')
    state = {path.name: text_input(path) for path in compiler_files}
    if args.toolchain:
        toolchain = Path(args.toolchain)
        if not toolchain.is_absolute():
            toolchain = args.build / toolchain
        state['toolchain'] = text_input(toolchain)
    cjthread_paths = [Path(line) for line in
                      (args.build / 'runtime-cjthread-inputs.txt').read_text().splitlines() if line]
    cjthread = {str(path.relative_to(args.staging)): sha(path) for path in cjthread_paths}
    return {
        'schema': 6,
        'commands': sorted([json.loads(normalize(canonical_json(entry))) for entry in commands],
                           key=canonical_json),
        'links': links,
        'generated': {path.name: text_input(path) for path in exports},
        'compiler_state': state,
        'cjthread': cjthread,
        'products': {args.runtime.name: sha(args.runtime), args.boundscheck.name: sha(args.boundscheck)},
    }, cjthread_paths


def publish(args):
    for field in ['source', 'build', 'staging', 'runtime', 'boundscheck', 'compiler_state']:
        setattr(args, field, getattr(args, field).resolve())
    if not re.fullmatch(r'[a-z0-9._+-]+', args.label):
        raise RuntimeError('invalid readable configuration label')
    inputs, cjthread_paths = generated_inputs(args)
    signature_text = canonical_json(inputs)
    signature = hashlib.sha256(signature_text.encode()).hexdigest()
    config_id = args.label + '-' + signature
    root = args.source / 'output/temp' / config_id
    relative_lib = Path('lib') / args.library_subdir
    lib = root / relative_lib
    files = {str(relative_lib / args.runtime.name): args.runtime,
             str(relative_lib / args.boundscheck.name): args.boundscheck}
    files.update({str(path.relative_to(args.staging)): path for path in cjthread_paths})
    hashes = {name: sha(path) for name, path in files.items()}
    manifest = (f'SCHEMA_VERSION=6\nCONFIG_ID={config_id}\n'
                f'CONFIG_SIGNATURE_SHA256={signature}\nLIB_DIR={lib}\n'
                f'RUNTIME_SHA256={sha(args.runtime)}\nBOUNDSCHECK_SHA256={sha(args.boundscheck)}\n')
    metadata = {'runtime-build-config.txt': manifest,
                'runtime-build-inputs.txt': signature_text,
                'runtime-product-hashes.json': canonical_json(hashes)}
    root.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix='.publishing-', dir=root.parent))
    try:
        for relative, source in files.items():
            destination = temporary / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
            if sha(destination) != hashes[relative]:
                raise RuntimeError('input changed while copying: ' + str(source))
        for relative, content in metadata.items():
            (temporary / relative).write_text(content)
        try:
            os.rename(temporary, root)
        except OSError as error:
            if error.errno not in (errno.EEXIST, errno.ENOTEMPTY):
                raise
            # Published artifacts are immutable. A failed comparison never
            # replaces the previous directory, including in concurrent builds.
            for path in temporary.rglob('*'):
                if path.is_file():
                    existing = root / path.relative_to(temporary)
                    if not existing.is_file() or sha(existing) != sha(path):
                        raise RuntimeError('identity collision: published bytes differ: ' + str(existing))
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    # Record the last successfully published identity, never a configure guess.
    cache = args.build / 'CMakeCache.txt'
    cache_text = cache.read_text()
    for key, value in [('CANGJIE_RUNTIME_CONFIG_ID', config_id),
                       ('CANGJIE_RUNTIME_CONFIG_SIGNATURE', signature),
                       ('OUTPUT_TEMP_PATH', str(root))]:
        cache_text = re.sub(r'^' + key + r':[^=\n]*=.*\n?', '', cache_text, flags=re.M)
        cache_text += f'{key}:INTERNAL={value}\n'
    if cache_text != cache.read_text():
        cache_tmp = cache.with_suffix('.publish.tmp')
        cache_tmp.write_text(cache_text)
        os.replace(cache_tmp, cache)
    print(f'RUNTIME_OUTPUT_PUBLISHED config={config_id} lib_dir={lib} '
          f'runtime_sha256={sha(lib / args.runtime.name)} '
          f'boundscheck_sha256={sha(lib / args.boundscheck.name)}', flush=True)
    if args.gate:
        env = dict(os.environ, GCV2_RUNTIME_CONFIG=config_id,
                   GCV2_RUNTIME_LIB_DIR=str(lib), GCV2_RUNTIME_OUTPUT_ROOT=str(root),
                   MRT_TESTABLE_INTERNALS=args.testable, MRT_GC_UNIT_OHOS_HOST=args.ohos)
        return subprocess.run(['bash', str(args.gate)], cwd=args.source, env=env).returncode
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(publish(parse_args()))
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        print(f'RUNTIME_OUTPUT_PUBLISH_FAIL: {error}', flush=True)
        raise SystemExit(2)
