# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Read real request payloads in the product SO; no product hooks or writes.

REQUEST_FIXTURE selects an existing runtime fixture. Run via gdb -batch -x
this-file --args cj_gc_unit, with GCV2_RUNTIME_LIB_DIR identifying the SO.
The ordinary fixture also asserts the completed generation/worker results.
"""
import gdb
import json
import os
from pathlib import Path

sent = []
received = []
failures = []
errors = []


def observe(stage, major):
    actual = Path(gdb.solib_name(gdb.newest_frame().pc())).resolve()
    expected = (Path(os.environ['GCV2_RUNTIME_LIB_DIR']) / 'libcangjie-runtime.so').resolve()
    if actual != expected:
        raise RuntimeError('Unexpected product SO: ' + str(actual))
    request = gdb.parse_and_eval('request')
    payload = tuple(int(request[field]) for field in ('_cause', '_young_nworkers', '_old_nworkers'))
    if stage == 'collect':
        stack = gdb.execute('bt', to_string=True)
        if 'ZCollectedHeap::collect' in stack or 'RegionManager::' in stack:
            wanted = (int(gdb.parse_and_eval('MapleRuntime::ZYoungGCThreads')),
                      int(gdb.parse_and_eval('MapleRuntime::ZOldGCThreads')) if major else 0)
            if payload[1:] != wanted:
                failures.append('producer_budget')
        if payload[1] == 0 or (major and payload[2] == 0):
            failures.append('nonzero_budget')
        sent.append(payload)
    else:
        # Compare the actual receiver's payload with the emitted request. A
        # producer bypassing collect cannot acquire a matching route receipt.
        if payload not in sent:
            failures.append('collect_route')
        received.append(payload)
    print('REQUEST_PAYLOAD_TARGET ' + json.dumps(dict(stage=stage, payload=payload,
          product=str(actual), failures=failures)), flush=True)
    return bool(failures)


class Observe(gdb.Breakpoint):
    def __init__(self, symbol, stage, major=False):
        super().__init__(symbol, internal=True)
        self.stage, self.major = stage, major

    def stop(self):
        try:
            return observe(self.stage, self.major)
        except Exception as error:
            errors.append(repr(error))
            return True


try:
    for setting in ('pagination off', 'confirm off', 'breakpoint pending on',
                    'print thread-events off', 'follow-fork-mode child'):
        gdb.execute('set ' + setting)
    fixture = os.environ.get('REQUEST_FIXTURE', 'RequestWorkers.ExternalUser')
    gdb.execute('set environment GC_UNIT_FILTER ' + fixture)
    gdb.execute('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    gdb.execute('start')
    Observe('MapleRuntime::ZDriverMinor::collect', 'collect')
    Observe('MapleRuntime::ZDriverMajor::collect', 'collect', True)
    Observe('MapleRuntime::ZDriver::ExecuteDriverRequest', 'execute')
    gdb.execute('continue')
    if errors:
        raise RuntimeError(str(errors))
    if failures:
        print('REQUEST_PAYLOAD_FAIL ' + json.dumps(failures), flush=True)
        gdb.execute('quit 1')
    if not received:
        raise RuntimeError('No received product payload observed')
    rc = int(gdb.parse_and_eval('$_exitcode'))
    print('REQUEST_PAYLOAD_RESULT ' + json.dumps(dict(fixture=fixture, rc=rc,
          sent=sent, received=received)), flush=True)
    gdb.execute('quit ' + str(rc))
except Exception as error:
    print('REQUEST_HARNESS_ERROR ' + repr(error), flush=True)
    gdb.execute('quit 2')
