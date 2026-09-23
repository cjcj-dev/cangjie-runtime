# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe request cause and completion at the real product driver.

The abort arm invokes the product stop flag used by ZCollectedHeap::stop,
inside a real asynchronously requested warmup collection. The subprocess is
then terminated, just as VM shutdown makes outstanding sync waiters moot.
No product test callback, replacement entry, or synthetic request is used.
"""
import gdb
import json
import os
from pathlib import Path

abort = os.environ.get('DRIVER_ABORT', '0') == '1'
fixture = os.environ.get('DRIVER_FIXTURE', 'GcDirector.ProductWarmupStopsAfterThreeCycles')
expected_so = Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so').resolve()
acks = 0
failures = []


def val(expr):
    return gdb.parse_and_eval(expr)


def check(name, result, **values):
    print('DRIVER_TARGET ' + json.dumps(dict(name=name, passed=bool(result), **values)), flush=True)
    if not result:
        failures.append(name)


def product():
    so = gdb.solib_name(gdb.newest_frame().pc())
    if not so or Path(so).resolve() != expected_so:
        raise RuntimeError('Not requested product SO: ' + str(so))


class Ack(gdb.Breakpoint):
    def stop(self):
        global acks
        if gdb.selected_thread().name != 'ZDriverMajor':
            return False
        product()
        acks += 1
        if not abort:
            return True
        return False


try:
    for setting in ('pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off'):
        gdb.execute('set ' + setting)
    gdb.execute('set environment GC_UNIT_FILTER ' + fixture)
    gdb.execute('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    gdb.Breakpoint('MapleRuntime::ZDriverMajor::gc', temporary=True, internal=True)
    gdb.execute('run')
    product()
    cause = int(val('request._cause'))
    driver = int(val('this'))
    gdb.execute('set $driver = (MapleRuntime::ZDriverMajor*)' + str(driver))
    Ack('MapleRuntime::ZDriverPort::ack', internal=True)
    gdb.Breakpoint('MapleRuntime::ZGenerationOld::collect', temporary=True, internal=True)
    gdb.execute('continue')
    product()
    during = int(val('$driver->_gc_cause'))
    check('A_CAUSE_DURING', during == cause, actual=during, expected=cause)
    if abort:
        # Same product operation as zCollectedHeap.cpp stop(); the debugger
        # chooses the exact collection point, not a replacement abort model.
        gdb.execute('call (void) MapleRuntime::ZAbort::abort()')
        source = Path(os.environ['DRIVER_SOURCE_ROOT'], 'runtime/src/Heap/z/zThread.cpp')
        line = next(i+1 for i,s in enumerate(source.read_text().splitlines())
                    if 'std::unique_lock<std::mutex> ml(_terminator_lock)' in s)
        gdb.Breakpoint('zThread.cpp:' + str(line), temporary=True, internal=True)
        gdb.execute('continue')
        product()
        busy = bool(val('$driver->_port._has_message'))
        returned = gdb.selected_thread().name == 'ZDriverMajor' and 'run_service' in gdb.newest_frame().name()
        check('B_ABORT_NO_ACK', acks == 0 and busy and returned,
              ack=acks, busy=busy, returned_from_driver_loop=returned)
    else:
        gdb.execute('continue')
        check('B_COMPLETION_ACK_CONTROL', acks == 1, ack=acks)
    after = int(val('$driver->_gc_cause'))
    no_cause = int(val('MapleRuntime::GC_REASON_INVALID'))
    check('A_CAUSE_RESTORED', after == no_cause, actual=after, expected=no_cause)
    print('DRIVER_RESULT ' + json.dumps(dict(abort=abort, failures=failures, product=str(expected_so))), flush=True)
    gdb.execute('quit ' + ('1' if failures else '0'))
except Exception as error:
    print('DRIVER_HARNESS_ERROR ' + repr(error), flush=True)
    gdb.execute('quit 2')
