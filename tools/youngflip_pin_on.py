#!/usr/bin/env python3
"""Flip kYoungConcMark to true on a src copy. Audit/terminate stay product-off."""
import pathlib
import sys

src = pathlib.Path(sys.argv[1])
path = src / "runtime/src/Heap/Allocator/RegionSpace.h"
text = path.read_text()
old = "constexpr bool kYoungConcMark = false;"
new = "constexpr bool kYoungConcMark = true; /* youngflip measure: MARK on */"
n = text.count(old)
if n != 1:
    raise SystemExit(f"kYoungConcMark needle count={n} in {path}")
path.write_text(text.replace(old, new, 1))
print(f"PINNED kYoungConcMark=true audit=off terminate=off at {src}")
