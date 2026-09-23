"""Observe the real director's soft-capacity sample and warmup threshold.

Run with gdb -nx -batch -ex 'source test_soft_max_gdb.py' <gc-unit-ELF>.
Requires DIRECTOR_SOURCE and GCV2_RUNTIME_LIB_DIR. No product state is written.
"""
import gdb
import json
import os
from pathlib import Path

try:
    for setting in ('pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off'):
        gdb.execute('set ' + setting)
    fixture = 'GcDirector.ProductWarmupStopsAfterThreeCycles'
    gdb.execute('set environment GC_UNIT_FILTER ' + fixture)
    gdb.execute('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    gdb.execute('set environment cjSoftMaxHeapSize 32M')
    source = Path(os.environ['DIRECTOR_SOURCE']).read_text().splitlines()
    start = next(i for i, line in enumerate(source) if 'static bool rule_major_warmup(' in line)
    target = next(i + 1 for i in range(start + 1, len(source))
                  if 'return stats.heap.used >= used_threshold;' in source[i])
    bp = gdb.Breakpoint('zDirector.cpp:' + str(target), temporary=True)
    gdb.execute('run')
    if bp.is_valid():
        raise RuntimeError('Director warmup consumer was not reached')
    library = gdb.solib_name(gdb.newest_frame().pc())
    if Path(library).resolve() != Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so').resolve():
        raise RuntimeError('Unexpected product library: ' + str(library))
    observed = int(gdb.parse_and_eval('stats.heap.soft_max_heap_size'))
    cycles = int(gdb.parse_and_eval('stats.old_stats.cycle.warmupCycles'))
    threshold = int(gdb.parse_and_eval('used_threshold'))
    expected_soft = 32 * 1024 * 1024
    expected_threshold = int(expected_soft * ((cycles + 1) * 0.1))
    passed = observed == expected_soft and threshold == expected_threshold
    print('SOFT_MAX_DIRECTOR_ASSERT ' + json.dumps(dict(
        soft=observed, expected_soft=expected_soft, threshold=threshold,
        expected_threshold=expected_threshold, cycles=cycles, passed=passed,
        library=library, stack=gdb.execute('bt', to_string=True)), sort_keys=True), flush=True)
    gdb.execute('quit ' + ('0' if passed else '1'))
except Exception as error:
    print('SOFT_MAX_HARNESS_ERROR ' + str(error), flush=True)
    gdb.execute('quit 2')
