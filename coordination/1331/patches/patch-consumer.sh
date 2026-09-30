#!/bin/bash
# Consumer cut: product serial ZPageTableIterator::next drops 3-granule pages.
# ZGC zPageTable.inline.hpp:57-68 is the load-bearing filter.
set -e
f="$1/runtime/src/Heap/z/zPageTable.inline.hpp"
python3 - "$f" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text()
old = "        if (entry != nullptr && entry != _prev) {"
new = "        if (entry != nullptr && entry != _prev && entry->size() != 3 * ZGranuleSize) {"
assert s.count(old) == 1, s.count(old)
p.write_text(s.replace(old, new))
PY
