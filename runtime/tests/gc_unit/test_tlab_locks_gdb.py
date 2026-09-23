# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe pthread lock acquisitions on the product TLAB retirement stack.

Run with run_tlab_locks_gdb.sh on Linux x86_64. Software breakpoints only;
no interposition, product state writes, counters, or test-only product hooks.
The ordinary fixture still checks the actual published allocation history.
ZGC: zThreadLocalAllocBuffer.cpp:65-78 retires into private statistics.
"""
import gdb
import json
import os
from pathlib import Path


def emit(tag, **data):
    print(tag + " " + json.dumps(data, sort_keys=True), flush=True)


def stack_names():
    names = []
    frame = gdb.newest_frame()
    while frame is not None:
        names.append(frame.name() or "<unknown>")
        frame = frame.older()
    return names


retirements = []
locks = []
errors = []
exit_codes = []
retire_name = "MapleRuntime::RegionManager::RetireTLAB"


class Retirement(gdb.Breakpoint):
    def stop(self):
        try:
            library = gdb.solib_name(gdb.newest_frame().pc())
            expected = Path(os.environ["GCV2_RUNTIME_LIB_DIR"], "libcangjie-runtime.so")
            if library is None or Path(library).resolve() != expected.resolve():
                raise RuntimeError("Retirement is not in the selected product SO")
            event = dict(thread=gdb.selected_thread().global_num, library=library,
                         stack=stack_names())
            retirements.append(event)
            emit("TLAB_RETIRE_PRODUCT", **event)
        except Exception as error:
            errors.append(repr(error))
        return False


class Lock(gdb.Breakpoint):
    def stop(self):
        try:
            names = stack_names()
            if any(retire_name in name for name in names):
                # SysV x86_64: pthread_mutex_lock's first argument is the
                # actual lock address. Record it before executing the lock.
                event = dict(thread=gdb.selected_thread().global_num,
                             address=hex(int(gdb.parse_and_eval("$rdi"))), stack=names)
                locks.append(event)
                emit("TLAB_RETIRE_LOCK", **event)
        except Exception as error:
            errors.append(repr(error))
        return False


try:
    for setting in ("pagination off", "confirm off", "breakpoint pending on",
                    "print thread-events off"):
        gdb.execute("set " + setting, to_string=True)
    fixture = "TLABSnapshot.ParallelRootPublicationPreservesLaterRefills"
    # Run the isolated runtime fixture directly in this inferior, not its
    # fork/exec parent; this is the runner's existing child protocol.
    gdb.execute("set environment GC_UNIT_FILTER " + fixture)
    gdb.execute("set environment GC_UNIT_OTHER_VM_CHILD " + fixture)
    Retirement(retire_name, internal=True)
    Lock("pthread_mutex_lock", internal=True)
    gdb.events.exited.connect(lambda event: exit_codes.append(getattr(event, "exit_code", None)))
    gdb.execute("run")
    if errors or exit_codes != [0] or len(retirements) < 2:
        raise RuntimeError("Observation incomplete: " + repr(dict(
            errors=errors, exit_codes=exit_codes, retirements=len(retirements))))
    owners = {}
    for event in locks:
        owners.setdefault(event["address"], set()).add(event["thread"])
    shared = sorted(address for address, threads in owners.items() if len(threads) > 1)
    # Retirement, accumulation and resize have no mutex acquisition in ZGC.
    # Check every observed acquisition, not only contended locks/futex calls.
    passed = not locks
    emit("ASSERT_TLAB_RETIRE_NO_MUTEX", executed=True, passed=passed,
         retirements=len(retirements), lock_acquisitions=len(locks),
         shared_addresses=shared, fixture_rc=exit_codes[0])
    gdb.execute("quit " + ("0" if passed else "1"))
except Exception as error:
    emit("HARNESS_ERROR", error=repr(error))
    gdb.execute("quit 2")
