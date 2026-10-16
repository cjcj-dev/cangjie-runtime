# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""A notification before the product wait must not suppress its timed wait.

Use the real runtime fixture and linked SO; no product fields or results are
changed. The notification is delivered through the product notification API.
ZGC zDirector.cpp:843-860 intentionally has no notification latch.
"""
import gdb
import json
import os
from pathlib import Path


def command(value):
    return gdb.execute(value, to_string=True)


result = {'wait_result': None}


class WaitResult(gdb.Breakpoint):
    def stop(self):
        result['wait_result'] = int(gdb.parse_and_eval('$eax'))
        return False


try:
    for option in ['pagination off', 'confirm off', 'breakpoint pending on',
                   'print thread-events off']:
        command('set ' + option)
    fixture = 'GcDirector.ProductWarmupStopsAfterThreeCycles'
    command('set environment GC_UNIT_FILTER ' + fixture)
    command('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    entry = gdb.Breakpoint('MapleRuntime::ZDirector::wait_for_tick()', temporary=True)
    command('run')
    if entry.is_valid():
        raise RuntimeError('product wait entry not reached')
    product = gdb.solib_name(gdb.selected_frame().pc())
    expected = Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so').resolve()
    if Path(product).resolve() != expected:
        raise RuntimeError('unexpected product identity: ' + str(product))
    result['product'] = str(expected)
    command('set scheduler-locking on')
    command('call MapleRuntime::ZDirector::evaluate_rules()')
    address = int(gdb.parse_and_eval('&_ZN12MapleRuntime9ZDirector13wait_for_tickEv'))
    instructions = gdb.selected_frame().architecture().disassemble(address, address + 512)
    for index, instruction in enumerate(instructions[:-1]):
        if 'call' in instruction['asm'] and 'pthread_cond_' in instruction['asm']:
            WaitResult('*' + hex(instructions[index + 1]['addr']), internal=True)
    for _ in range(16):
        if 'wait_for_tick' not in command('bt'):
            break
        command('finish')
    result['passed'] = result['wait_result'] == 110  # ETIMEDOUT: notification was not retained
    print('DIRECTOR_TICK_ASSERT ' + json.dumps(result, sort_keys=True), flush=True)
    Path(os.environ['DIRECTOR_TICK_RESULT']).write_text(json.dumps(result) + '\n')
    command('quit ' + ('0' if result['passed'] else '1'))
except Exception as error:
    print('DIRECTOR_TICK_ERROR ' + repr(error), flush=True)
    command('quit 2')
