# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Read product allocation returns and published counts; never write inferior state.

Counts are derived independently from Heap::alloc_page return values, not from
allocator increments. GDB serializes breakpoint callbacks, but workers remain
concurrent. Each invocation and task has its own observation record.
"""
import gdb
import json
import os
from pathlib import Path


def command(text):
    return gdb.execute(text, to_string=True)


def emit(tag, **values):
    print(tag + ' ' + json.dumps(values, sort_keys=True))


def count(allocator):
    return int(allocator['inPlaceCount']['_M_i'])


checks = []
failures = [0, 0]
invocations = {}
tasks = []
reuses = 0


def check(tag, passed, **values):
    checks.append(bool(passed))
    emit(tag, passed=bool(passed), **values)


class AllocationReturn(gdb.FinishBreakpoint):
    def __init__(self, event):
        super().__init__(internal=True)
        self.event = event

    def stop(self):
        failed = int(self.return_value) == 0
        self.event['attempts'] += 1
        self.event['failures'] += int(failed)
        failures[self.event['kind']] += int(failed)
        emit('PRODUCT_ALLOCATION_RETURN', failed=failed, kind=self.event['kind'],
             result=int(self.return_value), failures=failures[:])
        return False


class Allocation(gdb.Breakpoint):
    def stop(self):
        event = invocations.get(gdb.selected_thread().global_num)
        if event is not None:
            AllocationReturn(event)
        return False


class AllocatorReturn(gdb.FinishBreakpoint):
    def __init__(self, event, allocator):
        super().__init__(internal=True)
        self.event = event
        self.allocator = allocator
        self.thread_id = gdb.selected_thread().global_num

    def stop(self):
        global reuses
        event = self.event
        after = count(self.allocator.dereference())
        # Per-call count deltas are only asserted for single-worker fixtures.
        # For concurrent workers, the task total below is the invariant.
        if event['kind'] == 1 and event['attempts'] == 0:
            reuses += 1
            check('ASSERT_SHARED_TARGET_REUSED', int(self.return_value) != 0,
                  after=after, **event)
        emit('PRODUCT_ALLOCATOR_RETURN', after=after, result=int(self.return_value), **event)
        invocations.pop(self.thread_id, None)
        return False


class Allocator(gdb.Breakpoint):
    def __init__(self, kind):
        self.kind = kind
        name = 'Small' if kind == 0 else 'Medium'
        super().__init__('MapleRuntime::ZRelocate' + name + 'Allocator::alloc_and_retire_target_page', internal=True)

    def stop(self):
        allocator = gdb.parse_and_eval('this')
        event = dict(kind=self.kind, attempts=0, failures=0,
                     before=count(allocator.dereference()))
        invocations[gdb.selected_thread().global_num] = event
        AllocatorReturn(event, allocator)
        return False


class ConstructorReturn(gdb.FinishBreakpoint):
    def __init__(self):
        self.task = gdb.parse_and_eval('this')
        super().__init__(internal=True)

    def stop(self):
        task = self.task.dereference()
        failures[:] = [0, 0]
        tasks.append(int(self.task))
        small = count(task['smallAllocator'])
        medium = count(task['mediumAllocator'])
        check('ASSERT_TASK_INITIAL_COUNTS', small == 0 and medium == 0,
              task=len(tasks), small=small, medium=medium)
        return False


class Constructor(gdb.Breakpoint):
    def stop(self):
        ConstructorReturn()
        return False


class Deactivate(gdb.Breakpoint):
    def stop(self):
        actual = Path(gdb.solib_name(gdb.newest_frame().pc())).resolve()
        expected = Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so').resolve()
        stack = command('bt 12')
        if actual != expected or 'ZRelocate::relocate' not in stack or 'ForwardTask' not in stack:
            raise RuntimeError('Product identity/entry mismatch: ' + str(actual) + stack)
        generation = 'MapleRuntime::ZGeneration::' + ('_old' if case.startswith('Old') else '_young')
        small = int(gdb.parse_and_eval(generation + '->statRelocation._smallInPlaceCount'))
        medium = int(gdb.parse_and_eval(generation + '->statRelocation._mediumInPlaceCount'))
        active = bool(gdb.parse_and_eval('this->isActive._M_base._M_i'))
        expected_counts = failures[:]
        check('ASSERT_COUNTS_PUBLISHED_BEFORE_DEACTIVATE',
              [small, medium] == expected_counts and active,
              task=len(tasks), small=small, medium=medium, expected=expected_counts,
              active=active, library=str(actual), stack=stack)
        if len(tasks) == 1:
            target = expected_counts[0 if 'Small' in case else 1]
            check('ASSERT_ALLOCATION_INPUT', target == 0 if case.endswith('Available') else target > 0,
                  failed_allocations=target)
        return False


try:
    case = os.environ.get('RELOCATION_COUNTS_CASE', 'OldSmall')
    fixture = 'RelocationEndCounts.' + case
    for setting in ['pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off']:
        command('set ' + setting)
    command('set environment GC_UNIT_FILTER ' + fixture)
    command('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    command('start')
    enum = '1' if case.startswith('Old') else '0'
    Constructor('MapleRuntime::ForwardTask<(MapleRuntime::Generation)' + enum + '>::ForwardTask', internal=True)
    Allocator(0)
    Allocator(1)
    Allocation('MapleRuntime::Heap::alloc_page(unsigned long, MapleRuntime::ZPageType, bool, MapleRuntime::PageAge, MapleRuntime::ZAllocationFlags)', internal=True)
    Deactivate('MapleRuntime::ZRelocateQueue::deactivate()', internal=True)
    command('continue')
    rc = int(gdb.parse_and_eval('$_exitcode'))
    check('ASSERT_TASKS_OBSERVED', len(tasks) == 2, observed=len(tasks))
    emit('FIXTURE_COMPLETE', exit_code=rc, tasks=len(tasks), reuses=reuses)
    command('quit ' + ('0' if checks and all(checks) and rc == 0 else '1'))
except Exception as error:
    emit('HARNESS_ERROR', error=repr(error))
    command('quit 2')
