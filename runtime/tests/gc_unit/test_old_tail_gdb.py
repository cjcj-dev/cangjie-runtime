# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe the real director decision while the real old driver is at relock.

Run on x86_64 with the gc_unit ELF. Scheduler locking holds the old driver at
its actual lock entry; the director independently samples the product workers.
No statistics, product instructions, or return values are changed.
"""
import gdb
import json
import os
import time
from pathlib import Path


def cmd(s):
    return gdb.execute(s, to_string=True)


def val(s):
    return gdb.parse_and_eval(s)


def emit(tag, **data):
    print(tag + ' ' + json.dumps(data, sort_keys=True))


def until(spec):
    bp = gdb.Breakpoint(spec, temporary=True)
    cmd('continue')
    if bp.is_valid():
        raise RuntimeError('Boundary not reached: ' + spec)


class Tail(gdb.Breakpoint):
    def stop(self):
        return gdb.selected_thread().name == 'ZDriverMajor'


try:
    for setting in ['pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off']:
        cmd('set ' + setting)
    fixture = 'GcDirector.ProductWarmupStopsAfterThreeCycles'
    cmd('set environment GC_UNIT_FILTER ' + fixture)
    cmd('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    fixture_source = Path(os.environ['DIRECTOR_SOURCE']).parents[3].joinpath(
        'tests/gc_unit/test_gc_director.cpp').read_text().splitlines()
    fixture_name = 'GC_RUNTIME_OTHER_VM_TEST(GcDirector, ProductWarmupStopsAfterThreeCycles)'
    start = next(i for i, line in enumerate(fixture_source) if fixture_name in line)
    init_line = next(i + 1 for i in range(start + 1, len(fixture_source))
                     if 'GC_EXPECT_EQ(InitCJRuntime' in fixture_source[i])
    init = gdb.Breakpoint('test_gc_director.cpp:' + str(init_line), temporary=True)
    cmd('run')
    location = gdb.selected_frame().find_sal()
    at_init = (not init.is_valid() and location.symtab is not None and
               Path(location.symtab.filename).name == 'test_gc_director.cpp' and
               location.line == init_line)
    emit('ASSERT_FIXTURE_INIT_BOUNDARY', actual=location.line, expected=init_line,
         passed=at_init)
    if not at_init:
        raise RuntimeError('Fixture did not stop before InitCJRuntime')
    cmd('set var params.gcParam.backupGCInterval=1')
    cmd('set var params.gcParam.concGCThreads=2')
    cmd('set var params.gcParam.youngGCThreads=2')
    cmd('set var params.gcParam.oldGCThreads=2')
    product = gdb.solib_name(int(val('(void*)&_ZN12MapleRuntime9ZDirector10run_threadEv')))
    if Path(product).resolve() != Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so').resolve():
        raise RuntimeError('Product identity mismatch')
    emit('PRODUCT_IDENTITY', library=product)
    # Observe service startup before waiting for old tail, so an allocator-
    # initiated first cycle cannot precede the director thread's naming.
    until('MapleRuntime::ZDirector::run_thread()')
    bp = Tail('MapleRuntime::ZGenerationOld::concurrent_relocate()')
    cmd('continue')
    bp.delete()
    old = gdb.selected_thread()
    cmd('finish')
    # At this point the concurrent relocation returned. The next lock is the
    # scope exit's relock, after destructor body in the fixed implementation.
    cmd('set scheduler-locking on')
    until('MapleRuntime::ZDriver::lock()')
    stack = cmd('bt')
    emit('OLD_RELOCK', stack=stack, thread=old.num)
    if 'ZGenerationOld::collect' not in stack:
        raise RuntimeError('Not the collection relock')
    emit('THREADS', listing=cmd('info threads'))
    director = next(t for t in gdb.selected_inferior().threads() if t.name == 'ZDirector')
    director.switch()
    source = Path(os.environ['DIRECTOR_SOURCE']).read_text().splitlines()
    # Release inlines sample_stats: its call-site line can resolve to the
    # run_thread epilogue. Stop at the timestamp producer inside the sample,
    # before the decision consumes it (ZGC zDirector.cpp:919, :612).
    sample = next(i for i, line in enumerate(source)
                  if line.startswith('static ZDirectorStats sample_stats('))
    end = next(i for i in range(sample + 1, len(source)) if source[i] == '}')
    tick = next(i + 1 for i in range(sample + 1, end)
                if 'const uint64_t now' in source[i])
    until('zDirector.cpp:' + str(tick))
    location = gdb.selected_frame().find_sal()
    emit('SAMPLE_BOUNDARY', requested=tick, actual=location.line,
         pc=hex(gdb.selected_frame().pc()), thread=gdb.selected_thread().num)
    # Let the public timer expire before the real sample obtains its timestamp.
    time.sleep(1.1)
    until('MapleRuntime::make_minor_gc_decision(MapleRuntime::ZDirectorStats const&)')
    resize = bool(val('stats.old_stats.resize.is_active'))
    emit('PRODUCT_SAMPLE', old_active=resize, thread=director.num)
    if os.environ.get('TAIL_ENUM') == '1':
        # Auxiliary -O0 product build only: read the actual enum return value.
        frame = gdb.newest_frame()
        if frame.type() == gdb.INLINE_FRAME:
            raise RuntimeError('Decision return optimized away')
        result = gdb.FinishBreakpoint(frame, internal=True)
        cmd('continue')
        if result.return_value is None:
            raise RuntimeError('No observable decision return value')
        actual = int(result.return_value)
        invalid = int(val('MapleRuntime::GC_REASON_INVALID'))
        passed = actual == invalid
        emit('ASSERT_OLD_TAIL_NO_MINOR', actual=actual, expected=invalid, passed=passed,
             sampled_old_active=resize, observation='auxiliary-enum')
    else:
        # Production Release can eliminate the enum entirely. Observe the
        # actual downstream branch, as authorized in the lane advisor reply.
        no_minor = gdb.Breakpoint('MapleRuntime::adjust_gc(MapleRuntime::ZDirectorStats const&)', temporary=True)
        minor = gdb.Breakpoint('MapleRuntime::start_minor_gc(MapleRuntime::ZDirectorStats const&, MapleRuntime::GCReason)', temporary=True)
        cmd('continue')
        observed = gdb.newest_frame().name()
        passed = observed == 'MapleRuntime::adjust_gc'
        if observed not in ('MapleRuntime::adjust_gc', 'MapleRuntime::start_minor_gc'):
            raise RuntimeError('No actual decision consumer observed: ' + str(observed))
        emit('ASSERT_OLD_TAIL_NO_MINOR', actual=observed, expected='MapleRuntime::adjust_gc',
             passed=passed, sampled_old_active=resize, observation='release-machine-branch',
             pc=hex(gdb.newest_frame().pc()))
    cmd('quit ' + ('0' if passed else '1'))
except Exception as error:
    emit('HARNESS_ERROR', error=repr(error))
    cmd('quit 2')
