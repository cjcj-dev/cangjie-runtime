"""Read actual generation samples, reset stores and cycle records in the product SO.
No product counters, callbacks, result writes, or replacement entry points.
"""
import gdb
import json

failures = []
samples = []
starts = []
ends = []
resets = []
watch = None
cycle_inputs = {}
finished = False


def emit(tag, **fields):
    print(tag + ' ' + json.dumps(fields, sort_keys=True), flush=True)


def val(expr):
    return gdb.parse_and_eval(expr)


def stack():
    names = []
    frame = gdb.newest_frame()
    while frame:
        names.append(frame.name() or '?')
        frame = frame.older()
    return names


def check(name, passed, **fields):
    emit('ASSERT_' + name, passed=bool(passed), **fields)
    if not passed:
        failures.append(name)


def total(sampler):
    base = int(val('MapleRuntime::ZStatValue::base'))
    stride = int(val('MapleRuntime::ZStatValue::stride'))
    offset = int(sampler['offset'])
    cpus = int(val("(unsigned int)'MapleRuntime::ZCPU::count()::configured'"))
    inferior = gdb.selected_inferior()
    return sum(int.from_bytes(inferior.read_memory(base + stride * i + offset, 8), 'little')
               for i in range(cpus))


class ResetWatch(gdb.Breakpoint):
    def __init__(self, address):
        super().__init__('*(unsigned long long*)' + str(address), gdb.BP_WATCHPOINT,
                         wp_class=gdb.WP_WRITE, internal=True)
        self.address = address

    def stop(self):
        result = int(val('*(unsigned long long*)' + str(self.address)))
        frames = stack()
        resets.append((result, frames))
        emit('RESET_RESULT', value=result, stack=frames)
        return False


class Entry(gdb.Breakpoint):
    def stop(self):
        global watch
        cycle_inputs[gdb.selected_thread().global_num] = {
            name: tuple(int(val('MapleRuntime::ZGeneration::_' + name + '->cycleStats.' + field))
                        for field in ('start', 'end')) for name in ('young', 'old')}
        if watch is None:
            address = int(val('&MapleRuntime::ZGeneration::_old->_freed'))
            check('RESET_INPUT', int(val('*(unsigned long long*)' + str(address))) == 37)
            watch = ResetWatch(address)
        return False


pending = {}


class Sample(gdb.Breakpoint):
    def stop(self):
        # Keep the real sampler store and its read in one debugger scheduling
        # interval: the service thread otherwise CollectAndReset()s the counter.
        # This short path contains only atomic sampling, no cross-thread waits.
        gdb.execute('set scheduler-locking on')
        sampler = val('this').dereference()['sampler']
        if sampler['group'].string() == 'Young Generation':
            kind = int(val('MapleRuntime::ZGeneration::_young->youngType._M_i'))
            expected = int(val('&MapleRuntime::ZPhaseGenerationYoung[%d].sampler' % kind))
        else:
            kind = 4
            expected = int(val('&MapleRuntime::ZPhaseGenerationOld.sampler'))
        pending[gdb.selected_thread().global_num] = (sampler, total(sampler), kind, expected, stack())
        return False


class SampleDone(gdb.Breakpoint):
    def stop(self):
        item = pending.pop(gdb.selected_thread().global_num, None)
        if item is None:
            return False
        sampler, before, kind, expected, frames = item
        after = total(sampler)
        gdb.execute('set scheduler-locking off')
        name = 'old' if kind == 4 else 'young'
        before_cycle = cycle_inputs[gdb.selected_thread().global_num][name]
        for index, field in enumerate(('start', 'end')):
            current = int(val('MapleRuntime::ZGeneration::_' + name + '->cycleStats.' + field))
            check('CYCLE_' + field.upper() + '_STORE', current > before_cycle[index],
                  before=before_cycle[index], after=current, generation=name)
        address = int(sampler.address)
        samples.append((kind, address, after - before))
        check('TYPE_TIMER_SAMPLE', address == expected and after - before == 1,
              type=kind, actual=hex(address), expected=hex(expected),
              before=before, after=after, stack=frames)
        return False


class CycleEndDone(gdb.FinishBreakpoint):
    def __init__(self, cycle, frames):
        super().__init__(gdb.newest_frame(), internal=True)
        self.cycle = cycle
        self.frames = frames

    def stop(self):
        record = self.cycle.dereference()
        result = bool(record['hasEnded']) and int(record['end']) >= int(record['start'])
        ends.append(result)
        check('CYCLE_END_RESULT', result and any('at_collection_end' in f for f in self.frames),
              start=int(record['start']), end=int(record['end']), stack=self.frames)
        return False


class CycleStart(gdb.Breakpoint):
    def stop(self):
        frames = stack()
        starts.append(frames)
        check('CYCLE_START_OWNER', any('at_collection_start' in f for f in frames), stack=frames)
        return False


class CycleEnd(gdb.Breakpoint):
    def stop(self):
        CycleEndDone(val('this'), stack())
        return False


class Complete(gdb.Breakpoint):
    def stop(self):
        global finished
        finished = True
        kinds = sorted(x[0] for x in samples)
        check('TYPE_TIMER_COVERAGE', kinds == [0, 1, 2, 3, 4, 4], observed=kinds)
        # The input changes from 37 to zero once. Its real store must execute
        # inside the combined mark-start VM operation, never collection-start.
        reset_ok = len(resets) == 1 and resets[0][0] == 0 and any(
            'VM_ZMarkStartYoungAndOld::do_operation' in f for f in resets[0][1])
        check('OLD_RESET_IN_MARK_PAUSE', reset_ok, stores=len(resets), results=resets)
        check('TYPE_RESTORED', int(val('MapleRuntime::ZGeneration::_young->youngType._M_i')) == 4)
        check('CYCLE_COVERAGE', len(starts) == 6 and len(ends) == 6,
              starts=len(starts), ends=len(ends))
        return True


try:
    for setting in ('pagination off', 'confirm off', 'breakpoint pending on', 'print thread-events off'):
        gdb.execute('set ' + setting)
    Entry('MapleRuntime::ZDriver::RunGarbageCollection', internal=True)
    Sample('MapleRuntime::ZStatPhaseGeneration::RegisterEnd', internal=True)
    SampleDone('MapleRuntime::ZStatHeap::PrintStalls', internal=True)
    CycleStart('MapleRuntime::ZStatCycle::AtStart', internal=True)
    CycleEnd('MapleRuntime::ZStatCycle::AtEnd', internal=True)
    Complete('CollectionScopeFixtureComplete', internal=True)
    gdb.execute('run')
    if not finished:
        raise RuntimeError('Fixture did not reach completion')
    emit('SCOPE_RESULT', failures=failures, samples=len(samples), resets=len(resets))
    gdb.execute('quit ' + ('1' if failures else '0'))
except Exception as error:
    emit('SCOPE_HARNESS_ERROR', error=str(error))
    gdb.execute('quit 2')
