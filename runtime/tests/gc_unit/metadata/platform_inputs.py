"""Bound inputs consumed by the existing metadata workflow and build entry."""
import gzip
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

PRODUCER = '20a76c752153ca2f56b3f65b205653f7f60e8869'
PLATFORMS = {'linux-arm64': 'linux_aarch64', 'windows-x64': 'windows_x86_64',
             'macos-arm64': 'darwin_aarch64'}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dispatch(environment):
    """The new source is explicit, but remains bound to the approved event head."""
    event = environment.get('GITHUB_SHA', '')
    source = environment.get('METADATA_RUNTIME_SOURCE_SHA', '')
    candidate = environment.get('METADATA_CANDIDATE_SHA', '')
    if (environment.get('GITHUB_EVENT_NAME') != 'workflow_dispatch' or
            environment.get('GITHUB_REF_NAME') != 'sym/1496-fixtures-1006' or
            not re.fullmatch('[0-9a-f]{40}', event) or source != event or candidate != event):
        raise ValueError('fixture dispatch must bind sym/1496 event, candidate and runtime source SHA')
    if environment.get('METADATA_RESUME_RUN', '') or environment.get('METADATA_PAC_RESUME', 'false') != 'false':
        raise ValueError('fixture mode cannot consume A2/PAC resume inputs')
    if not re.fullmatch('[1-9][0-9]*', environment.get('METADATA_TUPLE_RUN', '')):
        raise ValueError('an existing cjcj tuple run is required')
    manifests = json.loads(environment['METADATA_TOOL_MANIFESTS'])
    if set(manifests) != set(PLATFORMS):
        raise ValueError('manifest hashes required for all three native platforms')
    for hashes in manifests.values():
        if set(hashes) != {'tuple', 'reader'} or any(not re.fullmatch('[0-9a-f]{64}', v) for v in hashes.values()):
            raise ValueError('invalid approved tuple/reader manifest hashes')
    return source, manifests


def checkout_identity(root, expected):
    actual = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != expected:
        raise ValueError('runtime checkout does not match runtime_source_sha')
    if subprocess.check_output(['git', '-C', str(root), 'status', '--porcelain'], text=True).strip():
        raise ValueError('runtime checkout must be clean')
    return actual


def fields(path):
    result = {}
    for line in path.read_text().splitlines():
        key, value = line.split('=', 1)
        if key in result:
            raise ValueError('duplicate manifest field: ' + key)
        result[key] = value
    return result


def native(path, platform):
    data = path.read_bytes()[:4096]
    if platform == 'linux_aarch64':
        valid = data[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', data, 18)[0] == 183
    elif platform == 'darwin_aarch64':
        valid = data[:4] == b'\xcf\xfa\xed\xfe' and struct.unpack_from('<I', data, 4)[0] == 0x100000c
    else:
        offset = struct.unpack_from('<I', data, 60)[0] if data[:2] == b'MZ' else len(data)
        valid = data[offset:offset + 6] == b'PE\0\0\x64\x86'
    if not valid:
        raise ValueError('tool is not native ' + platform + ': ' + str(path))


def tools(artifact, output, platform, hashes):
    """Consume the existing tuple layout; never select latest or generate tools."""
    artifact = artifact.resolve()
    manifest = artifact / 'llvm-tools.manifest'
    reader_manifest = artifact / 'llvm-tools.packaged.manifest'
    if sha(manifest) != hashes['tuple'] or sha(reader_manifest) != hashes['reader']:
        raise ValueError('tool manifest differs from approved dispatch input')
    values = fields(manifest)
    linker_name = 'ld64.lld' if platform.startswith('darwin_') else 'ld.lld'
    if (values['LLVM_SHA'] != PRODUCER or values['PLATFORM'] != platform or
            values['LLD_SOURCE'] != 'tuple:' + PRODUCER or values['LLD_TOOL'] != linker_name):
        raise ValueError('tuple platform/linker producer mismatch')
    lines = reader_manifest.read_text().splitlines()
    if lines[:2] != ['SCHEMA=packaged-v1', 'LLVM_SHA=' + PRODUCER] or lines[3] != 'tool\tpresent\tsource\tversion\tsha256':
        raise ValueError('reader requires same-producer packaged-v1 manifest')
    rows = [line.split('\t') for line in lines[4:]]
    matches = [row for row in rows if row[0] == 'llvm-readobj']
    if len(matches) != 1 or len(matches[0]) != 5 or matches[0][1:3] != ['yes', 'tuple:' + PRODUCER]:
        raise ValueError('missing same-producer llvm-readobj manifest row')
    output.mkdir(parents=True, exist_ok=True)
    result = {}
    for name, digest, version in ((linker_name, values['LLD_SHA256'], values['LLD_VERSION']),
                                  ('llvm-readobj', matches[0][4], matches[0][3])):
        compressed = (artifact / (name + '.gz')).resolve()
        if not compressed.is_relative_to(artifact):
            raise ValueError('tool payload escapes artifact')
        destination = output / (name + ('.exe' if platform.startswith('windows_') else ''))
        with gzip.open(compressed, 'rb') as stream:
            destination.write_bytes(stream.read())
        if sha(destination) != digest:
            raise ValueError('tool payload digest mismatch: ' + name)
        native(destination, platform)  # before any subprocess or system fallback
        destination.chmod(0o755)
        actual = subprocess.check_output([str(destination), '--version'], text=True).strip()
        if version not in actual:
            raise ValueError('tool version differs from manifest: ' + name)
        result['linker' if name == linker_name else 'reader'] = str(destination.resolve())
    return dict(result, producer=PRODUCER, platform=platform,
                tuple_manifest_sha256=hashes['tuple'], reader_manifest_sha256=hashes['reader'])


def configuration(build):
    commands = json.loads((Path(build) / 'compile_commands.json').read_text())
    rows = [row for row in commands if row['file'].endswith('/Heap/z/zGeneration.cpp')]
    if len(rows) != 1:
        raise ValueError('expected one actual zGeneration product compile recipe')
    import shlex
    argv = rows[0].get('arguments') or shlex.split(rows[0]['command'])
    macros = ['MRT_TESTABLE_INTERNALS', 'MRT_GC_UNIT_TESTS', 'MRT_GC_UNIT_OHOS_HOST', 'MRT_DEBUG', 'NDEBUG']
    return [name for name in macros if '-D' + name in argv or '-D' + name + '=1' in argv]


def metadata_command(tree, build, library, testbuild, linker):
    return ['cmake', '-S', str(tree / 'tests/gc_unit/metadata'), '-B', str(testbuild), '-G', 'Ninja',
            '-DCMAKE_CXX_COMPILER=clang++', '-DCMAKE_ASM_COMPILER=clang',
            '-DCMAKE_CXX_COMPILER_LAUNCHER=sccache', '-DCMAKE_ASM_COMPILER_LAUNCHER=sccache',
            '-DMANAGED_METADATA_LINKER=' + linker, '-DGCV2_RUNTIME_LIB_DIR=' + str(library),
            '-DPRODUCT_BUILD=' + str(build)]


if __name__ == '__main__':
    import argparse
    import os
    p = argparse.ArgumentParser()
    p.add_argument('--configuration', type=Path)
    a = p.parse_args()
    if a.configuration:
        print(';'.join(configuration(a.configuration)))
    else:
        dispatch(os.environ)
