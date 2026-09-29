# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Schedule two real allocations through the losing shared-page CAS path.

Use gdb -batch -x this_file --args cj_gc_unit with GCV2_RUNTIME_LIB_DIR and
LD_LIBRARY_PATH selecting the same product SO. Only scheduling and software
breakpoints are changed; no product state writes, calls, or test-only hooks.
ZGC: zObjectAllocator.cpp:66-110 and zHeap.cpp:240-250.
"""
import gdb
import os
from pathlib import Path

exits = []
decreases = []


def product_frame():
    library = gdb.solib_name(gdb.newest_frame().pc())
    expected = Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so')
    if library is None or Path(library).resolve() != expected.resolve():
        raise RuntimeError('unexpected product identity: ' + str(library))
    return library


class FirstCharge(gdb.Breakpoint):
    def stop(self):
        if not (gdb.selected_thread().name or '').startswith('tlab1306-'):
            return False
        print('TLAB1306_FIRST_CHARGE product=' + product_frame(), flush=True)
        return True


class Decrease(gdb.Breakpoint):
    def stop(self):
        library = product_frame()
        names = []
        frame = gdb.newest_frame()
        while frame is not None:
            names.append(frame.name())
            frame = frame.older()
        decreases.append(names)
        print('TLAB1306_DECREASE product=' + library + ' stack=' + repr(names), flush=True)
        return False


try:
    for setting in ('pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off'):
        gdb.execute('set ' + setting, to_string=True)
    test = 'TLABUsage1306.ContendedPageUndoKeepsNetUsage'
    gdb.execute('set environment GC_UNIT_FILTER ' + test)
    gdb.execute('set environment GC_UNIT_OTHER_VM_CHILD ' + test)
    first = FirstCharge('MapleRuntime::ZTLABUsage::increase_used', internal=True)
    Decrease('MapleRuntime::ZTLABUsage::decrease_used', internal=True)
    gdb.events.exited.connect(lambda event: exits.append(getattr(event, 'exit_code', None)))
    gdb.execute('run')
    loser = gdb.selected_thread()
    winner = next(t for t in gdb.selected_inferior().threads()
                  if t != loser and (t.name or '').startswith('tlab1306-'))
    first.enabled = False
    source = Path(__file__).with_name('test_tlab_usage.cpp')
    line = next(i for i, value in enumerate(source.read_text().splitlines(), 1)
                if 'GDB scheduling stop, after product allocation.' in value)
    completed = gdb.Breakpoint('test_tlab_usage.cpp:' + str(line), temporary=True, internal=True)
    completed.thread = winner.global_num
    gdb.execute('set scheduler-locking on')
    winner.switch()
    gdb.execute('continue')
    if gdb.selected_thread() != winner or exits:
        raise RuntimeError('winner did not finish the competing allocation')
    print('TLAB1306_WINNER_PUBLISHED loser=' + str(loser.global_num) +
          ' winner=' + str(winner.global_num), flush=True)
    gdb.execute('set scheduler-locking off')
    gdb.execute('continue')
    print('TLAB1306_UNDO_OBSERVATION decreases=' + str(len(decreases)) + ' exits=' + repr(exits), flush=True)
    if len(exits) != 1 or exits[0] is None:
        raise RuntimeError('no normal test exit')
    gdb.execute('quit ' + str(exits[0]))
except Exception as error:
    print('TLAB1306_SCHEDULE_ERROR ' + repr(error), flush=True)
    gdb.execute('quit 2')
