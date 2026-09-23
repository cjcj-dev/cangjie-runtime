set pagination off
set confirm off
set breakpoint pending on
set print thread-events off
handle SIGSEGV nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
python
import gdb
import os

samples = []
young_active_samples = []

class VerifyEntry(gdb.Breakpoint):
    def stop(self):
        active = bool(gdb.parse_and_eval('observed_young->active'))
        samples.append(active)
        print('OLD_VERIFY_TARGET sample=%d young_active=%d' % (len(samples), active))
        return False

class YoungEntry(gdb.Breakpoint):
    def stop(self):
        active = bool(gdb.parse_and_eval('observed_young->active'))
        young_active_samples.append(active)
        return False

VerifyEntry('MapleRuntime::VM_ZVerifyOld::name() const')
YoungEntry('MapleRuntime::ZGenerationYoung::concurrent_mark()')
gdb.execute('run')
exit_code = int(gdb.parse_and_eval('$_exitcode'))
expected = os.environ.get('ZVerifyRoots') == '1' or os.environ.get('ZVerifyObjects') == '1'
qualified = exit_code == 0 and len(young_active_samples) >= 50 and all(young_active_samples)
qualified = qualified and (len(samples) >= 50 if expected else len(samples) == 0)
violations = sum(samples)
print('OLD_VERIFY_RESULT inferior_rc=%d samples=%d violations=%d young_positive=%d qualified=%d' %
      (exit_code, len(samples), violations, sum(young_active_samples), qualified))
gdb.execute('quit %d' % (0 if qualified and violations == 0 else 1))
end
