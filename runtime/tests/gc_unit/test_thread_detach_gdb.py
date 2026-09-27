# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe the product's final stack publication relative to the saferegion.

Uses the existing managed-exit fixture and the real product SO. No product
state, return value, entry point or test assertion is replaced.

ZGC form (threads.cpp:1089-1104): the detach flush runs before the exiting
thread is counted as safepoint-safe, so no pause can overlap the publication.
The load-bearing observation is the owner's saferegion word read from the
live TransitMutatorToExit frame while the exiting thread is stopped inside
the real publication: it must be SAFE_REGION_FALSE. Afterwards the shared
list must reference the exiting stack exactly once.
"""
import gdb
import json
import os
from pathlib import Path

SAFE_REGION_FALSE = 0x03020100


def cmd(command):
    result = gdb.execute(command, to_string=True)
    if 'Python Exception' in result:
        raise RuntimeError(result)
    return result


def val(expression):
    return gdb.parse_and_eval(expression)


def emit(tag, **fields):
    print(tag + ' ' + json.dumps(fields, sort_keys=True), flush=True)


class ExitPublication(gdb.Breakpoint):
    def stop(self):
        return gdb.selected_thread().num == EXIT_THREAD


def owner_saferegion_word():
    frame = gdb.newest_frame()
    while frame is not None:
        if frame.name() == 'MapleRuntime::MutatorManager::TransitMutatorToExit':
            frame.select()
            owner = frame.read_var('mutator')
            return int(owner['inSaferegion']['_M_i'])
        frame = frame.older()
    raise RuntimeError('TransitMutatorToExit frame not found')


try:
    for option in ('pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off'):
        cmd('set ' + option)
    fixture = 'ThreadLifecycle.ManagedDetachMarksBothGenerations'
    cmd('set environment GC_UNIT_FILTER ' + fixture)
    cmd('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    entry = gdb.Breakpoint('MapleRuntime::MutatorManager::TransitMutatorToExit', temporary=True)
    cmd('run')
    if entry.is_valid():
        raise RuntimeError('Real TransitMutatorToExit entry was not reached')
    EXIT_THREAD = gdb.selected_thread().num
    boundary = ExitPublication('MapleRuntime::MarkStripeStackList::Push')
    cmd('continue')
    if not gdb.selected_inferior().threads():
        raise RuntimeError('Product exit publication was not reached')
    product = gdb.solib_name(gdb.newest_frame().pc())
    expected = Path(os.environ['GCV2_RUNTIME_LIB_DIR']) / 'libcangjie-runtime.so'
    if Path(product).resolve() != expected.resolve():
        raise RuntimeError('Product identity mismatch: ' + str(product))
    cmd('set $published = this')
    cmd('set $chunk = stack')
    raw = owner_saferegion_word()
    in_saferegion = 0 if raw == SAFE_REGION_FALSE else 1
    emit('EXIT_PUBLICATION_STATE', library=product, chunk=int(val('$chunk')),
         in_saferegion=in_saferegion, stack=cmd('bt'))
    cmd('finish')
    count = int(val('$published->length._M_i'))
    # Count actual references to this same stack, not call/hit counts.
    node = val('$published->head._M_b._M_p')
    duplicates = 0
    seen = set()
    while int(node):
        if int(node) in seen:
            raise RuntimeError('Invalid published node chain')
        seen.add(int(node))
        duplicates += int(node.dereference()['stack']) == int(val('$chunk'))
        node = node.dereference()['next']
    passed = in_saferegion == 0 and duplicates == 1 and count == 1
    emit('ASSERT_EXIT_SINGLE_PUBLICATION', passed=passed, in_saferegion=in_saferegion,
         stack_references=duplicates, published_nodes=count)
    if not passed:
        cmd('quit 1')
    cmd('continue')
    code = int(val('$_exitcode'))
    emit('FIXTURE_EXIT', rc=code)
    cmd('quit ' + str(code))
except Exception as error:
    emit('HARNESS_ERROR', error=str(error))
    cmd('quit 2')
