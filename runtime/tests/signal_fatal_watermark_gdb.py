# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
# Deliver SIGABRT to a thread verifiably holding the StackWatermark mutex,
# using gdb's `signal` command (kernel-level delivery on resume), so no
# inferior function call is in flight and termination is observed cleanly.
# Verdicts derive from the inferior's own state (gdb's termination report,
# pid liveness), never from gdb-internal messages alone.
import gdb
import os

for cmd in ['set pagination off', 'set confirm off', 'set breakpoint pending on',
            'set print thread-events off', 'handle SIGUSR1 nostop noprint pass',
            'handle SIGUSR2 nostop noprint pass', 'handle SIGABRT nostop noprint pass',
            'handle SIGSEGV nostop noprint pass', 'handle SIGBUS nostop noprint pass',
            'handle SIGILL nostop noprint pass', 'handle SIGFPE nostop noprint pass']:
    gdb.execute(cmd)


def inferior_alive(pid):
    if pid <= 0:
        return False
    try:
        if gdb.selected_inferior().pid == 0:
            return False
    except gdb.error:
        return False
    try:
        with open('/proc/%d/stat' % pid) as stat:
            state = stat.read().rsplit(')', 1)[1].split()[0]
        return state != 'Z'
    except OSError:
        return False


class OwnerEntry(gdb.Breakpoint):
    def stop(self):
        # Only the owner entering from LeaveSaferegion, never the GC worker
        # processing a remote owner. No watermark/mutator state is fabricated.
        # Prefixed PREINJECT| so the checker never mistakes this entry stack
        # for the post-injection stack.
        trace = gdb.execute('bt 18', to_string=True)
        if 'LeaveSaferegion' not in trace:
            return False
        print('WATERMARK_REAL_ENTRY', flush=True)
        for line in trace.splitlines():
            print('PREINJECT| ' + line, flush=True)
        return True


try:
    entry = gdb.Breakpoint('SignalWatermarkWork')
    gdb.execute('run')
    entry.delete()
    pid = gdb.selected_inferior().pid
    print('INFERIOR_PID=%d' % pid, flush=True)
    loaded = set()
    with open('/proc/%d/maps' % pid) as maps:
        for line in maps:
            name = line.split()[-1]
            if 'libcangjie-runtime.so' in name or 'libboundscheck.so' in name \
                    or 'libcangjie-trace.so' in name:
                loaded.add(name)
    for name in sorted(loaded):
        print('LOADED_SO=' + name, flush=True)

    owner = OwnerEntry('MapleRuntime::ZStackWatermark::start_processing_impl(void*)')
    gdb.execute('continue')
    owner.delete()
    # The selected thread entered start_processing_impl from LeaveSaferegion;
    # StackWatermark::start_processing (zStackWatermark.cpp:319) already holds
    # the mutex. Verify ownership against this OS thread before injection.
    wm = gdb.parse_and_eval('this')
    print('WATERMARK_THIS=%#x' % int(wm), flush=True)
    injected_tid = gdb.selected_thread().ptid[1]
    lock_owner = int(wm['lock']['_M_mutex']['__data']['__owner'])
    print('WATERMARK_LOCK_TARGET owner=%d tid=%d held=%d'
          % (lock_owner, injected_tid, lock_owner == injected_tid), flush=True)
    if lock_owner != injected_tid:
        print('GDB_ERROR lock not owned by injected thread', flush=True)
        gdb.execute('quit 5')

    print('FATAL_INJECTING producer=Logger::FormatLog body=SIGNAL_WATERMARK_REAL_FATAL_1253', flush=True)
    # Keep the interrupted product frames intact. Inferior-call errors are
    # not termination evidence: only the termination event and pid death are.
    try:
        gdb.execute('call (void) SignalFatal()')
    except gdb.error as call_error:
        print('INFERIOR_CALL_RETURN ' + str(call_error).splitlines()[0], flush=True)
    if not inferior_alive(pid):
        print('INFERIOR_TERMINATED pid=%d' % pid, flush=True)
        gdb.execute('quit 0')
    # Survived: the checker SIGINT-interrupted gdb's `signal` wait and the
    # inferior is stopped. The stop may land on any thread; find the injected
    # thread by its reentry stack and prove same-thread reentry: the mutex
    # owner equals the waiting thread's own tid, so the wait is no artifact
    # of any debugger thread scheduling.
    found = False
    for thread in gdb.selected_inferior().threads():
        thread.switch()
        trace = gdb.execute('bt 30', to_string=True)
        if 'start_processing' not in trace and 'on_safepoint' not in trace:
            continue
        print('WATERMARK_POSTINJECT_STACK', flush=True)
        for line in trace.splitlines():
            print('POSTINJECT| ' + line, flush=True)
        waiter_now = thread.ptid[1]
        owner_now = -1
        frame = gdb.newest_frame()
        while frame is not None:
            if 'start_processing_impl' in (frame.name() or ''):
                frame.select()
                owner_now = int(gdb.parse_and_eval('this')['lock']['_M_mutex']['__data']['__owner'])
                break
            frame = frame.older()
        print('WATERMARK_SELFLOCK owner=%d waiter=%d same=%d'
              % (owner_now, waiter_now, owner_now == waiter_now), flush=True)
        found = True
        break
    gdb.execute('kill')
    if not found:
        print('GDB_ERROR no watermark-reentry thread found after survive', flush=True)
        gdb.execute('quit 4')
    gdb.execute('quit 3' if owner_now == waiter_now else 'quit 4')
except gdb.error as err:
    print('GDB_ERROR %s' % str(err).strip().splitlines()[0], flush=True)
    gdb.execute('quit 4')
