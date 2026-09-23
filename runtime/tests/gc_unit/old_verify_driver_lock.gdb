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

# Inspect the real collection frame in every stopped product thread. Relocate
# remains the generation phase between collections, so it cannot encode lifetime.
# The driver lock encloses collect(), including its collection-scope destructor.
samples = []
young_positive = {'mark': [], 'relocate': []}

def young_collections():
    selected = gdb.selected_thread()
    active = []
    try:
        for thread in gdb.selected_inferior().threads():
            thread.switch()
            frame = gdb.newest_frame()
            phase = 'other'
            while frame is not None:
                name = frame.name() or ''
                if name == 'MapleRuntime::ZGenerationYoung::concurrent_mark':
                    phase = 'mark'
                elif name == 'MapleRuntime::ZGenerationYoung::concurrent_relocate':
                    phase = 'relocate'
                if name == 'MapleRuntime::ZGenerationYoung::collect':
                    active.append((thread.num, phase))
                    break
                frame = frame.older()
    finally:
        selected.switch()
    return active

class VerifyEntry(gdb.Breakpoint):
    def stop(self):
        active = young_collections()
        samples.append(bool(active))
        print('OLD_VERIFY_TARGET sample=%d young_active=%d frames=%s' %
              (len(samples), bool(active), active))
        return False

class YoungEntry(gdb.Breakpoint):
    def __init__(self, phase, symbol):
        super().__init__(symbol)
        self.phase = phase

    def stop(self):
        active = young_collections()
        current = gdb.selected_thread().num
        observed = (current, self.phase) in active
        young_positive[self.phase].append(observed)
        print('OLD_VERIFY_POSITIVE phase=%s young_active=%d frames=%s' %
              (self.phase, observed, active))
        return False

VerifyEntry('MapleRuntime::VM_ZVerifyOld::name() const')
YoungEntry('mark', 'MapleRuntime::ZGenerationYoung::concurrent_mark()')
YoungEntry('relocate', 'MapleRuntime::ZGenerationYoung::concurrent_relocate()')
gdb.execute('run')
exit_code = int(gdb.parse_and_eval('$_exitcode'))
expected = os.environ.get('ZVerifyRoots') == '1' or os.environ.get('ZVerifyObjects') == '1'
qualified = exit_code == 0 and all(len(values) >= 50 and all(values)
                                 for values in young_positive.values())
qualified = qualified and (len(samples) >= 50 if expected else len(samples) == 0)
violations = sum(samples)
print('OLD_VERIFY_RESULT inferior_rc=%d samples=%d violations=%d mark_positive=%d relocate_positive=%d qualified=%d' %
      (exit_code, len(samples), violations, sum(young_positive['mark']),
       sum(young_positive['relocate']), qualified))
gdb.execute('quit %d' % (0 if qualified and violations == 0 else 1))
end
