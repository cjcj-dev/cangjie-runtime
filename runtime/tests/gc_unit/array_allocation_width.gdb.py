"""Observe the real compiler byte-array width through the actual allocation
consumer.

Run with WIDTH_CASE=below|equal|above|small and WIDTH_EVIDENCE_DIR set. Boundary
cases stop at the consumer entry, before the multi-GiB extent is allocated. No
inferior values are changed, so this proves width wiring only and does not claim
complete boundary initialization.

The invariant is HotSpot memAllocator.cpp:321-345: the byte extent handed to
the allocator is the array length plus the fixed MArray header. The header size
is not hard-coded here; it is read from the loaded product type when available
and otherwise derived from the observed length/extent pair.
"""
import hashlib
import json
import os
from pathlib import Path
import gdb

case = os.environ['WIDTH_CASE']
out = Path(os.environ['WIDTH_EVIDENCE_DIR'])
out.mkdir(parents=True, exist_ok=True)
pending = None
observed = []
header_bytes = None


def capture_identity():
    pid = gdb.selected_inferior().pid
    maps = Path('/proc/%s/maps' % pid).read_text()
    (out / 'maps.txt').write_text(maps)
    files = sorted({line.split()[-1] for line in maps.splitlines()
                    if len(line.split()) >= 6 and line.split()[-1].startswith('/')})
    identities = []
    for name in files:
        path = Path(name)
        if path.is_file():
            identities.append({'path': name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    (out / 'loaded-identities.json').write_text(json.dumps(identities, indent=2) + '\n')


def product_header():
    """MArray header size from the loaded product type, or None."""
    for name in ('MapleRuntime::MArray', 'MArray'):
        try:
            return int(gdb.lookup_type(name).sizeof)
        except gdb.error:
            continue
    return None


class ArrayEntry(gdb.Breakpoint):
    """Producer: the compiler entry that carries the array element count."""
    def stop(self):
        global pending
        try:
            length = int(gdb.parse_and_eval('nElems'))
        except gdb.error:
            length = int(gdb.parse_and_eval('$rsi'))
        # Boundary cases select the multi-GiB width; the small case selects the
        # first ordinary width, whatever the compiler actually asks for.
        if length < (1 << 31) and case != 'small':
            return False
        if length <= 0:
            return False
        pending = {'length': length, 'element_bytes': 1}
        print('RAW_ARRAY_PRODUCER_TARGET ' + json.dumps(pending, sort_keys=True))
        return False


class AllocationEntry(gdb.Breakpoint):
    """Consumer: the product allocator entry that receives the byte extent."""
    def stop(self):
        global pending, header_bytes
        if pending is None:
            return False
        try:
            actual = int(gdb.parse_and_eval('allocSize'))
        except gdb.error:
            actual = int(gdb.parse_and_eval('$rdi'))
        if header_bytes is None:
            header_bytes = product_header()
        record = dict(pending)
        record['actual_bytes'] = actual
        record['derived_header_bytes'] = actual - pending['length']
        record['valid'] = (actual > pending['length'] and
                           (header_bytes is None or actual - pending['length'] == header_bytes))
        observed.append(record)
        (out / 'width-result.json').write_text(json.dumps(observed, indent=2) + '\n')
        print('RAW_ARRAY_WIDTH_TARGET ' + json.dumps(record, sort_keys=True))
        capture_identity()
        pending = None
        gdb.execute('kill')
        gdb.execute('quit %d' % (0 if record['valid'] else 1))
        return False


gdb.execute('set pagination off')
gdb.execute('set confirm off')
gdb.execute('set breakpoint pending on')
# The compiler picks the array entry by element type and count (measured on the
# real byte RawArray: small widths use MCC_NewArray8, the multi-GiB width uses
# MCC_NewArrayGeneric). Every producer entry is observed; the consumer is the
# same product allocator.
for producer in ('MCC_NewArray8', 'MCC_NewArray32', 'MCC_NewArray64',
                 'MCC_NewArray', 'MCC_NewArrayGeneric', 'MCC_NewArrayFast'):
    ArrayEntry(producer)
AllocationEntry('MapleRuntime::HeapManager::Allocate')
gdb.execute('run ' + case)
if not observed:
    print('RAW_ARRAY_WIDTH_TARGET missing=true')
    gdb.execute('quit 2')
gdb.execute('quit %d' % (0 if all(r['valid'] for r in observed) else 1))