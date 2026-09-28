# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
# Inject only at a real GC watermark entry after its mutex is acquired.
import gdb
import os

for cmd in ['set pagination off', 'set confirm off', 'set breakpoint pending on',
            'set print thread-events off', 'handle SIGUSR1 nostop noprint pass',
            'handle SIGUSR2 nostop noprint pass', 'handle SIGABRT nostop noprint pass',
            'handle SIGSEGV nostop noprint pass', 'handle SIGBUS nostop noprint pass',
            'handle SIGILL nostop noprint pass', 'handle SIGFPE nostop noprint pass']:
    gdb.execute(cmd)

class OwnerEntry(gdb.Breakpoint):
    def stop(self):
        # Only the owner entering from LeaveSaferegion, never the GC worker
        # processing a remote owner. No watermark/mutator state is fabricated.
        trace = gdb.execute('bt 18', to_string=True)
        if 'LeaveSaferegion' not in trace:
            return False
        print('WATERMARK_REAL_ENTRY\n' + trace, flush=True)
        return True

entry = gdb.Breakpoint('SignalWatermarkWork')
gdb.execute('run')
entry.delete()
owner = OwnerEntry('MapleRuntime::ZStackWatermark::start_processing_impl(void*)')
gdb.execute('continue')
owner.delete()
gdb.execute('set scheduler-locking on')
# Verify the mutex owner against this OS thread before fault injection.
wm = gdb.parse_and_eval('this')
lock_owner = int(wm['lock']['_M_mutex']['__data']['__owner'])
tid = gdb.selected_thread().ptid[1]
print('WATERMARK_LOCK_TARGET owner=%d tid=%d held=%d' % (lock_owner, tid, lock_owner == tid), flush=True)
if lock_owner != tid:
    gdb.execute('quit 5')
# Use the real Logger as producer; the injected call never returns in a correct
# process. A timeout controller records the old-path self-lock stack.
sig = int(os.environ.get('FATAL_SIGNAL', '6'))
if sig == 6:
    gdb.execute('call MapleRuntime::Logger::GetLogger().FormatLog(RTLOG_FATAL, false, "FATAL_WATERMARK_1253")')
else:
    gdb.execute('call (int)raise(%d)' % sig)
gdb.execute('continue')
