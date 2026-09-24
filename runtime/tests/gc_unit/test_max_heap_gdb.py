"""Read the exact byte flag between product argument parsing and GC alignment.

Run with gdb -nx -batch -ex 'source test_max_heap_gdb.py' <gc-unit-ELF>.
GCV2_RUNTIME_LIB_DIR selects the product SO; MAX_HEAP_FIXTURE selects one of
our real startup tests. The debugger reads state and never changes it.
"""
import gdb
import hashlib
import json
import os
from pathlib import Path

try:
    for setting in ('pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off'):
        gdb.execute('set ' + setting)
    fixture = os.environ.get('MAX_HEAP_FIXTURE', 'MaxHeapSize.ExactBytes')
    expected = {'MaxHeapSize.ExactBytes': 67108865,
                'MaxHeapSize.ApiEnvironmentBytes': 67108865,
                'MaxHeapSize.ApiKilobytes': 65537 * 1024}[fixture]
    gdb.execute('set environment GC_UNIT_FILTER ' + fixture)
    gdb.execute('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    bp = gdb.Breakpoint('MapleRuntime::GCArguments::initialize_heap_flags_and_sizes', temporary=True)
    gdb.execute('run')
    if bp.is_valid():
        raise RuntimeError('Product GC size initialization was not reached')
    library = Path(gdb.solib_name(gdb.newest_frame().pc())).resolve()
    if library != Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so').resolve():
        raise RuntimeError('Unexpected product library: ' + str(library))
    actual = int(gdb.parse_and_eval("'MapleRuntime::(anonymous namespace)::g_maxHeapSize'"))
    passed = actual == expected
    print('MAX_PARSE_BYTES_ASSERT ' + json.dumps(dict(
        fixture=fixture, actual=actual, expected=expected, passed=passed,
        library=str(library), so_sha256=hashlib.sha256(library.read_bytes()).hexdigest(),
        stack=gdb.execute('bt', to_string=True)), sort_keys=True), flush=True)
    gdb.execute('quit ' + ('0' if passed else '1'))
except Exception as error:
    print('MAX_PARSE_HARNESS_ERROR ' + str(error), flush=True)
    gdb.execute('quit 2')
