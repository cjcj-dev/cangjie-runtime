# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe product installation state in real driver/worker threads."""
import gdb
import json
import os

def cmd(s):
    return gdb.execute(s, to_string=True)

def emit(tag, **data):
    print(tag + ' ' + json.dumps(data, sort_keys=True))

class Installed(gdb.FinishBreakpoint):
    def __init__(self, pointer, before, stack):
        super().__init__(gdb.newest_frame(), internal=True)
        self.pointer, self.before, self.stack = pointer, before, stack
    def stop(self):
        after = int(gdb.parse_and_eval('((MapleRuntime::StoreBarrierBuffer*)%d)->lastInstalledColor' % self.pointer))
        expected = int(gdb.parse_and_eval('g_cjStoreGoodMask'))
        emit('INSTALL_RESULT', before=self.before, after=after, expected=expected, stack=self.stack)
        return False

class Install(gdb.Breakpoint):
    def stop(self):
        pointer = int(gdb.parse_and_eval('this'))
        before = int(gdb.parse_and_eval('this->lastInstalledColor'))
        Installed(pointer, before, cmd('bt 8'))
        return False

try:
    for s in ['pagination off','confirm off','breakpoint pending on','print thread-events off']:
        cmd('set '+s)
    fixture='GcDirector.ProductWarmupStopsAfterThreeCycles'
    cmd('set environment GC_UNIT_FILTER '+fixture)
    cmd('set environment GC_UNIT_OTHER_VM_CHILD '+fixture)
    cmd('start')
    Install('MapleRuntime::StoreBarrierBuffer::install_base_pointers()')
    cmd('continue')
    cmd('quit 0')
except Exception as e:
    emit('HARNESS_ERROR', error=repr(e))
    cmd('quit 2')
