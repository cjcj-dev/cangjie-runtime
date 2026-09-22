# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe real timer dispatch; do not write clock/statistics/decision results.

Normal public configuration selects env/API and default/explicit interval.
For the minor case, product port and worker methods hold a major request busy,
matching the existing director busy matrix. Only the director then runs.
"""
import gdb
import json
import os
import time
from pathlib import Path

source = Path(os.environ['TIMER_SOURCE']).read_text().splitlines()
generation = os.environ['TIMER_GENERATION']
explicit = os.environ['TIMER_EXPLICIT'] == '1'


def line_in(function, text):
    start = next(i for i, line in enumerate(source) if function in line)
    return next(i + 1 for i in range(start + 1, len(source)) if text in source[i])


def command(text):
    output = gdb.execute(text, to_string=True)
    if 'Python Exception' in output:
        raise RuntimeError(output)
    return output


def value(text):
    return gdb.parse_and_eval(text)


def emit(tag, **fields):
    print(tag + ' ' + json.dumps(fields, sort_keys=True), flush=True)


def advance(line):
    bp = gdb.Breakpoint('zDirector.cpp:' + str(line), temporary=True)
    command('continue')
    if bp.is_valid():
        raise RuntimeError('Director boundary was not reached: ' + str(line))


try:
    for setting in ('pagination off', 'confirm off', 'breakpoint pending on',
                    'print thread-events off'):
        command('set ' + setting)
    tick = line_in('void ZDirector::run_thread', 'stats = sample_stats')
    loop = line_in('void ZDirector::run_thread', 'while (wait_for_tick())')
    sample_end = line_in('static ZDirectorStats sample_stats', 'stats.relocation_headroom')
    gdb.Breakpoint('zDirector.cpp:' + str(tick), temporary=True)
    command('run')
    director = gdb.selected_thread()
    product = gdb.solib_name(int(value('(void*)&_ZN12MapleRuntime9ZDirector10run_threadEv')))
    expected = Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so').resolve()
    emit('PRODUCT_IDENTITY', library=product, entry='MapleRuntime::ZDirector::run_thread')
    if Path(product).resolve() != expected:
        raise RuntimeError('Unexpected product library')
    command('set scheduler-locking on')
    command('set $major = &MapleRuntime::ZCollectedHeap::_collected_heap->_driver_major->_port')
    command('set $minor = &MapleRuntime::ZCollectedHeap::_collected_heap->_driver_minor->_port')
    if generation == 'minor':
        # Freeze driver consumption while establishing busy state via product methods.
        next(t for t in gdb.selected_inferior().threads() if t.num == 1).switch()
        command('set language c++')
        command('set $req = (MapleRuntime::ZDriverRequest*)malloc(sizeof(MapleRuntime::ZDriverRequest))')
        command('call ((void (*)(void*, unsigned int, unsigned int, unsigned int)) '
                '&_ZN12MapleRuntime14ZDriverRequestC1ENS_8GCReasonEjj)($req, 3, 1, 1)')
        command('call $major->send_async(*$req)')
        command('call MapleRuntime::ZCollectedHeap::_collected_heap->_heap._old.workers.get()->set_active()')
        emit('MAJOR_BUSY_INPUT', busy=bool(value('$major->_has_message')),
             cause=int(value('$major->_message._cause')))
        director.switch()
    # Exceed the old default without editing the clock or any cycle statistic.
    start = time.monotonic()
    time.sleep(float(os.environ.get('TIMER_WAIT_SECONDS', '241')))
    emit('ELAPSED', seconds=time.monotonic() - start)
    advance(sample_end)
    emit('SAMPLED', interval=float(value('stats.collection_interval_sec')),
         young_elapsed=float(value('stats.young_stats.cycle.timeSinceLast')),
         old_elapsed=float(value('stats.old_stats.cycle.timeSinceLast')))
    # Observe the complete real decision and send path up to the next loop tick.
    for _ in range(80):
        command('next')
        frame = gdb.newest_frame()
        location = frame.find_sal().line
        if frame.name() == 'MapleRuntime::ZDirector::run_thread' and loop <= location < tick:
            break
    else:
        raise RuntimeError('Director iteration did not finish')
    port = '$major' if generation == 'major' else '$minor'
    busy = bool(value(port + '->_has_message'))
    cause = int(value(port + '->_message._cause'))
    backup = busy and cause == 2
    passed = backup == explicit
    emit('ASSERT_TIMER_DISPATCH', generation=generation, explicit=explicit,
         busy=busy, cause=cause, backup=backup, passed=passed)
    command('quit ' + ('0' if passed else '1'))
except Exception as error:
    emit('HARNESS_ERROR', error=str(error))
    command('quit 2')
