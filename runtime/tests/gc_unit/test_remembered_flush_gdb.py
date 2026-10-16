# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Read worker old stacks before work exit and before termination flush.

No product state is injected. The fixture creates old->young->old fields;
product scan/follow produces the stack entries being asserted here.
"""
import gdb
import json

owners = {}
results = []


def population(address):
    data = gdb.Value(address).cast(gdb.lookup_type('MapleRuntime::ThreadGCData').pointer())
    vector = data['markStacks'][1]['stacks']['_M_impl']
    begin, end = vector['_M_start'], vector['_M_finish']
    return sum(int((begin + i).dereference()['top'])
               for i in range(int(end - begin)) if int((begin + i).dereference()))


class BeforeWorkerFlush(gdb.FinishBreakpoint):
    def __init__(self):
        super().__init__(gdb.newest_frame(), internal=True)

    def stop(self):
        tls = gdb.parse_and_eval('(MapleRuntime::ThreadLocalData*)MapleRuntime::threadLocalData')
        address = int(tls['gcData'])
        count = population(address)
        owners[address] = count
        print('REMEMBERED1314_PRECONDITION ' + json.dumps({'owner': address, 'old_local': count}), flush=True)
        return False


class WorkInner(gdb.Breakpoint):
    def stop(self):
        BeforeWorkerFlush()
        return False


class BeforeTermination(gdb.Breakpoint):
    def stop(self):
        frame = gdb.newest_frame()
        in_scan = False
        while frame:
            in_scan |= 'ZRemembered::scan_and_follow' in (frame.name() or '')
            frame = frame.older()
        if not in_scan:
            return False
        before = sum(owners.values())
        after = sum(population(address) for address in owners)
        # Preconditions are printed separately and never prevent the target
        # assertion from being evaluated.
        passed = after == 0
        results.append((before > 0, passed))
        print('REMEMBERED1314_BEFORE_TERMINATION_TARGET ' +
              json.dumps({'precondition': before > 0, 'old_local_before': before,
                          'old_local_after': after, 'passed': passed}), flush=True)
        return False


for command in ['set pagination off', 'set confirm off', 'set breakpoint pending on',
                'set print thread-events off', 'handle SIGUSR1 nostop noprint pass',
                'handle SIGUSR2 nostop noprint pass', 'handle SIGSEGV nostop noprint pass',
                'set environment GC_UNIT_FILTER Remembered1314.MajorRootsPublishesOtherGeneration']:
    gdb.execute(command)
WorkInner('MapleRuntime::ZRememberedScanMarkFollowTask::work_inner()')
BeforeTermination('MapleRuntime::ZMark::TryTerminateFlush()')
gdb.execute('run')
rc = int(gdb.parse_and_eval('$_exitcode'))
passed = bool(results) and all(precondition and target for precondition, target in results) and rc == 0
print('REMEMBERED1314_OBSERVER_RESULT ' + json.dumps({'passed': passed, 'samples': len(results), 'rc': rc}), flush=True)
gdb.execute('quit %d' % (0 if passed else 1))
