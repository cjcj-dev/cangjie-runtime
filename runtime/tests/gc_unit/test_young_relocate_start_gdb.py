# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Read the young queue at real collect boundaries in the product SO.

Run with gdb -batch -x this-file --args cj_gc_unit. GCV2_RUNTIME_LIB_DIR
must name the tested product directory. No product state is modified.
"""
import gdb
import json
import os
from pathlib import Path


def emit(tag, **values):
    print(tag + ' ' + json.dumps(values, sort_keys=True))


def cmd(command):
    return gdb.execute(command, to_string=True)


def queue_active():
    # libstdc++ unique_ptr has one pointer; read memory without calling into
    # the stopped process (and without relying on optimized inline accessors).
    relocate = '*((MapleRuntime::ZRelocate**)&MapleRuntime::ZGeneration::_young->_relocate)'
    return bool(gdb.parse_and_eval('(' + relocate + ')->relocateQueue.isActive._M_base._M_i'))


def reach(symbol):
    bp = gdb.Breakpoint(symbol, temporary=True)
    cmd('continue')
    if bp.is_valid():
        raise RuntimeError('Product boundary not reached: ' + symbol)
    frame = gdb.newest_frame()
    actual = Path(gdb.solib_name(frame.pc())).resolve()
    expected = Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so').resolve()
    if actual != expected:
        raise RuntimeError('Product identity mismatch: ' + str(actual))
    stack = cmd('bt 12')
    if 'ZGenerationYoung::collect' not in stack:
        raise RuntimeError('Boundary did not run through product collect')
    emit('PRODUCT_BOUNDARY', symbol=symbol, library=str(actual), stack=stack)


try:
    for setting in ['pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off']:
        cmd('set ' + setting)
    fixture = 'GcDirector.ProductWarmupStopsAfterThreeCycles'
    cmd('set environment GC_UNIT_FILTER ' + fixture)
    cmd('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    cmd('start')
    reach('MapleRuntime::ZGenerationYoung::concurrent_mark_free()')
    before = queue_active()
    emit('ASSERT_MARK_END_QUEUE_INACTIVE', active=before, passed=not before)
    reach('MapleRuntime::ZGenerationYoung::concurrent_relocate()')
    after = queue_active()
    # Keep the precondition nonfatal: the target is always observed separately.
    emit('ASSERT_YOUNG_PAUSE_QUEUE_ACTIVE', active=after, passed=after)
    emit('OBSERVATION_COMPLETE', before=before, after=after)
    cmd('quit ' + ('0' if not before and after else '1'))
except Exception as error:
    emit('HARNESS_ERROR', error=repr(error))
    cmd('quit 2')
