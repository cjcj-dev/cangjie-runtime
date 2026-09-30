# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe the real relocation queue without an exported test accessor.

The stop point is the fixture's snapshot read, not a product callback. GDB's
all-stop mode freezes all threads, reads the existing ZArray length, and writes
only the observation file consumed by the unchanged queued/done assertions.
No inferior variable, forwarding entry, queue or return value is written.
"""
import gdb
import json
import os
from pathlib import Path

fixture = 'StringDedup.RelocationWaitAllowsWorkerAndStop'
source = Path(os.environ['DEDUP_QUEUE_OBSERVER_SOURCE']).resolve()
output = Path(os.environ['DEDUP_QUEUE_OBSERVER_FILE'])
state = {'inferior_rc': None, 'transitions': [], 'samples': 0, 'error': None}


def exited(event):
    state['inferior_rc'] = getattr(event, 'exit_code', None)


gdb.events.exited.connect(exited)
for option in ('pagination off', 'confirm off', 'breakpoint pending on',
               'print thread-events off', 'non-stop off'):
    gdb.execute('set ' + option, to_string=True)
gdb.execute('handle SIGSEGV nostop noprint pass', to_string=True)
gdb.execute('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture, to_string=True)
gdb.execute('set args --gtest_filter=' + fixture, to_string=True)
line = next(i for i, text in enumerate(source.read_text().splitlines(), 1)
            if '// DEDUP_QUEUE_SNAPSHOT' in text)


class Snapshot(gdb.Breakpoint):
    def stop(self):
        try:
            frame = gdb.newest_frame()
            queue = None
            while frame:
                try:
                    candidate = frame.read_var('queue')
                    if 'ZRelocateQueue' in str(candidate.type):
                        queue = candidate
                        break
                except (gdb.error, ValueError):
                    pass
                frame = frame.older()
            if queue is None:
                raise RuntimeError('real product queue not found in fixture frame')
            count = int(queue['queue']['_len'])
            if count < 0:
                raise RuntimeError('invalid queue length')
            state['samples'] += 1
            if not state['transitions'] or state['transitions'][-1] != count:
                state['transitions'].append(count)
            output.write_text(str(count) + '\n')
            return False
        except Exception as exc:
            state['error'] = str(exc)
            return True


Snapshot(str(source) + ':' + str(line), internal=True)
try:
    gdb.execute('run')
except Exception as exc:
    state['error'] = str(exc)
state['qualified'] = state['error'] is None and 1 in state['transitions'] and \
    bool(state['transitions']) and state['transitions'][-1] == 0
print('DEDUP_QUEUE_OBSERVER ' + json.dumps(state, sort_keys=True))
gdb.execute('quit ' + ('0' if state['qualified'] and state['inferior_rc'] == 0 else '1'))
