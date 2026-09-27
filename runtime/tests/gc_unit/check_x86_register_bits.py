#!/usr/bin/env python3
"""Compare the runtime register numbers with the paired LLVM emitter table."""
import argparse
import hashlib
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--llvm-stackmaps', type=Path, required=True)
args = parser.parse_args()
header = Path(__file__).resolve().parents[2] / 'src/Common/RegisterX86-64.h'
emitter = args.llvm_stackmaps.read_text()
runtime = header.read_text()
block = re.search(r'X86Bit2Reg\s*=\s*\{(.*?)\};', emitter, re.S).group(1)
expected = re.findall(r'"([^"]+)"', block)
block = re.search(r'enum RegisterId\s*:\s*uint32_t\s*\{(.*?)REGISTERS_COUNT', runtime, re.S).group(1)
actual = [name.strip().lower() for name in block.split(',') if name.strip()]
for path in (args.llvm_stackmaps, header):
    print(f'sha256={hashlib.sha256(path.read_bytes()).hexdigest()} path={path}')
for bit, (producer, consumer) in enumerate(zip(expected, actual)):
    print(f'X86_BIT_TARGET bit={bit} producer={producer} consumer={consumer} match={producer == consumer}')
assert actual == expected, (expected, actual)
print(f'X86_BIT_TABLE_MATCH count={len(actual)}')
