"""Park only the maintenance thread between product table operations."""
import json
import os
import time
from pathlib import Path

import gdb

fixture = os.environ['DEDUP_MAINTENANCE_FIXTURE']
maintenance_prefix = Path(os.environ['DEDUP_MAINTENANCE_FILE'])
source = Path(os.environ['DEDUP_MAINTENANCE_SOURCE']).resolve()
product = source.parents[2] / 'src/Heap/shared/stringdedup/stringDedup.cpp'
line = next(index for index, text in enumerate(product.read_text().splitlines(), 1)
            if 'while (!should_terminate()) {' in text)
result = {'fixture': fixture, 'window': None, 'inferior_rc': None, 'error': None}


def exited(event):
    result['inferior_rc'] = getattr(event, 'exit_code', None)


gdb.events.exited.connect(exited)
for option in ('pagination off', 'confirm off', 'breakpoint pending on',
               'print thread-events off', 'non-stop on'):
    gdb.execute('set ' + option, to_string=True)
gdb.execute('handle SIGSEGV nostop noprint pass', to_string=True)
gdb.execute('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture, to_string=True)
gdb.execute('set args --gtest_filter=' + fixture, to_string=True)


class Window(gdb.Breakpoint):
    def stop(self):
        try:
            owner = gdb.parse_and_eval('this->owner')
            table = owner['table']
            buckets = int(table['numberOfBuckets'])
            state = int(table['deadState']['_M_i'])
            shrink = fixture.endswith('ShrinkingOldBucketKeepsCanonicalIdentity')
            if (buckets == 503) != shrink:
                return False
            result['window'] = {'buckets': buckets, 'state': state,
                                'entries': int(table['numberOfEntries']),
                                'thread': gdb.selected_thread().global_num,
                                'file': str(product), 'line': line}
            print('DEDUP_MAINTENANCE_WINDOW ' + json.dumps(result['window']), flush=True)
            print('DEDUP_MAINTENANCE_PREFIX ' + str(maintenance_prefix), flush=True)
            Path(str(maintenance_prefix) + '.ready').write_text('ready\n')
            self.enabled = False
            return True
        except Exception as exc:
            result['error'] = str(exc)
            return True


try:
    symbol = '_ZN12MapleRuntime11StringDedup9Processor9WithTableIZNS1_12CleanupTableEbbEUlvE0_EEbT_'
    breakpoint = Window(symbol, internal=True)
    if fixture.endswith('ShrinkingOldBucketKeepsCanonicalIdentity'):
        breakpoint.enabled = False

        class ArmShrink(gdb.Breakpoint):
            calls = 0

            def stop(self):
                self.calls += 1
                if self.calls == 2:
                    breakpoint.enabled = True
                    self.enabled = False
                return False

        ArmShrink('DedupOldCycle', internal=True)
    gdb.execute('run')
    if result['window'] is not None and result['error'] is None:
        deadline = time.monotonic() + 25
        while not Path(str(maintenance_prefix) + '.release').exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        if not Path(str(maintenance_prefix) + '.release').exists():
            raise RuntimeError('fixture did not release maintenance window')
        gdb.execute('continue -a')
except Exception as exc:
    result['error'] = str(exc)
finally:
    Path(str(maintenance_prefix) + '.json').write_text(json.dumps(result, indent=2) + '\n')
    for suffix in ('.ready', '.release'):
        Path(str(maintenance_prefix) + suffix).unlink(missing_ok=True)
print('DEDUP_MAINTENANCE_RESULT ' + json.dumps(result), flush=True)
gdb.execute('quit ' + ('0' if result['window'] is not None and result['error'] is None
                      and result['inferior_rc'] == 0 else '1'))
