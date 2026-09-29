# Run with gdb -batch -x this_file --args cj_gc_unit --gtest_filter=...
# Product callback counts complement the test's owner-retirement assertions.
# No product hooks, replacement symbols, or callback injection are used.
import gdb
import os

name = os.environ['GC_UNIT_FILTER']
symbols = {
    'WeakRootsProduct.CollectionReportsDeadToOwnerOnce':
        'MapleRuntime::FinalizerProcessor::ReportNumDead(unsigned long)',
    'SyncNativeWait.CollectionRetiresUnreachableOwnerOnce':
        'MapleRuntime::SyncRetireDead()',
}
if name not in symbols:
    raise RuntimeError('unsupported owner test: ' + name)

gdb.execute('set pagination off')
gdb.execute('set breakpoint pending on')
gdb.execute('set print thread-events off')
gdb.execute('set environment GC_UNIT_OTHER_VM_CHILD ' + name)

class OwnerCall(gdb.Breakpoint):
    def __init__(self, symbol):
        super().__init__(symbol, internal=False)
        self.calls = 0

    def stop(self):
        self.calls += 1
        gdb.write('WEAK_OWNER_PRODUCT_CALL test=%s count=%d\n' % (name, self.calls))
        return False

observed = OwnerCall(symbols[name])
exits = []
gdb.events.exited.connect(lambda event: exits.append(getattr(event, 'exit_code', None)))
gdb.execute('run')
passed = exits == [0] and observed.calls == 1
gdb.write('WEAK_OWNER_ONCE_ASSERT test=%s calls=%d exits=%s result=%s\n' %
          (name, observed.calls, exits, 'PASS' if passed else 'FAIL'))
gdb.execute('quit %d' % (0 if passed else 1))
