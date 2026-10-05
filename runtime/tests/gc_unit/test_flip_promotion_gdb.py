# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe the real promotion cycle without product hooks or state writes.

Run with gdb -batch -x this-file --args cj_gc_unit. Set PROMOTION_SOURCE_ROOT
(to runtime/) and GCV2_RUNTIME_LIB_DIR. The fixture calls Heap::RequestGC;
all breakpoints observe product state, never call product functions.
"""
import gdb
import hashlib
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
    if not state.get('identity_observed'):
        libraries = {}
        for name in ('libcangjie-runtime.so', 'libboundscheck.so'):
            wanted = (expected.parent / name).resolve()
            loaded = {Path(obj.filename).resolve() for obj in gdb.objfiles()
                      if Path(obj.filename).name == name}
            if loaded != {wanted}:
                raise RuntimeError('Unexpected loaded libraries: ' + repr(loaded))
            libraries[name] = {'path': str(wanted),
                               'sha256': hashlib.sha256(wanted.read_bytes()).hexdigest()}
        emit('PROMOTION_LOADED_IDENTITY', libraries=libraries,
             pid=gdb.selected_inferior().pid,
             mappings=gdb.execute('info proc mappings', to_string=True))
        state['identity_observed'] = True
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


class Completed(gdb.Breakpoint):
    def __init__(self, name, function, action=None):
        frame = gdb.newest_frame()
        while frame is not None and function not in (frame.name() or ''):
            frame = frame.older()
        if frame is None:
            raise RuntimeError('Missing product frame: ' + function)
        # Use the actual caller return PC. FinishBreakpoint may discard an
        # optimized tail-call frame before the caller resumes.
        caller = frame.older()
        if caller is None:
            raise RuntimeError('Missing caller for: ' + function)
        pc = caller.pc()
        super().__init__('*' + hex(pc), temporary=True, internal=True)
        self.thread = gdb.selected_thread().global_num
        self.name = name
        self.action = action
        emit('PROMOTION_RETURN_BOUNDARY', function=function, pc=pc)

    def stop(self):
        event(self.name)
        return self.action() if self.action else False


def capture_field():
    state['field'] = int(gdb.parse_and_eval('fields'))
    state['object'] = int(gdb.parse_and_eval('address'))
    state['field_offset'] = state['field'] - state['object']
    emit('PROMOTION_INPUT', field=state['field'], object=state['object'],
         field_offset=state['field_offset'],
         raw=int(gdb.parse_and_eval('*(unsigned long*)fields')),
         copied_self=int(gdb.parse_and_eval('*(unsigned long*)(fields + 1)')))
    return False


def flip():
    if state['field'] is not None:
        product_identity()
        Completed('flip_completed', 'flip_promote')
    return False


def handshake():
    name = gdb.parse_and_eval('op_->cl_->name_').string()
    emit('HANDSHAKE_OBSERVED', name=name, field=state['field'])
    if state['field'] is not None and name == 'ZRendezvous':
        product_identity()
        Completed('rendezvous_completed', 'VM_HandshakeAllThreads::doit')
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
    flips = [i for i, name in enumerate(events) if name == 'flip_batch_completed']
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
    return False


def remembered_return():
    word = int(gdb.parse_and_eval('*(unsigned long*)' + str(state['field'])))
    bad = int(gdb.parse_and_eval('g_cjLoadBadMask'))
    good = word != 0 and word & bad == 0
    emit('ASSERT_PROMOTED_NULL_REMAPPED', passed=good, raw=word, load_bad_mask=bad)
    state['passed'] = state['passed'] and good
    state['consumer_observed'] = True
    return True


def remember():
    # This callback is passed by address to the real object iterator. Match
    # the fixture field, rather than the first worker returning from a task.
    if state['field'] is None or 'passed' not in state or state['relocating']:
        return False
    field = int(gdb.parse_and_eval('&field'))
    if field != state['field'] or state.get('remember_started'):
        return False
    product_identity()
    state['remember_started'] = True
    emit('REMEMBER_CONSUMER_INPUT', field=field,
         raw=int(gdb.parse_and_eval('*(unsigned long*)' + str(field))))
    Completed('remember_completed', 'RemapAndMaybeAddRemset', remembered_return)
    return False


def flip_batch():
    if state['field'] is not None:
        product_identity()
        Completed('flip_batch_completed', 'ZRelocate::flip_age_pages')
    return False


def relocated_remembered_return():
    field = state['relocated_field']
    word = int(gdb.parse_and_eval('*(unsigned long*)' + str(field)))
    bad = int(gdb.parse_and_eval('g_cjLoadBadMask'))
    good = word != 0 and word & bad == 0
    emit('ASSERT_RELOCATE_PROMOTED_NULL_REMAPPED', passed=good,
         field=field, raw=word, load_bad_mask=bad)
    state['passed'] = state['passed'] and good
    state['consumer_observed'] = True
    return True


def relocated_remember():
    if state['field'] is None or 'passed' not in state or 'relocated_field' in state:
        return False
    product_identity()
    field = int(gdb.parse_and_eval('&field'))
    # The fixture stores its own source object in field[1]. At the first
    # destination field callback this copied reference has not been consumed
    # yet. Decode its actual remap bits using the product's shift table
    # (zAddress.inline.hpp:43-47), without calling a product function.
    raw = int(gdb.parse_and_eval('*(unsigned long*)' + str(field + 8)))
    shift = int(gdb.parse_and_eval('MapleRuntime::ZPointerRemappedShift'))
    index = (raw >> shift) & 0xf
    if index not in (1, 2, 4, 8):
        return False
    load_shift = int(gdb.parse_and_eval('MapleRuntime::ZPointerLoadShiftTable[' + str(index) + ']'))
    source = raw >> load_shift
    if source != state['object']:
        return False
    destination = field - state['field_offset']
    state['relocated_field'] = field
    emit('RELOCATED_REMEMBER_PRODUCT', source=source, destination=destination,
         field=field, copied_self=raw, load_shift=load_shift,
         stack=gdb.execute('bt', to_string=True))
    Completed('relocated_remember_completed', 'UpdateRemsetPromotedFilterAndRemapPerField',
              relocated_remembered_return)
    return False


try:
    for setting in ['pagination off', 'confirm off', 'breakpoint pending on',
                    'print thread-events off', 'follow-fork-mode child']:
        gdb.execute('set ' + setting)
    fixture = os.environ.get('PROMOTION_FIXTURE', 'FlipPromotion.NullFieldsStayColoredThroughCollection')
    gdb.execute('set environment GC_UNIT_FILTER ' + fixture)
    gdb.execute('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    state['relocating'] = fixture.startswith('RelocatePromotion.')
    root = Path(os.environ['PROMOTION_SOURCE_ROOT'])
    test_lines = (root / 'tests/gc_unit/test_segmented_array_init.cpp').read_text().splitlines()
    capture = next(i + 1 for i, line in enumerate(test_lines) if 'heap.RequestGC(promote ?' in line)
    relocate_lines = (root / 'src/Heap/z/zRelocate.cpp').read_text().splitlines()
    task_start = next(i for i, line in enumerate(relocate_lines) if 'class ZPromoteBarrierTask' in line)
    work = next(i + 1 for i in range(task_start, len(relocate_lines)) if 'SuspendibleThreadSetJoiner stsJoiner' in relocate_lines[i])
    gdb.execute('start')
    Observe('test_segmented_array_init.cpp:' + str(capture), capture_field)
    Observe('MapleRuntime::ZGenerationYoung::flip_promote', flip)
    Observe('MapleRuntime::ZRelocate::flip_age_pages', flip_batch)
    Observe('MapleRuntime::(anonymous namespace)::VM_HandshakeAllThreads::doit()', handshake)
    Observe('zRelocate.cpp:' + str(work), barrier)
    Observe('MapleRuntime::ZGenerationYoung::pause_relocate_start', before_relocate)
    Observe('MapleRuntime::RemapAndMaybeAddRemset', remember)
    if fixture.startswith('RelocatePromotion.'):
        Observe('UpdateRemsetPromotedFilterAndRemapPerField', relocated_remember)
    gdb.execute('continue')
    if state['error'] or 'consumer_observed' not in state:
        raise RuntimeError(state['error'] or 'Product relocate-start boundary not reached')
    gdb.execute('quit ' + ('0' if state['passed'] else '1'))
except Exception as error:
    emit('HARNESS_ERROR', error=repr(error))
    gdb.execute('quit 2')
