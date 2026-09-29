# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe the partial owner stack at the real worker0 proactive flush.

The test's ZBreakpoint controls the phase; GDB only reads product state and
return values. In particular, no flush is called by the observer.
"""
import gdb
import json
import os
from pathlib import Path


def command(text):
    return gdb.execute(text, to_string=True)


def population(address):
    # Read the product-owned stack receipts without executing inferior calls.
    stacks = gdb.Value(address).cast(gdb.lookup_type(
        'MapleRuntime::MarkThreadLocalStacks').pointer()).dereference()['stacks']['_M_impl']
    begin, end = stacks['_M_start'], stacks['_M_finish']
    sizes = []
    for index in range(int(end - begin)):
        stack = (begin + index).dereference()
        if int(stack):
            sizes.append(max(1, int(stack.dereference()['top'])))
    return sum(sizes)


result = {'before': None, 'after': None, 'returned': None, 'entered': False}


class WorkerZero(gdb.Breakpoint):
    def stop(self):
        return int(gdb.parse_and_eval('workerId')) == 0


class Returned(gdb.Breakpoint):
    def stop(self):
        result['returned'] = bool(int(gdb.parse_and_eval('$al')))
        return True


try:
    for option in ['pagination off', 'confirm off', 'breakpoint pending on',
                   'print thread-events off']:
        command('set ' + option)
    fixture = 'ZMarkFlush.ConcurrentWorkerPublishesPartialMutatorStack'
    command('set environment GC_UNIT_FILTER ' + fixture)
    command('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    source = Path(os.environ['MARK_FLUSH_SOURCE']).read_text().splitlines()
    start = next(i for i, line in enumerate(source) if 'GC_RUNTIME_OTHER_VM_TEST(ZMarkFlush,' in line)
    line = next(i + 1 for i in range(start, len(source))
                if 'const bool reached = ConcurrentGCBreakpoints::RunTo' in source[i])
    ready = gdb.Breakpoint('test_value_root_identity.cpp:' + str(line), temporary=True)
    command('run')
    if ready.is_valid():
        raise RuntimeError('test setup boundary not reached')
    controller = gdb.selected_thread()
    stack_address = None
    request_address = None
    owner = None
    for thread in gdb.selected_inferior().threads():
        thread.switch()
        frame = gdb.newest_frame()
        while frame:
            try:
                request_address = int(frame.read_var('requests')['_M_i'].address)
                stack_address = int(frame.read_var('stacks').address)
                owner = thread
                break
            except (gdb.error, ValueError):
                frame = frame.older()
        if stack_address is not None:
            break
    controller.switch()
    if stack_address is None:
        raise RuntimeError('owner private stack not found')
    boundary = gdb.Breakpoint('MapleRuntime::ZGenerationOld::mark_follow()', temporary=True)
    command('continue')
    if boundary.is_valid():
        raise RuntimeError('mark-follow phase entry not reached')
    driver = gdb.selected_thread()
    owner.switch()
    print('OWNER_INPUT_STACK ' + command('bt'), flush=True)
    command('set {unsigned int}' + str(request_address) + ' = 1')
    print('OWNER_INPUT_REQUEST ' + command('p *(unsigned int*)' + str(request_address)), flush=True)
    seeded_line = next(i + 1 for i in range(start, len(source)) if 'before = stacks.Population();' in source[i])
    seeded = gdb.Breakpoint('test_value_root_identity.cpp:' + str(seeded_line), temporary=True)
    command('set scheduler-locking on')
    command('continue')
    if seeded.is_valid():
        raise RuntimeError('partial stack input not published by owner')
    result['before'] = population(stack_address)
    driver.switch()
    command('set scheduler-locking off')
    entry = WorkerZero('MapleRuntime::ZMark::TryProactiveFlush(unsigned long)')
    command('continue')
    if gdb.selected_inferior().pid:
        result['entered'] = True
        entry.delete()
        result['before'] = population(stack_address)
        product = gdb.solib_name(gdb.selected_frame().pc())
        expected = Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so').resolve()
        if Path(product).resolve() != expected:
            raise RuntimeError('unexpected product identity')
        result['product'] = str(expected)
        handshake = gdb.Breakpoint('MapleRuntime::ZMark::HandshakeFlush(MapleRuntime::ZMark*)', temporary=True)
        command('continue')
        if handshake.is_valid():
            raise RuntimeError('proactive handshake not reached')
        frame = gdb.newest_frame()
        while frame.type() == gdb.INLINE_FRAME:
            frame = frame.older()
        print('MARK_FLUSH_STACK ' + command('bt'), flush=True)
        returned = Returned('*' + hex(frame.older().pc()), temporary=True, internal=True)
        command('continue')
        result['after'] = population(stack_address)
    result['passed'] = (result['entered'] and result['before'] == 1 and
                        result['after'] == 0 and result['returned'] is not None)
    print('MARK_PROACTIVE_ASSERT ' + json.dumps(result, sort_keys=True), flush=True)
    Path(os.environ['MARK_FLUSH_RESULT']).write_text(json.dumps(result) + '\n')
    command('quit ' + ('0' if result['passed'] else '1'))
except Exception as error:
    print('MARK_PROACTIVE_ERROR ' + repr(error), flush=True)
    command('quit 2')
