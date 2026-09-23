# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe real timer results and dispatch; do not write clock/statistics/decision results.

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
changed = os.environ.get('TIMER_CHANGE', '0') == '1'
expected_enabled = not explicit if changed else explicit
flag = 'MapleRuntime::ZCollectionInterval' + generation.capitalize()


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


def observe_timer_result():
    # Release inlines these static bool functions into run_thread. The return
    # value therefore lives in condition flags, not in an ABI return register.
    # Read the actual comparison result and execute its consuming branch; never
    # recompute the rule from the sampled interval or infer it from the port.
    function = 'static bool rule_' + generation + '_timer'
    guard = line_in(function, 'if (')
    expiry = line_in(function, 'return time_until_gc <= 0')
    advance(guard)
    architecture = gdb.selected_frame().architecture()
    if architecture.name() != 'i386:x86-64':
        raise RuntimeError('Timer result decoder requires x86-64')
    for comparison in ('disabled', 'expired'):
        for _ in range(40):
            pc = int(value('$pc'))
            instruction = architecture.disassemble(pc, count=1)[0]
            if instruction['asm'].startswith('ucomisd'):
                break
            command('nexti')
        else:
            raise RuntimeError('Timer comparison was not reached')
        location = gdb.find_pc_line(pc)
        expected_line = guard if comparison == 'disabled' else expiry
        if location.line != expected_line:
            raise RuntimeError('Unexpected timer comparison source: ' + str(location.line))
        emit('RULE_COMPARISON', generation=generation, comparison=comparison,
             pc=hex(pc), line=location.line, instruction=instruction['asm'])
        command('stepi')
        flags = int(value('$eflags'))
        branch_pc = int(value('$pc'))
        branch = architecture.disassemble(branch_pc, count=1)[0]
        if not branch['asm'].startswith('jae '):
            raise RuntimeError('Unsupported timer result branch: ' + branch['asm'])
        branch_target = int(branch['asm'].split()[1], 16)
        result = not bool(flags & 1)  # JAE consumes CF=0, including ordered equality.
        command('stepi')
        next_pc = int(value('$pc'))
        if (next_pc == branch_target) != result:
            raise RuntimeError('CPU branch does not match observed comparison flags')
        emit('RULE_RESULT_BRANCH', comparison=comparison, flags=flags,
             branch=branch['asm'], pc=hex(branch_pc), next_pc=hex(next_pc), taken=result)
        if comparison == 'expired' or result:
            returned = result if comparison == 'expired' else False
            passed = returned == expected_enabled
            emit('ASSERT_TIMER_RULE_RETURN', generation=generation, explicit=explicit,
                 returned=returned, exit=comparison, passed=passed)
            return passed
    raise RuntimeError('Timer rule did not return')


try:
    for setting in ('pagination off', 'confirm off', 'breakpoint pending on',
                    'print thread-events off'):
        command('set ' + setting)
    tick = line_in('static ZDirectorStats sample_stats', 'const uint64_t now')
    loop = line_in('void ZDirector::run_thread', 'while (wait_for_tick())')
    sample_end = line_in('static GCReason make_major_gc_decision', 'if (')
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
    mapped_interval = float(value(flag))
    mapping_ok = mapped_interval == (1.0 if explicit else -1.0)
    emit('ASSERT_TIMER_PARAMETER_MAPPING', interval=mapped_interval, passed=mapping_ok)
    if changed:
        command('set var ' + flag + '=' + ('1.0' if expected_enabled else '-1.0'))
    emit('SAMPLED', interval=mapped_interval, current=float(value(flag)), changed=changed,
         young_elapsed=float(value('stats.young_stats.cycle.timeSinceLast')),
         old_elapsed=float(value('stats.old_stats.cycle.timeSinceLast')))
    rule_passed = observe_timer_result()
    # Observe the complete real decision and send path up to the next loop tick.
    for _ in range(80):
        command('next')
        frame = gdb.newest_frame()
        location = frame.find_sal().line
        if frame.name() == 'MapleRuntime::ZDirector::run_thread' and loop <= location <= loop + 3:
            break
    else:
        raise RuntimeError('Director iteration did not finish')
    port = '$major' if generation == 'major' else '$minor'
    busy = bool(value(port + '->_has_message'))
    cause = int(value(port + '->_message._cause'))
    backup = busy and cause == 2
    passed = backup == expected_enabled
    emit('ASSERT_TIMER_DISPATCH', generation=generation, explicit=explicit,
         busy=busy, cause=cause, backup=backup, passed=passed)
    if expected_enabled and generation == 'minor' and backup:
        minor_driver = next(t for t in gdb.selected_inferior().threads() if t.name == 'ZDriverMinor')
        minor_driver.switch()
        bp = gdb.Breakpoint('MapleRuntime::ZDriver::ExecuteDriverRequest', temporary=True)
        command('continue')
        if bp.is_valid():
            raise RuntimeError('Minor driver did not consume timer request')
        received_cause = int(value('request._cause'))
        emit('ASSERT_MINOR_REQUEST', cause=received_cause, passed=received_cause == 2)
        bp = gdb.Breakpoint('MapleRuntime::ZDriver::RunYoungCollection', temporary=True)
        command('continue')
        if bp.is_valid():
            raise RuntimeError('Minor collection entry was not reached')
        young_type = int(value('type'))
        minor_type = int(value('MapleRuntime::ZYoungType::minor'))
        minor_started = young_type == minor_type and received_cause == 2
        emit('ASSERT_MINOR_COLLECTION', young_type=young_type, passed=minor_started)
        passed = passed and minor_started
    command('quit ' + ('0' if mapping_ok and rule_passed and passed else '1'))
except Exception as error:
    emit('HARNESS_ERROR', error=str(error))
    command('quit 2')
