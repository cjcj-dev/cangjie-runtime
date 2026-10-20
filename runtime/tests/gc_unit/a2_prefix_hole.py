#!/usr/bin/env python3
# Derive one negative input from the real contiguous fixture, without changing
# mapped bytes. The middle two prefix bytes remain page-readable but fall
# outside both exact PT_LOAD registrations.
import pathlib
import struct
import sys

source, output = map(pathlib.Path, sys.argv[1:])
data = bytearray(source.read_bytes())
if data[:6] != b"\x7fELF\x02\x01":
    raise ValueError("fixture requires little-endian ELF64")
phoff = struct.unpack_from("<Q", data, 32)[0]
entsize, count = struct.unpack_from("<HH", data, 54)
changed = []
for index in range(count):
    pos = phoff + index * entsize
    kind, flags, offset, vaddr, paddr, filesz, memsz, align = struct.unpack_from("<IIQQQQQQ", data, pos)
    if kind != 1:
        continue
    if vaddr == 0x400000:
        if (flags, filesz, memsz) != (4, 4096, 4096):
            raise ValueError("unexpected left LOAD")
        filesz -= 1
        memsz -= 1
        changed.append("left")
    elif vaddr == 0x401000:
        if flags != 5 or filesz < 34 or filesz != memsz:
            raise ValueError("unexpected right LOAD")
        offset += 1
        vaddr += 1
        paddr += 1
        filesz -= 1
        memsz -= 1
        changed.append("right")
    else:
        continue
    struct.pack_into("<IIQQQQQQ", data, pos, kind, flags, offset, vaddr, paddr, filesz, memsz, align)
if changed != ["left", "right"]:
    raise ValueError("expected exactly the contiguous fixture LOAD pair")
output.write_bytes(data)
print("A2_HOLE_LOADS left=[0x400000,0x400fff) right=[0x401001,...); prefix=0x400ffe size=4")
