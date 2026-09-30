"""Observe actual count instructions in the Linux x86-64 product scheduler.

The layout probe compiles declarations only. No scheduler implementation is
linked into either probe. Breakpoints inspect inferior memory without writes.
"""
import gdb
import json
import os
from pathlib import Path
import re

layout = json.loads(Path(os.environ['SCHEDULER_LAYOUT']).read_text())
expected_product = Path(os.environ['SCHEDULER_PRODUCT']).resolve()
observations = {}
failure = None
inferior_exit = None
queue_checks = 0
snapshot_callers = {}
snapshot_returns = 0
read_modes = {}
thread_modes = {}


def record_exit(event):
    global inferior_exit
    inferior_exit = event.exit_code if hasattr(event, 'exit_code') else -1


gdb.events.exited.connect(record_exit)


def read_integer(address, size):
    return int.from_bytes(gdb.selected_inferior().read_memory(address, size).tobytes(), 'little')


class CountAccess(gdb.Breakpoint):
    def __init__(self, address, function, register, displacement):
        super().__init__('*' + hex(address), internal=True)
        self.function = function
        self.register = register
        self.displacement = displacement

    def stop(self):
        global failure
        count_address = int(gdb.parse_and_eval('$' + self.register)) + self.displacement
        schedule = count_address - layout['num']
        owner = read_integer(schedule + layout['mutex'] + layout['owner'], 4)
        tid = gdb.selected_thread().ptid[1]
        value = read_integer(count_address, 8)
        if self.function == 'CJ_ProcessorGlobalRead':
            mode = thread_modes.get(gdb.selected_thread().ptid[1], 'unknown')
            key = str(mode) + ':' + ('empty' if value == 0 else 'nonempty')
            read_modes[key] = read_modes.get(key, 0) + 1
        if self.function == 'CJ_ScheduleGlobalQueueCount':
            caller = gdb.newest_frame().older()
            caller_name = caller.name() if caller else 'unknown'
            snapshot_callers[caller_name] = snapshot_callers.get(caller_name, 0) + 1
            SnapshotReturn(value)
        product = gdb.solib_name(int(gdb.parse_and_eval('$pc')))
        passed = owner == tid and product and Path(product).resolve() == expected_product
        key = self.function + ':' + ('empty' if value == 0 else 'nonempty')
        observations[key] = observations.get(key, 0) + 1
        if observations[key] == 1 or not passed:
            print('SCHEDULER_LOCK_TARGET ' + json.dumps(dict(function=self.function,
                value=value, owner=owner, tid=tid, passed=bool(passed), product=product), sort_keys=True))
        if not passed:
            failure = key
            return True
        return False


class SnapshotReturn(gdb.FinishBreakpoint):
    def __init__(self, value):
        super().__init__(gdb.newest_frame(), internal=True)
        self.value = value

    def stop(self):
        global failure, snapshot_returns
        result = int(gdb.parse_and_eval('$rax'))
        passed = result == self.value
        snapshot_returns += 1
        if snapshot_returns == 1 or not passed:
            print('SCHEDULER_SNAPSHOT_TARGET ' + json.dumps(dict(value=self.value,
                returned=result, passed=passed), sort_keys=True))
        if not passed:
            failure = 'snapshot-return'
            return True
        return False


class ReadEntry(gdb.Breakpoint):
    def stop(self):
        thread_modes[gdb.selected_thread().ptid[1]] = bool(int(gdb.parse_and_eval('$rsi')) & 0xff)
        return False


class QueueUnlock(gdb.Breakpoint):
    def stop(self):
        global failure, queue_checks
        schedule = int(gdb.parse_and_eval('$rdi')) - layout['mutex']
        owner = read_integer(schedule + layout['mutex'] + layout['owner'], 4)
        value = read_integer(schedule + layout['num'], 8)
        head = schedule + layout['runq']
        node = read_integer(head + 8, 8)
        nodes = 0
        while node != head and nodes <= value:
            nodes += 1
            node = read_integer(node + 8, 8)
        passed = node == head and nodes == value and owner == gdb.selected_thread().ptid[1]
        queue_checks += 1
        if queue_checks == 1 or not passed:
            print('SCHEDULER_QUEUE_TARGET ' + json.dumps(dict(value=value, nodes=nodes,
                owner=owner, passed=passed), sort_keys=True))
        if not passed:
            failure = 'queue-count'
            return True
        return False


try:
    for setting in ('pagination off', 'confirm off', 'print thread-events off', 'disassembly-flavor att'):
        gdb.execute('set ' + setting)
    gdb.execute('start')
    functions = ('CJ_ScheduleGlobalWrite', 'CJ_ProcessorGlobalRead',
                 'CJ_ScheduleGlobalQueueCount', 'CJ_ScheduleAnyCJThread')
    ReadEntry('*CJ_ProcessorGlobalRead', internal=True)
    for function in functions:
        listing = gdb.execute('disassemble ' + function, to_string=True)
        count = 0
        for line in listing.splitlines():
            match = re.search(r'(0x[0-9a-f]+)\s+<[^>]+>:\s+(.+)', line)
            if not match or match[2].startswith('lea'):
                continue
            if '<pthread_mutex_unlock@plt>' in match[2]:
                QueueUnlock('*' + match[1], internal=True)
            operand = re.search(r'(0x[0-9a-f]+)\(%([a-z0-9]+)\)', match[2])
            if operand and int(operand[1], 16) == layout['num']:
                CountAccess(int(match[1], 16), function, operand[2], int(operand[1], 16))
                count += 1
        if count == 0:
            raise RuntimeError('No actual num instruction found in ' + function)
        print('SCHEDULER_ACCESS_INSTRUCTIONS function=' + function + ' count=' + str(count))
    gdb.execute('continue')
    covered = all(any(key.startswith(function + ':') for key in observations) for function in functions)
    exit_code = gdb.execute('info program', to_string=True)
    print('SCHEDULER_LOCK_RESULT ' + json.dumps(dict(observations=observations,
        failure=failure, covered=covered, queue_checks=queue_checks,
        snapshot_callers=snapshot_callers, snapshot_returns=snapshot_returns,
        read_modes=read_modes,
        inferior_exit=inferior_exit, program=exit_code), sort_keys=True))
    if failure:
        gdb.execute('quit 1')
    elif not covered or queue_checks == 0 or inferior_exit != 0:
        gdb.execute('quit 2')
    else:
        gdb.execute('quit 0')
except Exception as error:
    print('HARNESS_ERROR ' + str(error))
    gdb.execute('quit 2')
