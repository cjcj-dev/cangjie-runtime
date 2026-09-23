# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe constructor registration in the actual loaded product, before start.

Use the same gc_unit ELF with candidate/cut/restored SOs. DRIVER_KIND selects
minor or major. No product state or return value is modified by this observer.
"""
import gdb
import json
import os
from pathlib import Path

kind = os.environ['DRIVER_KIND']
assert kind in ('minor', 'major')
source = Path(os.environ['DRIVER_SOURCE']).read_text().splitlines()
constructor = 'ZDriver' + kind.title() + '::ZDriver' + kind.title() + '()'
start = next(i for i, line in enumerate(source) if constructor in line)
line = next(i + 1 for i in range(start + 1, len(source)) if 'create_and_start();' in source[i])
try:
    for setting in ('pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off'):
        gdb.execute('set ' + setting)
    gdb.execute('set environment GC_UNIT_FILTER DriverRegistration.ProductOwned' + kind.title())
    gdb.execute('set environment GC_UNIT_OTHER_VM_CHILD DriverRegistration.ProductOwned' + kind.title())
    gdb.Breakpoint('zDriver.cpp:' + str(line), temporary=True)
    gdb.execute('run')
    frame = gdb.newest_frame()
    product = gdb.solib_name(frame.pc())
    expected_product = Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so').resolve()
    if not product or Path(product).resolve() != expected_product:
        raise RuntimeError('Constructor is not from requested product SO: ' + str(product))
    owner = int(gdb.parse_and_eval('this'))
    registered = int(gdb.parse_and_eval('MapleRuntime::ZDriver::_' + kind))
    passed = registered == owner and owner != 0
    print('REGISTRATION_TARGET ' + json.dumps(dict(kind=kind, owner=owner,
        registered=registered, passed=passed, product=product,
        function=frame.name(), line=frame.find_sal().line), sort_keys=True))
    gdb.execute('quit ' + ('0' if passed else '1'))
except Exception as error:
    print('HARNESS_ERROR ' + str(error))
    gdb.execute('quit 2')
