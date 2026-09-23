# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe Linux product mutex ownership at the real driver request entry.

The warmup fixture allocates through the runtime. No request, lock state, or
return value is synthesized. This observer requires the Linux glibc ZLock
backing used by the product, and does not write any inferior state.
"""
import gdb
import json
import os
from pathlib import Path

try:
    for setting in ('pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off'):
        gdb.execute('set ' + setting)
    fixture = 'GcDirector.ProductWarmupStopsAfterThreeCycles'
    gdb.execute('set environment GC_UNIT_FILTER ' + fixture)
    gdb.execute('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    gdb.Breakpoint('MapleRuntime::ZDriver::ExecuteDriverRequest', temporary=True)
    gdb.execute('run')
    frame = gdb.newest_frame()
    product = gdb.solib_name(frame.pc())
    expected_product = Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so').resolve()
    if not product or Path(product).resolve() != expected_product:
        raise RuntimeError('Request entry is not from requested product SO: ' + str(product))
    lock = gdb.parse_and_eval('MapleRuntime::ZDriver::_lock')
    owner = int(lock.dereference()['_M_mutex']['__data']['__owner']) if int(lock) else 0
    tid = gdb.selected_thread().ptid[1]
    thread = gdb.selected_thread().name
    passed = owner == tid and thread in ('ZDriverMinor', 'ZDriverMajor')
    print('DRIVER_LOCK_TARGET ' + json.dumps(dict(owner=owner, driver_tid=tid,
        thread=thread, lock=int(lock), passed=passed, product=product,
        function=frame.name(), line=frame.find_sal().line), sort_keys=True))
    gdb.execute('quit ' + ('0' if passed else '1'))
except Exception as error:
    print('HARNESS_ERROR ' + str(error))
    gdb.execute('quit 2')
