# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
# Attach to an inferior that survived a fatal signal (old dispatch path) after
# the injecting gdb was SIGKILLed. Find the thread blocked re-entering the
# StackWatermark mutex and prove the wait is same-thread reentry: the mutex
# owner equals the waiting thread's own tid, so the wait cannot be an artifact
# of any debugger thread scheduling.
import gdb

for cmd in ['set pagination off', 'set confirm off']:
    gdb.execute(cmd)

found = False
for thread in gdb.selected_inferior().threads():
    thread.switch()
    trace = gdb.execute('bt 30', to_string=True)
    if 'start_processing' not in trace and 'on_safepoint' not in trace:
        continue
    print('WATERMARK_POSTINJECT_STACK', flush=True)
    for line in trace.splitlines():
        print('POSTINJECT| ' + line, flush=True)
    waiter = thread.ptid[1]
    frame = gdb.newest_frame()
    owner = -1
    while frame is not None:
        if 'start_processing_impl' in (frame.name() or ''):
            frame.select()
            wm = gdb.parse_and_eval('this')
            owner = int(wm['lock']['_M_mutex']['__data']['__owner'])
            break
        frame = frame.older()
    print('WATERMARK_SELFLOCK owner=%d waiter=%d same=%d'
          % (owner, waiter, owner == waiter), flush=True)
    found = True
    break

if not found:
    print('GDB_ERROR no watermark-reentry thread found', flush=True)
    gdb.execute('quit 4')
gdb.execute('quit 0')
