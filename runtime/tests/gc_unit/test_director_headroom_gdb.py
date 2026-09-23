# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe live headroom consumption on the actual director thread.

The warmup fixture supplies real cycle statistics. Static/high-usage cases stop
an old collection; static additionally allocates a real young page before the
sample. No sampled statistics or decision results are written. The existing
product report records the free-space value consumed by each rule.
"""
import gdb
import json
import os
import re
from pathlib import Path


def command(text):
    return gdb.execute(text, to_string=True)


def value(text):
    return gdb.parse_and_eval(text)


def emit(tag, **fields):
    print(tag + ' ' + json.dumps(fields, sort_keys=True), flush=True)


def advance(spec, condition=None):
    bp = gdb.Breakpoint(spec, temporary=True)
    if condition:
        bp.condition = condition
    command('continue')
    if bp.is_valid():
        raise RuntimeError('Boundary not reached: ' + spec)


try:
    mode = os.environ.get('HEADROOM_SITE', 'dynamic')
    changed = os.environ.get('HEADROOM_CHANGE', '1') == '1'
    source = Path(os.environ['DIRECTOR_SOURCE']).read_text().splitlines()
    report = Path(os.environ.get('HEADROOM_REPORT', './director-headroom')).resolve()

    def line_in(function, text):
        start = next(i for i, line in enumerate(source) if function in line)
        return next(i + 1 for i in range(start + 1, len(source)) if text in source[i])

    for setting in ('pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off'):
        command('set ' + setting)
    fixture = 'GcDirector.ProductWarmupStopsAfterThreeCycles'
    command('set environment GC_UNIT_FILTER ' + fixture)
    command('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    command('set environment MRT_REPORT ' + str(report))
    gdb.Breakpoint('test_gc_director.cpp:121', temporary=True)
    command('run')
    parameters = [('backupGCInterval', 1 if mode == 'dynamic' else 0),
                  ('staticGCThreads', 0 if mode == 'dynamic' else 1),
                  ('concGCThreads', 2), ('youngGCThreads', 2), ('oldGCThreads', 2)]
    for field, number in parameters:
        command('set var params.gcParam.' + field + '=' + str(number))
    sample_end = line_in('static GCReason make_major_gc_decision', 'if (')
    if mode == 'dynamic':
        advance('zDirector.cpp:' + str(sample_end), 'stats.old_stats.cycle.isTimeTrustable')
        command('set scheduler-locking on')
    else:
        bp = gdb.Breakpoint('MapleRuntime::ZGenerationOld::concurrent_mark', temporary=True)
        bp.ignore_count = 1 if mode == 'static' else 0
        command('continue')
        if bp.is_valid():
            raise RuntimeError('Old mark entry not reached')
        command('set scheduler-locking on')
        if mode == 'static':
            # The first cycle promotes the retained object. Allocate an actual
            # young page so is_young_small does not skip the static rule.
            next(t for t in gdb.selected_inferior().threads() if t.num == 1).switch()
            command('set language c++')
            command('set $flags=(MapleRuntime::ZAllocationFlags*)calloc(1,sizeof(MapleRuntime::ZAllocationFlags))')
            command('set $page=MapleRuntime::Heap::alloc_page(8388608,MapleRuntime::ZPageType::large,'
                    'false,false,MapleRuntime::PageAge::eden,*$flags)')
            emit('REAL_YOUNG_PAGE', page=str(value('$page')))
        next(t for t in gdb.selected_inferior().threads() if t.name == 'ZDirector').switch()
        advance('zDirector.cpp:' + str(sample_end))
    library = gdb.solib_name(gdb.newest_frame().pc())
    if Path(library).resolve() != Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so').resolve():
        raise RuntimeError('Unexpected product library')
    emit('SAMPLE_ROUTE', young_used=str(value('stats.young_stats.general.used')),
         trust=str(value('stats.old_stats.cycle.isTimeTrustable')),
         active=str(value('stats.old_stats.resize.is_active')))
    capacity = int(value('stats.heap.soft_max_heap_size'))
    used = int(value('stats.heap.used'))
    # This 64 MiB fixture has no medium tier; headroom scales with ConcGCThreads.
    if bool(value('MapleRuntime::ZPageSizeMediumEnabled')):
        raise RuntimeError('Fixture unexpectedly enables medium pages')
    before = int(value('MapleRuntime::ZHeuristics::relocation_headroom()'))
    requested = 2 if not changed else (16 if mode == 'high' else 4)
    command('set var MapleRuntime::ConcGCThreads=' + str(requested))
    after = int(value('MapleRuntime::ZHeuristics::relocation_headroom()'))
    emit('LIVE_HEADROOM_INPUT', before=before, after=after, library=library, stack=command('bt'))
    sites = {
        'dynamic': ('static ZDriverRequest rule_minor_allocation_rate_dynamic',
                    'if (time_until_gc > time_until_oom', 'Rule Minor: Allocation Rate (Dynamic GC Workers)'),
        'static': ('static bool rule_minor_allocation_rate_static',
                   'return time_until_gc <= 0', 'Rule Minor: Allocation Rate (Static GC Workers)'),
        'high': ('static bool is_high_usage', 'return free_percent <=', 'Rule Minor: High Usage'),
    }
    function, target, tag = sites[mode]
    advance('zDirector.cpp:' + str(line_in(function, target)))
    log = Path(str(report) + '.' + str(gdb.selected_inferior().pid)).read_text()
    rows = [line for line in log.splitlines() if tag in line]
    observed = int(re.search(r'Free: (\d+)MB', rows[-1]).group(1))
    expected_headroom = before * requested // 2
    expected = max(capacity - used - expected_headroom, 0) // 1048576
    passed = observed == expected and after == expected_headroom
    emit('ASSERT_LIVE_HEADROOM_FREE', site=mode, changed=changed, observed=observed, expected=expected,
         capacity=capacity, used=used, passed=passed, stack=command('bt'), product_log=rows[-1])
    command('quit ' + ('0' if passed else '1'))
except Exception as error:
    emit('HARNESS_ERROR', error=str(error))
    command('quit 2')
