# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Run with GDB against the real gc_unit ELF and its matching product SO.

The existing warmup fixture supplies a real runtime, allocation and (for the
merge rule) completed GC cycles. Only normal RuntimeParam inputs are adjusted.
A different stopped thread calls product port methods to schedule the busy
interleaving. No director statistics, return values or instructions are written.
This is an x86_64 debugger test: AL at the actual return PC is the bool result.
See run_director_busy_gdb.sh for the complete matrix and required environment.
"""
import gdb
import json
import os
import time
from pathlib import Path

SITE = os.environ['BUSY_SITE']
INITIAL = int(os.environ['BUSY_INITIAL'])
CURRENT = int(os.environ['BUSY_CURRENT'])
RESIZE = int(os.environ.get('BUSY_RESIZE', '0'))
EQUAL = int(os.environ.get('BUSY_EQUAL', '0'))
DYNAMIC = int(os.environ.get('BUSY_DYNAMIC', '1'))
SOURCE = Path(os.environ['DIRECTOR_SOURCE']).read_text().splitlines()
READS = []
RETURNS = set()
RULE_RESULTS = []


def line_in(function, text):
    start = next(i for i, line in enumerate(SOURCE) if function in line)
    return next(i + 1 for i in range(start + 1, len(SOURCE)) if text in SOURCE[i])


LINES = {
    'major': line_in('static GCReason make_major_gc_decision', 'if ('),
    'minor': line_in('static GCReason make_minor_gc_decision', 'if ('),
    'minor_major': line_in('static GCReason make_minor_gc_decision', 'resize.is_active'),
    'select': line_in('static void start_minor_gc', '? ZWorkerSelectionType'),
    'resize': line_in('static void start_minor_gc', 'if ('),
    'send': line_in('static void start_minor_gc', 'driver_minor()->port().send_async'),
    'merge': line_in('static bool start_gc', 'rule_major_allocation_rate(stats)'),
    'sample': line_in('static ZDirectorStats sample_stats', 'stats.mutator_alloc_rate'),
    'tick': line_in('void ZDirector::run_thread', 'const ZDirectorStats stats'),
    'loop': line_in('void ZDirector::run_thread', 'while (wait_for_tick())'),
    'entry': line_in('static ZDirectorStats sample_stats', 'stats.relocation_headroom'),
    'rule': line_in('static bool rule_major_allocation_rate', 'VLOG(REPORT'),
}


def command(text):
    output = gdb.execute(text, to_string=True)
    if 'Python Exception' in output:
        raise RuntimeError(output)
    return output


def value(expression):
    return gdb.parse_and_eval(expression)


def diagnostic(expression):
    # Diagnostic fields can disappear when the dispatch consumer is cut. Their
    # availability is not an invariant and must not mask the output assertion.
    try:
        return str(value(expression))
    except gdb.error as error:
        return '<unavailable: ' + str(error) + '>'


def emit(tag, **fields):
    print(tag + ' ' + json.dumps(fields, sort_keys=True))


def location():
    frame = gdb.newest_frame()
    return {'function': frame.name(), 'line': frame.find_sal().line,
            'pc': hex(frame.pc())}


def advance(tag):
    bp = gdb.Breakpoint('zDirector.cpp:' + str(LINES[tag]), temporary=True)
    command('continue')
    if bp.is_valid():
        raise RuntimeError('Product boundary not reached: ' + tag)


def controller():
    next(t for t in gdb.selected_inferior().threads() if t.num == 1).switch()
    command('set language c++')


def set_busy(port, state):
    controller()
    command('call ' + port + '->ack()')
    if state:
        command('call ' + port + '->send_async(*$req)')
    observed = int(value(port + '->is_busy()'))
    emit('PRODUCER_RESULT', port=port, requested=state, observed=observed)
    if observed != state:
        raise RuntimeError('Port producer prerequisite failed')
    director.switch()


class BusyReturn(gdb.Breakpoint):
    def __init__(self):
        caller = gdb.newest_frame().older()
        self.caller = caller.name()
        self.line = caller.find_sal().line
        self.address = caller.pc()
        RETURNS.add(self.address)
        super().__init__('*' + str(self.address), internal=True, temporary=True)

    def stop(self):
        RETURNS.discard(self.address)
        READS.append({'caller': self.caller, 'line': self.line,
                      'busy': int(value('$rax')) & 255})
        return False


class BusyRead(gdb.Breakpoint):
    def stop(self):
        if (gdb.selected_thread().name == 'ZDirector' and
                gdb.newest_frame().older().pc() not in RETURNS):
            BusyReturn()
        return False


class MajorRule(gdb.Breakpoint):
    def stop(self):
        if gdb.selected_thread().name == 'ZDirector':
            RULE_RESULTS.append({
                'time_trustable': bool(value('stats.old_stats.cycle.isTimeTrustable')),
                'extra_young_gc_time_for_lookahead': str(value('extra_young_gc_time_for_lookahead')),
                'old_gc_time': str(value('old_gc_time')),
                'location': location(),
            })
        return False


def check(tag, passed, **fields):
    emit(tag, passed=passed, **fields)
    command('quit ' + ('0' if passed else '1'))


try:
    for setting in ('pagination off', 'confirm off', 'breakpoint pending on',
                    'print thread-events off'):
        command('set ' + setting)
    command('set environment cjUseDynamicNumberOfGCThreads ' + str(DYNAMIC))
    fixture = 'GcDirector.ProductWarmupStopsAfterThreeCycles'
    command('set environment GC_UNIT_FILTER ' + fixture)
    command('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    gdb.Breakpoint('test_gc_director.cpp:121', temporary=True)
    command('run')
    command('set var params.gcParam.backupGCInterval=1')
    command('set var params.gcParam.concGCThreads=2')
    command('set var params.gcParam.youngGCThreads=2')
    command('set var params.gcParam.oldGCThreads=2')
    if SITE == 'merge':
        gdb.Breakpoint('test_gc_director.cpp:142', temporary=True)
        command('continue')
        emit('REAL_WARMUP', trustable=bool(value('stats.isTimeTrustable')),
             cycles=int(value('stats.warmupCycles')))
    product=gdb.solib_name(int(value('(void*)&_ZN12MapleRuntime9ZDirector10run_threadEv')))
    emit('PRODUCT_IDENTITY',library=product,entry='MapleRuntime::ZDirector::run_thread')
    if Path(product).resolve()!=Path(os.environ['GCV2_RUNTIME_LIB_DIR'],'libcangjie-runtime.so').resolve():
        raise RuntimeError('Loaded product SO differs from requested arm')
    advance('tick')
    director = gdb.selected_thread()
    command('set scheduler-locking on')
    # The public interval is clamped to one second. Wait before the product
    # obtains its sample timestamp, rather than editing any clock/statistic.
    time.sleep(1.1)
    advance('sample')
    command('set $major = &MapleRuntime::ZCollectedHeap::_collected_heap->_driver_major->_port')
    command('set $minor = &MapleRuntime::ZCollectedHeap::_collected_heap->_driver_minor->_port')
    controller()
    command('set $req = (MapleRuntime::ZDriverRequest*)malloc(sizeof(MapleRuntime::ZDriverRequest))')
    # Call the product constructor symbol, not an inline declaration from the ELF.
    command('call ((void (*)(void*, unsigned int, unsigned int, unsigned int)) '
            '&_ZN12MapleRuntime14ZDriverRequestC1ENS_8GCReasonEjj)($req, 3, 1, 1)')
    workers = 'MapleRuntime::ZCollectedHeap::_collected_heap->_heap._old.workers.get()'
    if not DYNAMIC or EQUAL:
        active_workers = (2 if EQUAL else 1) if not DYNAMIC else 1
        command('call ' + workers + '->set_active_workers(' + str(active_workers) + ')')
    if RESIZE:
        command('call ' + workers + '->set_active()')
    director.switch()
    set_busy('$major', INITIAL if SITE != 'minor' else 1)
    set_busy('$minor', INITIAL if SITE == 'minor' else 0)
    emit('SAMPLE_INPUT', site=SITE, initial=INITIAL, resize=RESIZE, equal=EQUAL)
    # Resolve the actual return statement from the matching source tree.
    port_lines = Path(os.environ['DIRECTOR_SOURCE']).with_name('zDriverPort.cpp').read_text().splitlines()
    port_return = next(i + 1 for i, text in enumerate(port_lines) if 'return _has_message;' in text)
    BusyRead('zDriverPort.cpp:' + str(port_return), internal=True)
    advance('entry')
    emit('SAMPLED', resize=diagnostic('stats.old_stats.resize.is_active'),
         workers=diagnostic('stats.old_stats.resize.nworkers_current'),
         interval=diagnostic('stats.collection_interval_sec'),
         old_minor_snapshot=diagnostic('stats.minor_busy'),
         old_major_snapshot=diagnostic('stats.major_busy'))
    if SITE == 'entry':
        set_busy('$major', CURRENT)
        for _ in range(64):
            command('next')
            here=location()
            if (LINES['loop'] <= here['line'] < LINES['tick'] and
                    here['function']=='MapleRuntime::ZDirector::run_thread'):
                break
        else:
            raise RuntimeError('Director loop boundary not observed')
        observed = {'busy': bool(value('$major->_has_message')),
                    'cause': int(value('$major->_message._cause'))}
        expected_cause = 3 if CURRENT else 2  # existing HEU / newly sent BACKUP
        check('ASSERT_ENTRY', observed['busy'] and observed['cause'] == expected_cause,
              observed=observed, expected_cause=expected_cause)
    advance('major')
    if SITE == 'major':
        set_busy('$major', CURRENT)
    else:
        set_busy('$major', 1)
        advance('minor')
        if SITE == 'minor':
            set_busy('$minor', CURRENT)
        else:
            advance('minor_major')
            set_busy('$major', CURRENT if SITE == 'minor_major' else 0)
            if SITE in ('merge', 'select', 'resize'):
                advance('merge')
                set_busy('$major', CURRENT if SITE == 'merge' else 1)
                if SITE in ('select', 'resize'):
                    advance('select')
                    set_busy('$major', CURRENT if SITE == 'select' else 1)
                    if SITE == 'resize':
                        advance('resize')
                        set_busy('$major', CURRENT)
    if SITE == 'merge':
        MajorRule('zDirector.cpp:' + str(LINES['rule']), internal=True)
    emit('TARGET_BEFORE', site=SITE, initial=INITIAL, current=CURRENT, location=location())
    if SITE == 'select':
        advance('resize')
    else:
        command('next')
    after = location()
    emit('TARGET_AFTER', location=after, product_busy_returns=READS)
    if SITE == 'merge':
        check('ASSERT_MERGE_GATE', bool(RULE_RESULTS) == (not bool(CURRENT)),
              rule_results=RULE_RESULTS, expected_enter=not bool(CURRENT))
    if SITE == 'select':
        set_busy('$major', 0)
        advance('send')
        command('next')
        observed = int(value('$minor->_message._young_nworkers'))
        expected = 1 if CURRENT else 2
        check('ASSERT_SELECTION', observed == expected,
              request_young_workers=observed, expected=expected)
    if SITE == 'resize':
        if location()['line'] != LINES['send']:
            advance('send')
        observed = int(value(workers + '->_requested_nworkers._M_i'))
        expected = 1 if DYNAMIC and CURRENT and not EQUAL else 0
        check('ASSERT_RESIZE', observed == expected, requested_workers=observed, expected=expected,
              dynamic=DYNAMIC, equal=EQUAL, current=CURRENT,
              sampled_workers=diagnostic('stats.old_stats.resize.nworkers_current'),
              selected_workers=diagnostic('selection.old_workers'))
    if SITE == 'major':
        rejected = after['function'] == 'MapleRuntime::start_gc'
    elif SITE == 'minor':
        rejected = after['function'] != 'MapleRuntime::make_minor_gc_decision'
    else:
        rejected = after['function'] == 'MapleRuntime::ZDirector::run_thread'
    expected = bool(CURRENT) and not (SITE == 'minor_major' and RESIZE)
    check('ASSERT_GATE', rejected == expected, expected_reject=expected, actual_reject=rejected)
except Exception as error:
    emit('HARNESS_ERROR', error=str(error))
    command('quit 2')
