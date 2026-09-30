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


try:
    for setting in ('pagination off', 'confirm off', 'print thread-events off', 'disassembly-flavor att'):
        gdb.execute('set ' + setting)
    gdb.execute('start')
    functions = ('CJ_ScheduleGlobalWrite', 'CJ_ProcessorGlobalRead', 'CJ_ScheduleGlobalQueueCount')
    for function in functions:
        listing = gdb.execute('disassemble ' + function, to_string=True)
        count = 0
        for line in listing.splitlines():
            match = re.search(r'(0x[0-9a-f]+)\s+<[^>]+>:\s+(.+)', line)
            if not match or match[2].startswith('lea'):
                continue
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
        failure=failure, covered=covered, program=exit_code), sort_keys=True))
    if failure:
        gdb.execute('quit 1')
    elif not covered or gdb.selected_inferior().pid:
        gdb.execute('quit 2')
    else:
        gdb.execute('quit 0')
except Exception as error:
    print('HARNESS_ERROR ' + str(error))
    gdb.execute('quit 2')
