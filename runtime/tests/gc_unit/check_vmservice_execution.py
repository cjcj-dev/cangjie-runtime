# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
# Permanent all-stop observer of the product dump consumer. No instrumentation,
# replacement entry, or calls into the inferior are needed.
import gdb
import os

samples = []
errors = []
exits = []
vm_threads = []

class VMThreadEntry(gdb.Breakpoint):
    def stop(self):
        vm_threads.append(gdb.selected_thread().ptid[1])
        return False

class HeapWriter(gdb.Breakpoint):
    def stop(self):
        try:
            tls = gdb.parse_and_eval(
                '((MapleRuntime::ThreadLocalData*)MapleRuntime::threadLocalData)->threadType')
            tid = gdb.selected_thread().ptid[1]
            samples.append((int(tls), tid))
            print('VM1350_WRITER_OBSERVE type=%d tid=%d vm_threads=%s' %
                  (int(tls), tid, vm_threads))
        except Exception as error:
            errors.append(str(error))
        return False

gdb.execute('set pagination off')
gdb.execute('set confirm off')
gdb.execute('set breakpoint pending on')
VMThreadEntry('MapleRuntime::VMThread::run()', internal=True)
HeapWriter(os.environ.get('VM_SERVICE_OBSERVER_LOCATION',
                          'MapleRuntime::CjHeapData::WriteHeap()'), internal=True)
gdb.events.exited.connect(lambda event: exits.append(getattr(event, 'exit_code', None)))
gdb.execute('run')
single = os.environ.get("VM_SERVICE_OBSERVER_EXPECT_SINGLE", "1") == "1"
observed = len(samples) == 1 if single else len(samples) > 0
executed = exits == [0] and not errors and observed
on_vm = observed and not errors and all(kind == 5 and tid in vm_threads for kind, tid in samples)
print('ASSERT_VM1350_WRITER_EXECUTED samples=%d exits=%s errors=%s %s' %
      (len(samples), exits, errors, 'PASS' if executed else 'FAIL'))
print('ASSERT_VM1350_WRITER_VM_THREAD samples=%d %s' %
      (len(samples), 'PASS' if on_vm else 'FAIL'))
gdb.execute('quit %d' % (0 if executed and on_vm else 1))
