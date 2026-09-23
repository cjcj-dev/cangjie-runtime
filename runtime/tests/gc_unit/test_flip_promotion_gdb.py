# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe the real promotion cycle without product hooks or state writes.

Run with gdb -batch -x this-file --args cj_gc_unit. Set PROMOTION_SOURCE_ROOT
(to runtime/) and GCV2_RUNTIME_LIB_DIR. The fixture calls Heap::RequestGC;
all breakpoints observe product state, never call product functions.
"""
import gdb
import json
import os
from pathlib import Path

state = {'events': [], 'field': None, 'error': None}


def emit(tag, **values):
    print(tag + ' ' + json.dumps(values, sort_keys=True), flush=True)


def event(name):
    state['events'].append(name)
    emit('PROMOTION_EVENT', name=name, sequence=len(state['events']))


def product_identity():
    actual = Path(gdb.solib_name(gdb.newest_frame().pc())).resolve()
    expected = (Path(os.environ['GCV2_RUNTIME_LIB_DIR']) / 'libcangjie-runtime.so').resolve()
    if actual != expected:
        raise RuntimeError('Unexpected product SO: ' + str(actual))
    return str(actual)


class Observe(gdb.Breakpoint):
    def __init__(self, spec, action):
        super().__init__(spec, internal=True)
        self.action = action

    def stop(self):
        try:
            return self.action()
        except Exception as error:
            state['error'] = repr(error)
            return True


class Completed(gdb.FinishBreakpoint):
    def __init__(self, name, function):
        frame = gdb.newest_frame()
        while frame is not None and function not in (frame.name() or ''):
            frame = frame.older()
        if frame is None:
            raise RuntimeError('Missing product frame: ' + function)
        super().__init__(frame=frame, internal=True)
        self.name = name

    def stop(self):
        event(self.name)
        return False


def capture_field():
    state['field'] = int(gdb.parse_and_eval('fields'))
    emit('PROMOTION_INPUT', field=state['field'], raw=int(gdb.parse_and_eval('*(unsigned long*)fields')))
    return False


def flip():
    if state['field'] is not None:
        product_identity()
        Completed('flip_completed', 'flip_promote')
    return False


def handshake():
    name = gdb.parse_and_eval('cl->name_').string()
    emit('HANDSHAKE_OBSERVED', name=name, field=state['field'])
    if state['field'] is not None and name == 'ZRendezvous':
        product_identity()
        Completed('rendezvous_completed', 'Handshake::execute')
    return False


def barrier():
    if state['field'] is not None:
        product_identity()
        event('barrier_work')
    return False


def before_relocate():
    if state['field'] is None:
        return False
    library = product_identity()
    word = int(gdb.parse_and_eval('*(unsigned long*)' + str(state['field'])))
    nonnull = int(gdb.parse_and_eval('*(unsigned long*)' + str(state['field'] + 8)))
    bad = int(gdb.parse_and_eval('g_cjStoreBadMask'))
    events = state['events']
    flips = [i for i, name in enumerate(events) if name == 'flip_completed']
    rendezvous = [i for i, name in enumerate(events) if name == 'rendezvous_completed']
    barriers = [i for i, name in enumerate(events) if name == 'barrier_work']
    good = word != 0 and word & bad == 0
    ordered = bool(flips and rendezvous and barriers and max(flips) < min(rendezvous) < min(barriers))
    emit('ASSERT_PROMOTED_NULL_STORE_GOOD', passed=good, raw=word, bad_mask=bad, library=library)
    emit('ASSERT_FLIP_RENDEZVOUS_BARRIER_ORDER', passed=ordered, events=events)
    control = nonnull != 0 and nonnull & bad == 0
    emit('ASSERT_MARKED_NONNULL_CONTROL', passed=control, raw=nonnull, bad_mask=bad)
    emit('PROMOTION_PRODUCT_STACK', stack=gdb.execute('bt', to_string=True))
    state['passed'] = good and ordered and control
    return True


try:
    for setting in ['pagination off', 'confirm off', 'breakpoint pending on',
                    'print thread-events off', 'follow-fork-mode child']:
        gdb.execute('set ' + setting)
    fixture = 'FlipPromotion.NullFieldsStayColoredThroughCollection'
    gdb.execute('set environment GC_UNIT_FILTER ' + fixture)
    gdb.execute('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    root = Path(os.environ['PROMOTION_SOURCE_ROOT'])
    test_lines = (root / 'tests/gc_unit/test_segmented_array_init.cpp').read_text().splitlines()
    capture = next(i + 1 for i, line in enumerate(test_lines) if 'heap.RequestGC(promote ?' in line)
    relocate_lines = (root / 'src/Heap/z/zRelocate.cpp').read_text().splitlines()
    task_start = next(i for i, line in enumerate(relocate_lines) if 'class ZPromoteBarrierTask' in line)
    work = next(i + 1 for i in range(task_start, len(relocate_lines)) if 'SuspendibleThreadSetJoiner stsJoiner' in relocate_lines[i])
    gdb.execute('start')
    Observe('test_segmented_array_init.cpp:' + str(capture), capture_field)
    Observe('MapleRuntime::ZGenerationYoung::flip_promote', flip)
    Observe('MapleRuntime::Handshake::execute(MapleRuntime::HandshakeClosure*)', handshake)
    Observe('zRelocate.cpp:' + str(work), barrier)
    Observe('MapleRuntime::ZGenerationYoung::pause_relocate_start', before_relocate)
    gdb.execute('continue')
    if state['error'] or 'passed' not in state:
        raise RuntimeError(state['error'] or 'Product relocate-start boundary not reached')
    gdb.execute('quit ' + ('0' if state['passed'] else '1'))
except Exception as error:
    emit('HARNESS_ERROR', error=repr(error))
    gdb.execute('quit 2')
