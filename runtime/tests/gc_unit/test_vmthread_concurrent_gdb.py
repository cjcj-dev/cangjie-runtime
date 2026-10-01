#!/usr/bin/env python3
"""Observe the product rendezvous and unblock on the real non-strong entry."""
import hashlib
import json
import os
from pathlib import Path
import gdb

state = {'entered': False, 'completed': False, 'unblock': False, 'error': None}
expected = (Path(os.environ['GCV2_RUNTIME_LIB_DIR']) / 'libcangjie-runtime.so').resolve()


def emit(tag, **values):
    print(tag + ' ' + json.dumps(values, sort_keys=True), flush=True)


def product():
    actual = Path(gdb.solib_name(gdb.newest_frame().pc())).resolve()
    if actual != expected:
        raise RuntimeError('Product identity mismatch: ' + str(actual))
    return hashlib.sha256(actual.read_bytes()).hexdigest()


def in_non_strong():
    frame = gdb.newest_frame()
    while frame is not None:
        if 'process_non_strong_references' in (frame.name() or ''):
            return True
        frame = frame.older()
    return False


class Completion(gdb.FinishBreakpoint):
    def __init__(self):
        super().__init__(gdb.newest_frame(), internal=True)

    def stop(self):
        state['completed'] = True
        emit('VM1349_RENDEZVOUS_COMPLETED', completed=True)
        return False


class Rendezvous(gdb.Breakpoint):
    def stop(self):
        try:
            digest = product()
            # Read the actual native thread TLS; never call a test helper.
            tls = gdb.parse_and_eval('(MapleRuntime::ThreadLocalData*)MapleRuntime::threadLocalData')
            on_vm = int(tls.dereference()['threadType']) == 5
            state['entered'] = True
            emit('VM1349_RENDEZVOUS_THREAD_TARGET', vm=on_vm, sha256=digest)
            if not on_vm or not in_non_strong():
                raise RuntimeError('Rendezvous executor is not VMThread on the product non-strong entry')
            Completion()
            return False
        except Exception as error:
            state['error'] = str(error)
            return True


class Unblock(gdb.Breakpoint):
    def stop(self):
        if not in_non_strong():
            return False
        product()
        state['unblock'] = True
        emit('VM1349_UNBLOCK_ORDER_TARGET', entered=state['entered'], completed=state['completed'])
        if not state['completed']:
            state['error'] = 'Unblock preceded completion of the GC rendezvous'
            return True
        return False


try:
    gdb.execute('set pagination off')
    gdb.execute('set breakpoint pending on')
    gdb.execute('set args --gtest_filter=ConcurrentVM1349.NonStrongRendezvousBeforeUnblock')
    Rendezvous('MapleRuntime::ZRendezvousGCThreads::doit()', internal=True)
    Unblock('MapleRuntime::ZResurrection::unblock()', internal=True)
    gdb.execute('run')
    if state['error'] or not state['unblock']:
        raise RuntimeError(state['error'] or 'Non-strong unblock was not observed')
    emit('VM1349_GDB_TARGET_PASS', **state)
    gdb.execute('quit 0')
except Exception as error:
    emit('VM1349_GDB_TARGET_FAIL', error=str(error), **state)
    gdb.execute('quit 1')
