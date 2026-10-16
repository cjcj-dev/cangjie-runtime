# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Read young liveness after real mark-end, before flip_age_pages resets it.

The inferior still completes its original minor/identity test. No product
state, marking result, forwarding entry or test result is changed by GDB.
"""
import gdb
import json
import os
from pathlib import Path

result = {'reached': False, 'qualified': False, 'marked': None, 'inferior_rc': None}
fixture = os.environ.get('DEDUP_YOUNG_FIXTURE', 'StringDedup.YoungTableRootKeepsIdentity')
if fixture not in ('StringDedup.YoungTableRootKeepsIdentity', 'StringDedup.YoungStrongRootControl'):
    raise RuntimeError('unsupported fixture: ' + fixture)
result['fixture'] = fixture

def cmd(s):
    return gdb.execute(s, to_string=True)

def exited(event):
    result['inferior_rc'] = getattr(event, 'exit_code', None)

gdb.events.exited.connect(exited)

try:
    for option in ('pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off'):
        cmd('set ' + option)
    cmd('handle SIGSEGV nostop noprint pass')
    cmd('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    cmd('set args --gtest_filter=' + fixture)
    bp = gdb.Breakpoint('MapleRuntime::ZGenerationYoung::concurrent_mark_free()', internal=True)
    cmd('run')
    # This breakpoint is before page selection, freeing, remapping and age flip.
    frame = gdb.newest_frame()
    if bp.hit_count != 1:
        raise RuntimeError('did not stop at young mark-free entry')
    result['reached'] = True
    result['stop_function'] = frame.name()
    result['stop_line'] = frame.find_sal().line
    result['stop_pc'] = hex(frame.pc())
    cycle = None
    for thread in gdb.selected_inferior().threads():
        thread.switch()
        frame = gdb.newest_frame()
        while frame:
            if 'CheckDedupCycle' in (frame.name() or ''):
                frame.select()
                cycle = frame.read_var('cycle')
                break
            frame = frame.older()
        if cycle is not None:
            break
    if cycle is None:
        raise RuntimeError('test input carrier not found')
    page = int(cycle['originalPage'])
    first = int(cycle['original'])
    second = int(cycle['candidateBacking'])
    result.update(page=hex(page), first=hex(first), second=hex(second), strong=bool(cycle['strongControl']))
    # These existing, side-effect-free page readers are emitted in the test
    # ELF at O0; they read the product page/livemap. They perform no marking.
    cmd('set scheduler-locking on')
    page_expr = '((MapleRuntime::ZPage*)%d)' % page
    result['allocating'] = bool(gdb.parse_and_eval(page_expr + '->IsAllocating()'))
    result['marked'] = bool(gdb.parse_and_eval(page_expr +
        '->is_object_marked((MapleRuntime::zaddress)%d, false)' % first))
    result['control_marked'] = bool(gdb.parse_and_eval(page_expr +
        '->is_object_marked((MapleRuntime::zaddress)%d, false)' % second))
    # Both eight-byte backing arrays were allocated into this single small
    # page, before GC starts. At this boundary neither can have been moved.
    result['same_granule'] = (first >> 21) == (second >> 21)
    result['qualified'] = bool(cycle['installed']) and not result['allocating'] and \
        result['same_granule'] and result['control_marked']
    print('DEDUP_YOUNG_MARK_ASSERT reached=1 marked=%d qualified=%d strong=%d' %
          (result['marked'], result['qualified'], result['strong']), flush=True)
    bp.delete()
    cmd('set scheduler-locking off')
    cmd('continue')
except Exception as exc:
    result['error'] = str(exc)
finally:
    Path(os.environ['DEDUP_YOUNG_RESULT']).write_text(json.dumps(result, indent=2) + '\n')

ok = result['qualified'] and result['marked'] and result['inferior_rc'] == 0
print('DEDUP_YOUNG_MARK_RESULT ' + json.dumps(result, sort_keys=True), flush=True)
cmd('quit ' + ('0' if ok else '1'))
