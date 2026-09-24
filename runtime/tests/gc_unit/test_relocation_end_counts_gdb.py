# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""kkk2 x86-64 SysV observer: read product allocation returns and published counts; never write inferior state.

Counts are derived independently from Heap::alloc_page return values, not from
allocator increments. GDB serializes breakpoint callbacks, but workers remain
concurrent. Each invocation and task has its own observation record.
"""
import gdb
import json
import os
import subprocess
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
young_started = False
publication_waiter = None
publications = 0


def check(tag, passed, **values):
    checks.append(bool(passed))
    emit(tag, passed=bool(passed), **values)


class ReturnBreakpoint(gdb.Breakpoint):
    def __init__(self, internal=True):
        # x86-64 SysV: at the exact entry rsp points to the actual return PC.
        # This also works for optimized tail calls where DWARF finish tracking
        # can expire without reporting a return.
        pc = int(gdb.parse_and_eval('*(unsigned long*)$rsp'))
        super().__init__('*' + str(pc), internal=internal, temporary=True)
        self.thread = gdb.selected_thread().global_num

    @property
    def return_value(self):
        return gdb.parse_and_eval('$rax')


class AllocationReturn(ReturnBreakpoint):
    def __init__(self, event):
        super().__init__(internal=True)
        self.event = event

    def stop(self):
        gdb.newest_frame().select()
        self.enabled = False
        failed = int(self.return_value) == 0
        self.event['attempts'] += 1
        self.event['failures'] += int(failed)
        failures[self.event['kind']] += int(failed)
        emit('PRODUCT_ALLOCATION_RETURN', failed=failed, kind=self.event['kind'],
             result=int(self.return_value), failures=failures[:])
        return False


def address(name):
    # Resolve against the selected product ELF, never the test ELF's PLT.
    return '*' + str(product_base + symbols[name])


class Allocation(gdb.Breakpoint):
    def stop(self):
        gdb.newest_frame().select()
        event = invocations.get(gdb.selected_thread().global_num)
        if event is None and 'ZRelocateSmallAllocator' in command('bt 10'):
            event = dict(kind=0, attempts=0, failures=0)
        if event is not None:
            AllocationReturn(event)
        return False


class AllocatorReturn(ReturnBreakpoint):
    def __init__(self, event, allocator):
        super().__init__(internal=True)
        self.event = event
        self.allocator = allocator
        self.thread_id = gdb.selected_thread().global_num

    def stop(self):
        gdb.newest_frame().select()
        self.enabled = False
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
        self.type_name = 'MapleRuntime::ZRelocate' + name + 'Allocator'
        super().__init__(address(self.type_name + '::alloc_and_retire_target_page(MapleRuntime::ZForwarding*, MapleRuntime::ZPage*)'), internal=True)

    def stop(self):
        gdb.newest_frame().select()
        allocator = gdb.parse_and_eval('$rdi').cast(gdb.lookup_type(self.type_name).pointer())
        event = dict(kind=self.kind, attempts=0, failures=0,
                     before=count(allocator.dereference()))
        invocations[gdb.selected_thread().global_num] = event
        AllocatorReturn(event, allocator)
        return False


class TaskStart(gdb.Breakpoint):
    def stop(self):
        gdb.newest_frame().select()
        task = gdb.parse_and_eval('task')
        failures[:] = [0, 0]
        tasks.append(int(task.address))
        small = count(task['smallAllocator'])
        medium = count(task['mediumAllocator'])
        check('ASSERT_TASK_INITIAL_COUNTS', small == 0 and medium == 0,
              task=len(tasks), small=small, medium=medium)
        return False


class Published(ReturnBreakpoint):
    def stop(self):
        gdb.newest_frame().select()
        self.enabled = False
        global publications
        publications += 1
        actual = Path(gdb.solib_name(gdb.newest_frame().pc())).resolve()
        expected = Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so').resolve()
        stack = command('bt 12')
        if actual != expected or 'ZRelocate::relocate' not in stack or 'ForwardTask' not in stack:
            raise RuntimeError('Product identity/entry mismatch: ' + str(actual) + stack)
        generation = 'MapleRuntime::ZGeneration::' + ('_old' if case.startswith('Old') else '_young')
        small = int(gdb.parse_and_eval(generation + '->statRelocation._smallInPlaceCount'))
        medium = int(gdb.parse_and_eval(generation + '->statRelocation._mediumInPlaceCount'))
        relocate = '(*(MapleRuntime::ZRelocate**)(&' + generation + '->_relocate))'
        active = bool(gdb.parse_and_eval(relocate + '->relocateQueue.isActive._M_base._M_i'))
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


class YoungStarted(ReturnBreakpoint):
    def stop(self):
        gdb.newest_frame().select()
        self.enabled = False
        global young_started
        young_started = True
        check('ASSERT_YOUNG_START_BETWEEN_OLD_COUNT_AND_PUBLICATION',
              sum(failures) > 0 and publications == 0,
              failures=failures[:], publications=publications)
        return publication_waiter is not None


class Starting(gdb.Breakpoint):
    def stop(self):
        gdb.newest_frame().select()
        if case.endswith('YoungStart') and int(gdb.parse_and_eval('$rdi')) == 0:
            YoungStarted(internal=True)
        return False


class Publishing(gdb.Breakpoint):
    def stop(self):
        gdb.newest_frame().select()
        global publication_waiter
        generation = 'MapleRuntime::ZGeneration::' + ('_old' if case.startswith('Old') else '_young')
        expected = int(gdb.parse_and_eval('&' + generation + '->statRelocation'))
        if int(gdb.parse_and_eval('$rdi')) != expected:
            return False
        Published(internal=True)
        if case.endswith('YoungStart') and not young_started:
            publication_waiter = gdb.selected_thread()
            return True
        return False


try:
    case = os.environ.get('RELOCATION_COUNTS_CASE', 'OldSmall')
    fixture = 'RelocationEndCounts.' + case
    for setting in ['pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off']:
        command('set ' + setting)
    command('set environment GC_UNIT_FILTER ' + fixture)
    command('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    command('start')
    library = str(Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so').resolve())
    symbols = {}
    for line in subprocess.check_output(['nm', '--defined-only', '-C', library], text=True).splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    load_vaddr = next(int(line.split()[2], 16) for line in subprocess.check_output(
        ['readelf', '-lW', library], text=True).splitlines() if line.strip().startswith('LOAD '))
    for line in command('info proc mappings').splitlines():
        parts = line.split()
        if parts and parts[-1] == library and int(parts[3], 16) == 0:
            product_base = int(parts[0], 16) - load_vaddr
            break
    else:
        raise RuntimeError('Product load mapping missing')
    emit('PRODUCT_IDENTITY', library=library, base=product_base)
    source = Path(__file__).resolve().parents[2] / 'src/Heap/z/zRelocate.cpp'
    generation_name = 'Old' if case.startswith('Old') else 'Young'
    lines = source.read_text().splitlines()
    task_line = next(i + 2 for i, line in enumerate(lines)
                     if 'ForwardTask<Generation::' + generation_name + '> task(' in line)
    TaskStart('zRelocate.cpp:' + str(task_line), internal=True)
    Allocator(0)
    Allocator(1)
    Allocation(address('MapleRuntime::Heap::alloc_page(unsigned long, MapleRuntime::ZPageType, bool, MapleRuntime::PageAge, MapleRuntime::ZAllocationFlags)'), internal=True)
    Starting(address('MapleRuntime::ZRelocate::StartRelocationTasks(MapleRuntime::ZGenerationId)'), internal=True)
    Publishing(address('MapleRuntime::ZStatRelocation::AtRelocateEnd(unsigned long, unsigned long)'), internal=True)
    gdb.execute('continue')
    if publication_waiter is not None:
        # Only scheduling is controlled: the fixture thread executes the real
        # young start, while the old thread remains before statistic storage.
        helper = next(t for t in gdb.selected_inferior().threads() if t.name == 'count-young')
        helper.switch()
        command('set scheduler-locking on')
        gdb.execute('continue')
        publication_waiter.switch()
        command('set scheduler-locking off')
        gdb.execute('continue')
    rc = int(gdb.parse_and_eval('$_exitcode'))
    check('ASSERT_TASKS_OBSERVED', len(tasks) == 2, observed=len(tasks))
    check('ASSERT_PUBLICATIONS_OBSERVED', publications == 2, observed=publications)
    if case.endswith('YoungStart'):
        check('ASSERT_YOUNG_START_OBSERVED', young_started)
    if case in ('OldMedium', 'OldMediumYoungStart'):
        check('ASSERT_REUSE_BRANCH_OBSERVED', reuses > 0, observed=reuses)
    emit('FIXTURE_COMPLETE', exit_code=rc, tasks=len(tasks), reuses=reuses)
    command('quit ' + ('0' if checks and all(checks) and rc == 0 else '1'))
except Exception as error:
    emit('HARNESS_ERROR', error=repr(error))
    command('quit 2')
