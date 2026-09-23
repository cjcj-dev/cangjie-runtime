# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Schedule a real allocation stall after director sampling, then observe its request.

No director input, allocator queue, clock, or return value is overwritten. GDB
only schedules existing threads; RuntimeParam inputs are set before initialization.
"""
import gdb
import json
import os
import time
from pathlib import Path

stall = os.environ.get('STALL_AFTER_SAMPLE', '1') == '1'


def cmd(command):
    return gdb.execute(command, to_string=True)


def val(expression):
    return gdb.parse_and_eval(expression)


def emit(tag, **fields):
    print(tag + ' ' + json.dumps(fields, sort_keys=True), flush=True)


def line_in(lines, function, text):
    start = next(i for i, line in enumerate(lines) if function in line)
    return next(i + 1 for i in range(start + 1, len(lines)) if text in lines[i])


def until(spec):
    bp = gdb.Breakpoint(spec, temporary=True)
    cmd('continue')
    if bp.is_valid():
        raise RuntimeError('Boundary not reached: ' + spec)


try:
    source = Path(os.environ['DIRECTOR_SOURCE']).read_text().splitlines()
    fixture_source = Path(os.environ['STALL_FIXTURE_SOURCE']).read_text().splitlines()
    for setting in ('pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off'):
        cmd('set ' + setting)
    fixture = 'GcDirector.OldStallSuppressesMinorAllocationRate'
    cmd('set environment GC_UNIT_FILTER ' + fixture)
    cmd('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    init = line_in(fixture_source, 'void CheckDirectorStallGate', 'GC_EXPECT_EQ(InitCJRuntime')
    gdb.Breakpoint('test_allocation_stall_queue.cpp:' + str(init), temporary=True)
    cmd('run')
    cmd('set var params.gcParam.backupGCInterval=1')
    cmd('set var params.gcParam.concGCThreads=4')
    cmd('set var params.gcParam.youngGCThreads=4')
    cmd('set var params.gcParam.oldGCThreads=4')
    bp = gdb.Breakpoint('MapleRuntime::RegionManager::ClaimCapacityOrStall', temporary=True)
    bp.condition = '((MapleRuntime::ZPageAllocation*)$rsi)->size >= 67108864'
    cmd('continue')
    if bp.is_valid():
        raise RuntimeError('Real allocation request not reached')
    waiter = gdb.selected_thread()
    emit('REAL_ALLOCATION', stack=cmd('bt'), arguments=cmd('info args'))
    cmd('set scheduler-locking on')
    director = next(t for t in gdb.selected_inferior().threads() if t.name == 'ZDirector')
    director.switch()
    tick = line_in(source, 'static ZDirectorStats sample_stats', 'const uint64_t now')
    until('zDirector.cpp:' + str(tick))
    time.sleep(1.1)
    end = line_in(source, 'static GCReason make_major_gc_decision', 'if (')
    until('zDirector.cpp:' + str(end))
    library = gdb.solib_name(gdb.newest_frame().pc())
    expected = Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so')
    if Path(library).resolve() != expected.resolve():
        raise RuntimeError('Unexpected product library: ' + str(library))
    emit('SAMPLED_BEFORE_ENQUEUE', library=library, stack=cmd('bt'))
    if stall:
        waiter.switch()
        until('MapleRuntime::RegionManager::StallAllocation')
        emit('REAL_ENQUEUE_RESULT', arguments=cmd('info args'), stack=cmd('bt'))
    director.switch()
    send = line_in(source, 'static void start_minor_gc', 'driver_minor()->port().send_async')
    until('zDirector.cpp:' + str(send))
    emit('PRODUCT_SELECTION', young=str(val('selection.young_workers')), old=str(val('selection.old_workers')))
    cmd('next')
    port = 'MapleRuntime::ZCollectedHeap::_collected_heap->_driver_minor->_port'
    actual = int(val(port + '._message._young_nworkers'))
    expected_workers = int(val('MapleRuntime::ZYoungGCThreads'))
    passed = actual == expected_workers if stall else actual < expected_workers
    emit('ASSERT_STALL_AFTER_SAMPLE_BOOSTS_REQUEST', actual=actual, limit=expected_workers, stall=stall, passed=passed)
    cmd('quit ' + ('0' if passed else '1'))
except Exception as error:
    emit('HARNESS_ERROR', error=str(error))
    cmd('quit 2')
