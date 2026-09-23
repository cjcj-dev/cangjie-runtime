# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe product installation state in real driver/worker threads."""
import gdb
import json
import os

observed = {'worker': 0, 'pause': 0, 'idempotent': 0, 'lazy': 0, 'violations': 0}
inner_calls = {}
worker_colors = {}
exit_codes = []
# The lazy positive-control SO omits worker dispatch, leaving threads uncovered.
lazy_control = os.environ.get('STORE_BASES_LAZY_CONTROL') == '1'

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
        stack = cmd('bt 12')
        if 'ZRelocateStoreBufferInstallBasePointersTask::work' in stack:
            observed['worker'] += 1
            worker_colors[pointer] = int(gdb.parse_and_eval('g_cjStoreGoodMask'))
        if 'VM_ZRelocateStartOld::do_operation' in stack or 'VM_ZRelocateStartYoung::do_operation' in stack:
            observed['pause'] += 1
        Installed(pointer, before, stack)
        return False

class InnerInstall(gdb.Breakpoint):
    def stop(self):
        pointer = int(gdb.parse_and_eval('$rdi'))
        inner_calls[pointer] = inner_calls.get(pointer, 0) + 1
        return False

class PhaseResult(gdb.FinishBreakpoint):
    def __init__(self, pointer, before, kind):
        super().__init__(gdb.newest_frame(), internal=True)
        self.pointer, self.before, self.kind = pointer, before, kind
        self.calls = inner_calls.get(pointer, 0)
    def stop(self):
        after = int(gdb.parse_and_eval('((MapleRuntime::StoreBarrierBuffer*)%d)->lastInstalledColor' % self.pointer))
        delta = inner_calls.get(self.pointer, 0) - self.calls
        expected = 0 if self.kind == 'idempotent' else 1
        # Mark bits may flip between installation and consumption. Remap bits
        # are the product's phase identity (ZGC zStoreBarrierBuffer.cpp:112).
        mask = int(gdb.parse_and_eval('MapleRuntime::ZPointerRemappedMask'))
        passed = delta == expected and (self.kind != 'idempotent' or (after & mask) == (self.before & mask))
        observed[self.kind] += 1
        observed['violations'] += not passed
        emit('ASSERT_PHASE_INSTALL', pointer=self.pointer, kind=self.kind,
             before=self.before, after=after, installs=delta, passed=passed)
        return False

class NewPhase(gdb.Breakpoint):
    def stop(self):
        pointer = int(gdb.parse_and_eval('$rdi'))
        before = int(gdb.parse_and_eval('((MapleRuntime::StoreBarrierBuffer*)%d)->lastInstalledColor' % pointer))
        mask = int(gdb.parse_and_eval('MapleRuntime::ZPointerRemappedMask'))
        remapped = int(gdb.parse_and_eval('MapleRuntime::ZPointerRemapped'))
        if (before & mask) != remapped:
            PhaseResult(pointer, before, 'lazy')
        elif (worker_colors.get(pointer, 0) & mask) == remapped:
            PhaseResult(pointer, before, 'idempotent')
        return False

try:
    for s in ['pagination off','confirm off','breakpoint pending on','print thread-events off']:
        cmd('set '+s)
    if 'i386:x86-64' not in gdb.selected_inferior().architecture().name():
        raise RuntimeError('This debugger fixture requires the x86-64 SysV this register')
    gdb.events.exited.connect(lambda event: exit_codes.append(getattr(event, 'exit_code', None)))
    fixture='GcDirector.ProductWarmupStopsAfterThreeCycles'
    cmd('set environment GC_UNIT_FILTER '+fixture)
    cmd('set environment GC_UNIT_OTHER_VM_CHILD '+fixture)
    cmd('start')
    Install('MapleRuntime::StoreBarrierBuffer::install_base_pointers()')
    InnerInstall('*MapleRuntime::StoreBarrierBuffer::install_base_pointers_inner')
    NewPhase('*MapleRuntime::StoreBarrierBuffer::on_new_phase')
    gdb.execute('continue')
    coverage = (observed['worker'] == 0 and observed['lazy'] > 0) if lazy_control else (
        observed['worker'] > 0 and observed['idempotent'] > 0)
    passed = (coverage and observed['pause'] == 0
              and observed['violations'] == 0 and exit_codes == [0])
    emit('PRODUCT_EXIT', codes=exit_codes, lazy_control=lazy_control)
    emit('ASSERT_CONCURRENT_BASE_INSTALL', passed=passed, **observed)
    cmd('quit ' + ('0' if passed else '1'))
except Exception as e:
    emit('HARNESS_ERROR', error=repr(e))
    cmd('quit 2')
