"""Observe real compiler byte-array ABI through the actual allocation consumer.

Run with WIDTH_CASE=below|equal|above|small and WIDTH_EVIDENCE_DIR set. Boundary
cases stop before allocating their multi-GiB extent. No inferior values are
changed. This proves width wiring, not complete boundary initialization.
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


class ArrayEntry(gdb.Breakpoint):
    def stop(self):
        global pending
        try:
            length = int(gdb.parse_and_eval('nElems'))
        except gdb.error:
            length = int(gdb.parse_and_eval('$rsi'))
        if length < (1 << 31) and not (case == 'small' and length == 17):
            return False
        header = int(gdb.lookup_type('MapleRuntime::MArray').sizeof)
        pending = {'length': length, 'element_bytes': 1, 'header_bytes': header,
                   'expected_bytes': length + header}
        print('RAW_ARRAY_PRODUCER_TARGET ' + json.dumps(pending, sort_keys=True))
        return False


class AllocationEntry(gdb.Breakpoint):
    def stop(self):
        global pending
        if pending is None:
            return False
        try:
            actual = int(gdb.parse_and_eval('allocSize'))
        except gdb.error:
            actual = int(gdb.parse_and_eval('$rdi'))
        record = {**pending, 'actual_bytes': actual, 'valid': actual == pending['expected_bytes']}
        observed.append(record)
        (out / 'width-result.json').write_text(json.dumps(observed, indent=2) + '\n')
        print('RAW_ARRAY_WIDTH_TARGET ' + json.dumps(record, sort_keys=True))
        capture_identity()
        pending = None
        if case != 'small' or not record['valid']:
            gdb.execute('kill')
            gdb.execute('quit %d' % (0 if record['valid'] else 1))
        return False


gdb.execute('set pagination off')
gdb.execute('set confirm off')
gdb.execute('set breakpoint pending on')
ArrayEntry('MCC_NewArray8')
AllocationEntry('MapleRuntime::HeapManager::Allocate')
gdb.execute('run ' + case)
if not observed:
    print('RAW_ARRAY_WIDTH_TARGET missing=true')
    gdb.execute('quit 2')
exit_code = gdb.parse_and_eval('$_exitcode')
gdb.execute('quit %d' % (0 if int(exit_code) == 0 and all(r['valid'] for r in observed) else 1))
