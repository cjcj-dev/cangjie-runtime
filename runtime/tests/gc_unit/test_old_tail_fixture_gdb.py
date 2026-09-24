# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Exercise old_tail setup through its real GDB script and runtime entry.

Run gdb -nx -batch -ex 'source test_old_tail_fixture_gdb.py' <gc_unit ELF>
with DIRECTOR_SOURCE and GCV2_RUNTIME_LIB_DIR as for run_old_tail_gdb.sh.
This checks fixture setup only; test_old_tail_gdb.py retains the full GC test.
"""
import json
from pathlib import Path

import gdb


class FixtureParameters(gdb.Breakpoint):
    def stop(self):
        try:
            # The real runtime API's argument, before it initializes the heap.
            param = gdb.parse_and_eval('param').dereference()['gcParam']
            expected = dict(backupGCInterval=1, concGCThreads=2,
                            youngGCThreads=2, oldGCThreads=2)
            actual = {field: int(param[field]) for field in expected}
            passed = actual == expected
            print('ASSERT_FIXTURE_PARAMETERS ' + json.dumps(
                dict(actual=actual, expected=expected, passed=passed), sort_keys=True), flush=True)
            gdb.execute('quit ' + ('0' if passed else '1'))
        except Exception as error:
            print('FIXTURE_TEST_ERROR ' + repr(error), flush=True)
            gdb.execute('quit 2')
        return True


gdb.execute('set breakpoint pending on')
FixtureParameters('InitCJRuntime', internal=True)
gdb.execute('source ' + str(Path(__file__).with_name('test_old_tail_gdb.py')))
