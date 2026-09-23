# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe elapsed-time normalization on the real director thread.

Use the standalone ProductWarmupStopsAfterThreeCycles fixture and a product SO
with debug information. A debugger scheduling stop holds a real young worker
batch after accounting starts. No statistics, arguments or return values are
written, and no director function is called by the debugger.
"""
import gdb
import json
import os
import time
from pathlib import Path


def cmd(command):
    output = gdb.execute(command, to_string=True)
    if command in ('run', 'continue', 'start'):
        print(output, flush=True)
    return output


def value(expression):
    return gdb.parse_and_eval(expression)


def emit(tag, **fields):
    print(tag + ' ' + json.dumps(fields, sort_keys=True), flush=True)


def until(spec, condition=None):
    bp = gdb.Breakpoint(spec, temporary=True)
    if condition:
        bp.condition = condition
    cmd('continue')
    if bp.is_valid():
        raise RuntimeError('Boundary not reached: ' + spec)


try:
    for setting in ['pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off']:
        cmd('set ' + setting)
    fixture = 'GcDirector.ProductWarmupStopsAfterThreeCycles'
    cmd('set environment GC_UNIT_FILTER ' + fixture)
    cmd('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    root = Path(os.environ['DIRECTOR_SOURCE_ROOT'])
    director_source = (root / 'zDirector.cpp').read_text().splitlines()
    major_line = next(i + 1 for i, s in enumerate(director_source) if 'initial_workers(stats, ZWorkerSelectionType::start_major)' in s)
    cmd('start')
    bp = gdb.Breakpoint('zDirector.cpp:' + str(major_line))
    bp.condition = 'stats.old_stats.cycle.isTimeTrustable'
    cmd('continue')
    bp.delete()
    if not bool(value('stats.old_stats.cycle.isTimeTrustable')):
        raise RuntimeError('No completed cycle before the observed young batch')
    root = Path(os.environ['DIRECTOR_SOURCE_ROOT'])
    source = (root / 'zWorkers.cpp').read_text().splitlines()
    line = next(i + 1 for i, s in enumerate(source) if '_workers.run_task(task->worker_task())' in s)
    until('zWorkers.cpp:' + str(line), "_generation_name[0] == 'Y'")
    emit('REAL_YOUNG_BATCH', stack=cmd('bt'))
    # The worker accounting lock has been released at this boundary. Other
    # threads remain stopped while the director samples the active batch.
    cmd('set scheduler-locking on')
    director = next(t for t in gdb.selected_inferior().threads() if t.name == 'ZDirector')
    director.switch()
    time.sleep(0.05)
    until('MapleRuntime::rule_minor_allocation_rate_dynamic(MapleRuntime::ZDirectorStats const&, double, double, bool, unsigned long)')
    stack = cmd('bt')
    if 'adjust_gc' not in stack or 'ZDirector::run_thread' not in stack:
        raise RuntimeError('Observed call did not traverse the real adjustment entry: ' + stack)
    library = gdb.solib_name(gdb.newest_frame().pc())
    expected_library = Path(os.environ['GCV2_RUNTIME_LIB_DIR']) / 'libcangjie-runtime.so'
    if Path(library).resolve() != expected_library.resolve():
        raise RuntimeError('Product identity mismatch: ' + str(library))
    sampled_serial = float(value('stats.young_stats.resize.serial_gc_time_passed'))
    sampled_parallel = float(value('stats.young_stats.resize.parallel_gc_time_passed'))
    actual_serial = float(value('serial_gc_time_passed'))
    actual_parallel = float(value('parallel_gc_time_passed'))
    emit('PRODUCT_INPUT', library=library, stack=stack,
         sampled_serial=sampled_serial, sampled_parallel=sampled_parallel,
         dynamic_serial=actual_serial, dynamic_parallel=actual_parallel)
    # The fixture validity check is distinct from the target assertion.
    if sampled_parallel <= 0.0 or sampled_serial <= 0.0:
        raise RuntimeError('The real in-flight batch must produce both elapsed times')
    result_line = next(i + 1 for i, s in enumerate(director_source) if 'if (time_until_gc > time_until_oom * 0.05)' in s)
    until('zDirector.cpp:' + str(result_line))
    serial_time = float(value('serial_gc_time'))
    expected_serial = abs(float(value('stats.young_stats.cycle.serialTime')) +
                          float(value('stats.young_stats.cycle.serialTimeSd')) * 3.290527)
    serial_passed = actual_serial == 0.0 and abs(serial_time - expected_serial) < 1e-12
    parallel_passed = actual_parallel == 0.0
    emit('ASSERT_DIRECTOR_SERIAL_ELAPSED_IGNORED', passed=serial_passed,
         serial=actual_serial, computed_serial_time=serial_time,
         expected_serial_time=expected_serial)
    emit('ASSERT_DIRECTOR_PARALLEL_ELAPSED_IGNORED', passed=parallel_passed,
         parallel=actual_parallel)
    passed = serial_passed and parallel_passed
    cmd('quit ' + ('0' if passed else '1'))
except Exception as error:
    emit('HARNESS_ERROR', error=repr(error))
    cmd('quit 2')
