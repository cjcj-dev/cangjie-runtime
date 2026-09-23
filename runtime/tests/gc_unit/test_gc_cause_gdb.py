# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe real director sampling -> driver collect -> asynchronous port result.

No rule/statistics/return value is written. Allocations and RuntimeParam come
from ProductCauseScenario. Busy-minor scenarios schedule actual product port
and worker state using their public operations, as the busy-director matrix does.
"""
import gdb
import json
import os
import time
from pathlib import Path

CASE = os.environ['CAUSE_CASE']
ROOT = Path(os.environ['CAUSE_SOURCE_ROOT'])
EXPECTED = {'major_timer': 'TIMER', 'minor_timer': 'TIMER',
            'warmup': 'WARMUP', 'high_usage': 'HIGH_USAGE',
            'allocation_rate': 'ALLOCATION_RATE', 'allocation_rate_static': 'ALLOCATION_RATE',
            'major_allocation_rate': 'ALLOCATION_RATE', 'proactive': 'PROACTIVE'}[CASE]
MINOR = CASE in ('minor_timer', 'high_usage', 'allocation_rate', 'allocation_rate_static')
RESULTS = []
DYNAMIC_CAUSES = []


def cmd(text):
    output = gdb.execute(text, to_string=True)
    if 'Python Exception' in output:
        raise RuntimeError(output)
    return output


def val(text):
    return gdb.parse_and_eval(text)


def emit(tag, **fields):
    print(tag + ' ' + json.dumps(fields, sort_keys=True), flush=True)


def line(file, text):
    return next(i + 1 for i, s in enumerate((ROOT / file).read_text().splitlines()) if text in s)


def until(spec):
    bp = gdb.Breakpoint(spec, temporary=True)
    cmd('continue')
    if bp.is_valid():
        raise RuntimeError('Unreached boundary: ' + spec)


def expect(tag, passed, **fields):
    RESULTS.append(bool(passed))
    emit(tag, passed=bool(passed), **fields)


class WarmupDelay(gdb.Breakpoint):
    def stop(self):
        if int(val('MapleRuntime::ZDriver::_major->_gc_cause')) == int(val('MapleRuntime::GC_REASON_WARMUP')):
            # Extend a real measured old cycle; do not edit its statistics.
            time.sleep(0.15)
        return False


class DynamicReturn(gdb.FinishBreakpoint):
    def __init__(self):
        self.address = int(val('this'))
        super().__init__(gdb.newest_frame(), internal=True)

    def stop(self):
        cause = int(val('((MapleRuntime::ZDriverRequest*)' + str(self.address) + ')->_cause'))
        DYNAMIC_CAUSES.append(cause)
        emit('DYNAMIC_PRODUCT_RETURN', cause=cause)
        return False


class DynamicEntry(gdb.Breakpoint):
    def stop(self):
        if 'rule_minor_allocation_rate_dynamic' in cmd('bt'):
            DynamicReturn()
        return False


try:
    for option in ('pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off'):
        cmd('set ' + option)
    fixture = 'GcDirector.ProductCauseScenario'
    cmd('set environment GC_UNIT_FILTER ' + fixture)
    cmd('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    cmd('set environment GC_UNIT_CAUSE_SCENARIO ' + CASE)
    # Reach the first allocation with the director still unscheduled.
    testfile = ROOT / '../tests/gc_unit/test_gc_director.cpp'
    tests = testfile.resolve().read_text().splitlines()
    first = next(i + 1 for i, s in enumerate(tests) if 'const size_t firstSize =' in s)
    ready = next(i + 1 for i, s in enumerate(tests) if 'CAUSE_FIRST_ALLOCATION_READY' in s)
    gdb.Breakpoint('test_gc_director.cpp:' + str(first), temporary=True)
    cmd('run')
    main = gdb.selected_thread()
    cmd('set scheduler-locking on')
    until('test_gc_director.cpp:' + str(ready))
    if CASE in ('allocation_rate', 'allocation_rate_static', 'major_allocation_rate', 'proactive'):
        if CASE == 'major_allocation_rate':
            WarmupDelay('MapleRuntime::ZDriverMajor::collect_old', internal=True)
        cmd('set scheduler-locking off')
        second = next(i + 1 for i, s in enumerate(tests) if 'const size_t nextSize =' in s)
        until('test_gc_director.cpp:' + str(second))
        emit('WARMUP_INPUT', completed=int(val('heap._old.cycleStats.warmupCycles')))
        cmd('set scheduler-locking on')
        second_ready = next(i + 1 for i, s in enumerate(tests) if 'CAUSE_SECOND_ALLOCATION_READY' in s)
        until('test_gc_director.cpp:' + str(second_ready))
    cmd('set $major = MapleRuntime::ZCollectedHeap::_collected_heap->_driver_major')
    cmd('set $minor = MapleRuntime::ZCollectedHeap::_collected_heap->_driver_minor')
    if MINOR:
        # Keep major occupied while allowing the real director to sample an
        # active old worker batch. No sampled state is edited.
        cmd('set $req = (MapleRuntime::ZDriverRequest*)malloc(sizeof(MapleRuntime::ZDriverRequest))')
        cmd('call ((void (*)(void*, unsigned int, unsigned int, unsigned int)) '
            '&_ZN12MapleRuntime14ZDriverRequestC1ENS_8GCReasonEjj)'
            '($req, MapleRuntime::GC_REASON_WARMUP, 1, 1)')
        cmd('call $major->_port.send_async(*$req)')
        cmd('call MapleRuntime::ZCollectedHeap::_collected_heap->_heap._old.workers.get()->set_active_workers(1)')
        cmd('call MapleRuntime::ZCollectedHeap::_collected_heap->_heap._old.workers.get()->set_active()')
    director = next(t for t in gdb.selected_inferior().threads() if t.name == 'ZDirector')
    director.switch()
    if 'timer' in CASE:
        time.sleep(1.1)
    until('zDirector.cpp:' + str(line('Heap/z/zDirector.cpp', 'stats.mutator_alloc_rate =')))
    product = gdb.solib_name(gdb.newest_frame().pc())
    if Path(product).resolve() != (Path(os.environ['GCV2_RUNTIME_LIB_DIR']) / 'libcangjie-runtime.so').resolve():
        raise RuntimeError('Loaded product identity mismatch: ' + str(product))
    emit('PRODUCT_IDENTITY', library=product, case=CASE, stack=cmd('bt'))
    if CASE == 'allocation_rate':
        DynamicEntry('MapleRuntime::ZDriverRequest::ZDriverRequest(MapleRuntime::GCReason, unsigned int, unsigned int)', internal=True)
    target = 'MapleRuntime::ZDriverMinor::collect' if MINOR else 'MapleRuntime::ZDriverMajor::collect'
    dispatch = gdb.Breakpoint(target, temporary=True)
    other_target = 'MapleRuntime::ZDriverMajor::collect' if MINOR else 'MapleRuntime::ZDriverMinor::collect'
    other_dispatch = gdb.Breakpoint(other_target, temporary=True)
    # Observe the next real loop boundary. Optimized inline adjust_gc source
    # lines need not execute when all rules decline the collection.
    idle = gdb.Breakpoint('MapleRuntime::ZDirector::wait_for_tick', temporary=True)
    cmd('continue')
    if dispatch.is_valid() and other_dispatch.is_valid():
        # The phase-entry cut can leave the actual port without a request.
        # Observe that product state and execute the cause assertion itself.
        port = '$minor->_port' if MINOR else '$major->_port'
        observed = int(val(port + '._message._cause'))
        expected = int(val('MapleRuntime::GC_REASON_' + EXPECTED))
        expect('ASSERT_DIRECTOR_CAUSE', False, observed=observed, expected=expected,
               dispatched=False, busy=bool(val(port + '._has_message')))
        cmd('quit 1')
    expected_driver = not dispatch.is_valid()
    actual_minor = MINOR if expected_driver else not MINOR
    if dispatch.is_valid(): dispatch.delete()
    if other_dispatch.is_valid(): other_dispatch.delete()
    if idle.is_valid(): idle.delete()
    observed = int(val('MapleRuntime::ZDriver::_major->_gc_cause'))
    expected = int(val('MapleRuntime::GC_REASON_' + EXPECTED))
    stack = cmd('bt')
    expect('ASSERT_DIRECTOR_CAUSE', observed == expected and expected_driver and 'ZDirector::run_thread' in stack,
           observed=observed, expected=expected, expected_driver=expected_driver, stack=stack)
    if CASE == 'allocation_rate':
        valid_causes = [c for c in DYNAMIC_CAUSES if c != 0xffffffff]
        expect('ASSERT_DYNAMIC_ALLOCATION_CAUSE', bool(valid_causes) and all(c == expected for c in valid_causes),
               causes=DYNAMIC_CAUSES, expected=expected)
    # This output remains observable with a producer mutation; it must not be
    # hidden by an earlier fatal assertion.
    port = '$minor->_port' if actual_minor else '$major->_port'
    async_bp = gdb.Breakpoint('MapleRuntime::ZDriverPort::send_async', temporary=True)
    sync_bp = gdb.Breakpoint('MapleRuntime::ZDriverPort::send_sync', temporary=True)
    cmd('continue')
    asynchronous = not async_bp.is_valid()
    if sync_bp.is_valid(): sync_bp.delete()
    if not asynchronous:
        expect('ASSERT_ASYNC_CAUSE_PRESERVED', False, route='send_sync', request=observed)
        cmd('quit 1')
    routed = int(val('message._cause'))
    cmd('finish')
    posted = int(val(port + '._message._cause'))
    busy = bool(val(port + '._has_message'))
    expect('ASSERT_ASYNC_CAUSE_PRESERVED', busy and posted == observed and routed == observed,
           request=observed, routed=routed, posted=posted, busy=busy)
    cmd('quit ' + ('0' if all(RESULTS) else '1'))
except Exception as error:
    emit('HARNESS_ERROR', error=repr(error))
    cmd('quit 2')
