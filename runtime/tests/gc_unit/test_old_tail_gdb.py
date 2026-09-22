# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe the real director decision while the real old driver is at relock.

Run on x86_64 with the gc_unit ELF. Scheduler locking holds the old driver at
its actual lock entry; the director independently samples the product workers.
No statistics, product instructions, or return values are changed.
"""
import gdb
import json
import os
import time
from pathlib import Path


def cmd(s):
    return gdb.execute(s, to_string=True)


def val(s):
    return gdb.parse_and_eval(s)


def emit(tag, **data):
    print(tag + ' ' + json.dumps(data, sort_keys=True))


def until(spec):
    bp = gdb.Breakpoint(spec, temporary=True)
    cmd('continue')
    if bp.is_valid():
        raise RuntimeError('Boundary not reached: ' + spec)


class Tail(gdb.Breakpoint):
    def stop(self):
        return gdb.selected_thread().name == 'ZDriverMajor'


try:
    for setting in ['pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off']:
        cmd('set ' + setting)
    fixture = 'GcDirector.ProductWarmupStopsAfterThreeCycles'
    cmd('set environment GC_UNIT_FILTER ' + fixture)
    cmd('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    gdb.Breakpoint('test_gc_director.cpp:121', temporary=True)
    cmd('run')
    cmd('set var params.gcParam.backupGCInterval=1')
    cmd('set var params.gcParam.concGCThreads=2')
    cmd('set var params.gcParam.youngGCThreads=2')
    cmd('set var params.gcParam.oldGCThreads=2')
    product = gdb.solib_name(int(val('(void*)&_ZN12MapleRuntime9ZDirector10run_threadEv')))
    if Path(product).resolve() != Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so').resolve():
        raise RuntimeError('Product identity mismatch')
    emit('PRODUCT_IDENTITY', library=product)
    bp = Tail('MapleRuntime::ZGenerationOld::concurrent_relocate()')
    cmd('continue')
    bp.delete()
    old = gdb.selected_thread()
    cmd('finish')
    # At this point the concurrent relocation returned. The next lock is the
    # scope exit's relock, after destructor body in the fixed implementation.
    cmd('set scheduler-locking on')
    until('MapleRuntime::ZDriver::lock()')
    stack = cmd('bt')
    emit('OLD_RELOCK', stack=stack, thread=old.num)
    if 'ZGenerationOld::collect' not in stack:
        raise RuntimeError('Not the collection relock')
    director = next(t for t in gdb.selected_inferior().threads() if t.name == 'ZDirector')
    director.switch()
    source = Path(os.environ['DIRECTOR_SOURCE']).read_text().splitlines()
    tick = next(i+1 for i,s in enumerate(source) if 'const ZDirectorStats stats = sample_stats' in s)
    until('zDirector.cpp:' + str(tick))
    # Let the public timer expire before the real sample obtains its timestamp.
    time.sleep(1.1)
    until('MapleRuntime::make_minor_gc_decision(MapleRuntime::ZDirectorStats const&)')
    resize = bool(val('stats.old_stats.resize.is_active'))
    emit('PRODUCT_SAMPLE', old_active=resize, thread=director.num)
    # The validation build must retain a real return value. An inlined enum
    # that the optimizer eliminated is not represented by an arbitrary ABI
    # register; reject that unsupported observation rather than infer it.
    frame = gdb.newest_frame()
    if frame.type() == gdb.INLINE_FRAME:
        raise RuntimeError('Decision return optimized away; use the approved product debug build')
    result = gdb.FinishBreakpoint(frame, internal=True)
    cmd('continue')
    if result.return_value is None:
        raise RuntimeError('No observable decision return value')
    actual = int(result.return_value)
    invalid = int(val('MapleRuntime::GC_REASON_INVALID'))
    passed = actual == invalid
    emit('ASSERT_OLD_TAIL_NO_MINOR', actual=actual, expected=invalid, passed=passed,
         sampled_old_active=resize)
    cmd('quit ' + ('0' if passed else '1'))
except Exception as error:
    emit('HARNESS_ERROR', error=str(error))
    cmd('quit 2')
