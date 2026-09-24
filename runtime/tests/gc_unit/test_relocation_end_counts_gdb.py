# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe product relocation results before queue deactivation; never modify state.

RELOCATION_COUNTS_CASE=OldSmall|OldMedium|YoungSmall|YoungMedium
GCV2_RUNTIME_LIB_DIR=<product SO directory> gdb -batch -x this-file --args cj_gc_unit
"""
import gdb
import json
import os
from pathlib import Path


def command(text):
    return gdb.execute(text, to_string=True)


def emit(tag, **values):
    print(tag + ' ' + json.dumps(values, sort_keys=True))


try:
    case = os.environ.get('RELOCATION_COUNTS_CASE', 'OldSmall')
    fixture = 'RelocationEndCounts.' + case
    for setting in ['pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off']:
        command('set ' + setting)
    command('set environment GC_UNIT_FILTER ' + fixture)
    command('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    command('start')
    boundary = gdb.Breakpoint('MapleRuntime::ZRelocateQueue::deactivate()', temporary=True)
    command('continue')
    if boundary.is_valid():
        raise RuntimeError('Product queue deactivation was not reached')
    actual = Path(gdb.solib_name(gdb.newest_frame().pc())).resolve()
    expected = Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so').resolve()
    if actual != expected:
        raise RuntimeError('Product identity mismatch: ' + str(actual))
    stack = command('bt 12')
    if 'ZRelocate::relocate' not in stack or 'ForwardTask' not in stack:
        raise RuntimeError('Deactivation did not run through the product relocation task')
    generation = 'MapleRuntime::ZGeneration::' + ('_old' if case.startswith('Old') else '_young')
    small = int(gdb.parse_and_eval(generation + '->statRelocation._smallInPlaceCount'))
    medium = int(gdb.parse_and_eval(generation + '->statRelocation._mediumInPlaceCount'))
    active = bool(gdb.parse_and_eval('this->isActive._M_base._M_i'))
    # Each assertion is evaluated independently, so no prerequisite hides the target.
    target = small if 'Small' in case else medium
    other = medium if 'Small' in case else small
    emit('PRODUCT_RESULT', case=case, library=str(actual), stack=stack,
         small=small, medium=medium, active=active)
    counts_ok = target == 0 if case.endswith('Available') else target > 0
    emit('ASSERT_COUNTS_PUBLISHED_BEFORE_DEACTIVATE', passed=counts_ok and active)
    emit('ASSERT_OTHER_ALLOCATOR_ISOLATED', passed=other == 0)
    passed = counts_ok and active and other == 0
    command('continue')
    emit('FIXTURE_COMPLETE', exit_code=int(gdb.parse_and_eval('$_exitcode')))
    command('quit ' + ('0' if passed and int(gdb.parse_and_eval('$_exitcode')) == 0 else '1'))
except Exception as error:
    emit('HARNESS_ERROR', error=repr(error))
    command('quit 2')
