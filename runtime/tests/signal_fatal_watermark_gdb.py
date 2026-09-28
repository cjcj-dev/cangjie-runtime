# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
# Inject FATAL only at a real GC watermark entry after its mutex is acquired.
# Verdicts derive from the inferior's own state (pid liveness, post-injection
# stack, mutex owner), never from gdb-internal messages alone.
import gdb
import os
import signal

for cmd in ['set pagination off', 'set confirm off', 'set breakpoint pending on',
            'set print thread-events off', 'handle SIGUSR1 nostop noprint pass',
            'handle SIGUSR2 nostop noprint pass', 'handle SIGABRT nostop noprint pass',
            'handle SIGSEGV nostop noprint pass', 'handle SIGBUS nostop noprint pass',
            'handle SIGILL nostop noprint pass', 'handle SIGFPE nostop noprint pass']:
    gdb.execute(cmd)


def inferior_alive(pid):
    if pid <= 0:
        return False
    if gdb.selected_inferior().pid == 0:
        return False
    return os.path.exists('/proc/%d' % pid)


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
    gdb.execute('set scheduler-locking on')
    wm = gdb.parse_and_eval('this')
    injected_tid = gdb.selected_thread().ptid[1]
    lock_owner = int(wm['lock']['_M_mutex']['__data']['__owner'])
    print('WATERMARK_LOCK_TARGET owner=%d tid=%d held=%d'
          % (lock_owner, injected_tid, lock_owner == injected_tid), flush=True)
    if lock_owner != injected_tid:
        print('GDB_ERROR lock not owned by injected thread', flush=True)
        gdb.execute('quit 5')

    sig = int(os.environ.get('FATAL_SIGNAL', '6'))
    print('FATAL_INJECTING signal=%d' % sig, flush=True)
    try:
        if sig == 6:
            gdb.execute('call MapleRuntime::Logger::GetLogger().FormatLog(RTLOG_FATAL, false, "FATAL_WATERMARK_1253")')
        else:
            gdb.execute('call (int)raise(%d)' % sig)
    except gdb.error as err:
        # Raised both when the inferior died mid-call (correct fatal path:
        # diagnostics then default raise) and when the checker SIGINT-stopped
        # a surviving inferior for stack capture. Distinguished by liveness.
        print('GDB_CALL_ENDED: %s' % str(err).strip().splitlines()[0], flush=True)

    if not inferior_alive(pid):
        print('INFERIOR_TERMINATED pid=%d' % pid, flush=True)
        gdb.execute('quit 0')

    # The inferior survived the fatal signal: capture the post-injection stack
    # and this same thread's mutex owner. owner == waiter proves same-thread
    # reentry, not a scheduler-locking artifact.
    print('WATERMARK_POSTINJECT_STACK', flush=True)
    trace = gdb.execute('bt 30', to_string=True)
    for line in trace.splitlines():
        print('POSTINJECT| ' + line, flush=True)
    owner_now = int(wm['lock']['_M_mutex']['__data']['__owner'])
    waiter_now = gdb.selected_thread().ptid[1]
    print('WATERMARK_SELFLOCK owner=%d waiter=%d same=%d'
          % (owner_now, waiter_now, owner_now == waiter_now), flush=True)
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    gdb.execute('quit 3' if owner_now == waiter_now else 'quit 4')
except gdb.error as err:
    print('GDB_ERROR %s' % str(err).strip().splitlines()[0], flush=True)
    gdb.execute('quit 4')
