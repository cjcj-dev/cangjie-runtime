# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Schedule the product's final stack publication against inventory flushing.

Uses the existing managed-exit fixture and the real product SO. No product
state, return value, entry point or test assertion is replaced. The trylock
is a nonblocking observation of the same mutex used by GC inventory readers.
"""
import gdb
import json
import os
from pathlib import Path


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
        return 'MapleRuntime::Mutator::ResetMutator' in cmd('bt')


try:
    for option in ('pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off'):
        cmd('set ' + option)
    fixture = 'ThreadLifecycle.ManagedDetachMarksBothGenerations'
    cmd('set environment GC_UNIT_FILTER ' + fixture)
    cmd('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    boundary = ExitPublication('MapleRuntime::MarkStripeStackList::Push')
    cmd('run')
    if not gdb.selected_inferior().threads():
        raise RuntimeError('Product exit publication was not reached')
    exiting = gdb.selected_thread()
    cmd('set scheduler-locking on')
    product = gdb.solib_name(gdb.newest_frame().pc())
    expected = Path(os.environ['GCV2_RUNTIME_LIB_DIR']) / 'libcangjie-runtime.so'
    if Path(product).resolve() != expected.resolve():
        raise RuntimeError('Product identity mismatch: ' + str(product))
    cmd('set $published = this')
    cmd('set $chunk = stack')
    emit('PRODUCT_EXIT_PUBLICATION', library=product, chunk=int(val('$chunk')), stack=cmd('bt'))
    frame = gdb.newest_frame()
    while frame and 'Mutator::ResetMutator' not in frame.name():
        frame = frame.older()
    if frame is None:
        raise RuntimeError('No real ResetMutator caller')
    frame.select()
    cmd('set $owner = this')
    cmd('set $mutex = (pthread_mutex_t*)&$owner->mutatorLock')
    boundary.enabled = False
    observer = next(t for t in gdb.selected_inferior().threads() if t.num == 1)
    observer.switch()
    lock_rc = int(val('(int)pthread_mutex_trylock($mutex)'))
    emit('INVENTORY_LOCK_OBSERVATION', trylock_rc=lock_rc)
    if lock_rc == 0:
        cmd('call (int)pthread_mutex_unlock($mutex)')
        # Run the actual inventory consumer on a second thread while the
        # exiting thread still holds the stack argument for its publication.
        cmd('call MapleRuntime::ZMark::FlushAllGenerations()')
        emit('CONCURRENT_INVENTORY_FLUSH', completed=True)
    elif lock_rc != 16:  # Linux EBUSY
        raise RuntimeError('Unexpected mutex result: ' + str(lock_rc))
    exiting.switch()
    gdb.newest_frame().select()
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
    passed = lock_rc == 16 and duplicates == 1 and count == 1
    emit('ASSERT_EXIT_SINGLE_PUBLICATION', passed=passed, lock_rc=lock_rc,
         stack_references=duplicates, published_nodes=count)
    if not passed:
        cmd('quit 1')
    cmd('set scheduler-locking off')
    cmd('continue')
    code = int(val('$_exitcode'))
    emit('FIXTURE_EXIT', rc=code)
    cmd('quit ' + str(code))
except Exception as error:
    emit('HARNESS_ERROR', error=str(error))
    cmd('quit 2')
