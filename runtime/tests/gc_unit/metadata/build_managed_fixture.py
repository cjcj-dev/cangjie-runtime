#!/usr/bin/env python3
"""Link complete, exclusive metadata inputs using the platform Cjstart header.

ELF uses the unmodified production CJ linker script. PE/MachO use Cjstart's
indirect table slots, with relocations to this input's descriptor/map sections.
No product entry or metadata consumer is compiled into the fixture.
"""
import argparse
from pathlib import Path
import platform
import subprocess
import hashlib
import json
import os

p = argparse.ArgumentParser()
p.add_argument('--runtime', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--cc', default='clang')
p.add_argument('--linker', required=True)
p.add_argument('--target', default='')
p.add_argument('--arch', default=platform.machine())
p.add_argument('--system', default=platform.system())
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
arch = {'AMD64': 'x86_64', 'arm64': 'aarch64'}.get(a.arch, a.arch)
system = {'Linux': 'linux', 'Windows': 'windows', 'Darwin': 'macos'}[a.system]
if arch.startswith('armv'): arch = 'arm'
source = a.runtime / 'src/arch' / (arch + '_' + system) / 'Cjstart.S'
text = source.read_text()
section = '.section  .cjmetadata.rw.header' if system == 'linux' else (
    '.section  .header' if system == 'windows' else '.section  __CJMETAHEADER,__cjmetaheader')
start = text.index(section)
header = text[start:]
# MachO Cjstart has constructor arrays after the header; the fixture is never
# executed as a CJ program and registers only through the original tests.
header = header.split('.section __DATA,__mod_init_func')[0]
# ARM's old Cjstart version is a producer prerequisite, not a reason to relax
# the product ABI check. Only this private fixture emits the current ABI.
header = header.replace('0x80000000', '0x80000001')
record = {'Cjstart': str(source), 'Cjstart_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
          'system': system, 'arch': arch, 'commands': []}

def run(argv):
    result = subprocess.run([str(x) for x in argv])
    record['commands'].append({'argv': [str(x) for x in argv], 'rc': result.returncode})
    (a.output / 'fixture-build.json').write_text(json.dumps(record, indent=2))
    if result.returncode: raise SystemExit(result.returncode)

flags = ['--target=' + a.target] if a.target else []
if system == 'linux':
    (a.output / 'cjstart.S').write_text(header)
    script = a.runtime / 'build/lds' / (arch + '_linux') / 'cjld.shared.lds'
    run([a.cc, *flags, '-fPIC', '-c', a.output / 'cjstart.S', '-o', a.output / 'cjstart.o'])
    run([a.cc, *flags, '-fPIC', '-c', Path(__file__).with_name('managed_input.S'), '-o', a.output / 'managed_input.o'])
    artifact = a.output / 'libcj_managed_metadata.so'
    run([a.linker, '-shared', '-Bsymbolic', '-T', script, a.output / 'cjstart.o', a.output / 'managed_input.o',
         '-Map=' + str(a.output / 'fixture-link.map'), '-o', artifact])
else:
    prefix = '_' if system == 'macos' else ''
    symbol = lambda name: prefix + name
    label = symbol('_CJMetadataStart')
    lines = header.splitlines()
    lines.insert(1, '.globl ' + label + '\n' + label + ':')
    # The header's pointers name linker-input slots, rather than directly
    # naming the table. This is the production PE/MachO indirection contract.
    names = [line.split()[-1] for line in lines if line.strip().startswith('.quad __CJ')]
    extra = ['.balign 8']
    for name in names:
        extra += [name + ':']
        if name.endswith('Size'):
            size = (3 * (56 if system == 'macos' else 48) if name == '__CJMethodInfoSize'
                    else 296 if name == '__CJStackMapSize' else 0)
            extra += ['.long ' + str(size), '.long 0']
        else:
            address = ('ManagedMetadataDescriptors' if name == '__CJMethodInfo' else
                       'ManagedMetadataEmptyMap' if name == '__CJStackMap' else 'ManagedMetadataTableEnd')
            extra += ['.quad ' + symbol(address)]
    extra += ['.globl ' + symbol('ManagedMetadataTableEnd'), symbol('ManagedMetadataTableEnd') + ':', '.quad 0']
    (a.output / 'cjstart.S').write_text('\n'.join(lines + extra) + '\n')
    run([a.cc, *flags, '-c', a.output / 'cjstart.S', '-o', a.output / 'cjstart.o'])
    run([a.cc, *flags, '-c', Path(__file__).with_name('windows_input.S') if system == 'windows'
         else Path(__file__).with_name('managed_input.S'), '-o', a.output / 'managed_input.o'])
    if system == 'windows':
        artifact = a.output / 'cj_managed_metadata.dll'
        run([a.linker, '/dll', '/noentry', '/out:' + str(artifact), a.output / 'cjstart.o', a.output / 'managed_input.o'])
    else:
        artifact = a.output / 'libcj_managed_metadata.dylib'
        run([a.cc, *flags, '-dynamiclib', a.output / 'cjstart.o', a.output / 'managed_input.o', '-o', artifact])
record['artifact'] = str(artifact)
record['sha256'] = hashlib.sha256(artifact.read_bytes()).hexdigest()
(a.output / 'fixture-build.json').write_text(json.dumps(record, indent=2))
