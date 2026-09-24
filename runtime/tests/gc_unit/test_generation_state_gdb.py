# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Read generation state at real old relocation, then deliver a real signal.

The fixture allocates through product allocation and requests complete GCs.
No product state or return value is written by this observer.
"""
import gdb
import json
import os

samples = []
mode = os.environ.get('GENERATION_OBSERVE', 'forwarding')
fixture = ('DriverCause.MajorTimer' if mode in ('phase', 'seqnum')
           else 'GenerationState.ProductSequenceAndForwarding')
old_before = None


def emit(tag, **fields):
    print(tag + ' ' + json.dumps(fields, sort_keys=True), flush=True)


class PublicationResult(gdb.FinishBreakpoint):
    def __init__(self, address):
        self.address = address
        super().__init__(gdb.newest_frame(), internal=True)

    def stop(self):
        forwarding = gdb.parse_and_eval('(MapleRuntime::ZForwarding*)%d' % self.address)
        published = int(forwarding['_relocated_remembered_fields_publish_young_seqnum'])
        young = int(gdb.parse_and_eval('MapleRuntime::ZGeneration::_young->_seqnum'))
        phase = int(gdb.parse_and_eval('MapleRuntime::ZGeneration::_old->_phase'))
        passed = published == young and phase == 2
        samples.append(passed)
        emit('GENERATION_FORWARDING_TARGET', passed=passed, published=published, young=young, old_phase=phase)
        return mode == 'signal'


class Publication(gdb.Breakpoint):
    def stop(self):
        PublicationResult(int(gdb.parse_and_eval('$rdi')))
        return False


class MarkStart(gdb.Breakpoint):
    def stop(self):
        global old_before
        old_before = int(gdb.parse_and_eval('MapleRuntime::ZGeneration::_old->_seqnum'))
        return False


class MarkConsumer(gdb.Breakpoint):
    def stop(self):
        return True


for command in ['set pagination off', 'set confirm off', 'set breakpoint pending on',
                'set print thread-events off', 'handle SIGUSR1 nostop noprint pass',
                'handle SIGUSR2 nostop noprint pass', 'handle SIGSEGV nostop noprint pass',
                'handle SIGABRT nostop noprint pass',
                'set environment GC_UNIT_FILTER ' + fixture,
                'set environment GC_UNIT_OTHER_VM_CHILD ' + fixture]:
    gdb.execute(command)
if mode in ('phase', 'seqnum'):
    MarkStart('MapleRuntime::ZGenerationOld::mark_start()')
    MarkConsumer('MapleRuntime::ZGenerationOld::concurrent_mark()')
else:
    Publication('MapleRuntime::ZForwarding::relocated_remembered_fields_after_relocate()')
gdb.execute('run')
if mode in ('phase', 'seqnum'):
    phase = int(gdb.parse_and_eval('MapleRuntime::ZGeneration::_old->_phase'))
    sequence = int(gdb.parse_and_eval('MapleRuntime::ZGeneration::_old->_seqnum'))
    if mode == 'phase':
        passed = phase == 0
        emit('GENERATION_PHASE_TARGET', passed=passed, phase=phase)
    else:
        passed = old_before is not None and sequence == old_before + 1
        emit('GENERATION_SEQNUM_TARGET', passed=passed, before=old_before, after=sequence)
    samples.append(passed)
elif mode == 'signal' and samples:
    emit('GENERATION_SIGNAL_DELIVERY', old_phase=int(gdb.parse_and_eval('MapleRuntime::ZGeneration::_old->_phase')))
    gdb.execute('signal SIGABRT')
else:
    inferior_rc = int(gdb.parse_and_eval('$_exitcode'))
    emit('GENERATION_INFERIOR', rc=inferior_rc)
    if inferior_rc != 0:
        samples.append(False)
passed = bool(samples) and all(samples)
emit('GENERATION_OBSERVER_RESULT', passed=passed, samples=len(samples), mode=mode)
gdb.execute('quit %d' % (0 if passed else 1))
