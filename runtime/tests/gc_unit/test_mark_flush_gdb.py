# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Observe publication and the final OR result at worker0 proactive flush.

The test's ZBreakpoint controls the phase; GDB only reads product state and
return values and their direct consumer branch. Only test-owned input requests
are changed; no flush is called by the observer.
"""
import gdb
import json
import os
from pathlib import Path


def command(text):
    return gdb.execute(text, to_string=True)


snapshot = []


def population(address):
    global snapshot
    # Read the product-owned stack receipts without executing inferior calls.
    stacks = gdb.Value(address).cast(gdb.lookup_type(
        'MapleRuntime::MarkThreadLocalStacks').pointer()).dereference()['stacks']['_M_impl']
    begin, end = stacks['_M_start'], stacks['_M_finish']
    sizes = []
    snapshot = []
    for index in range(int(end - begin)):
        stack = (begin + index).dereference()
        if int(stack):
            top = int(stack.dereference()['top'])
            capacity = int(stack.dereference()['entries']['_length'])
            snapshot.append({'top': top, 'capacity': capacity})
            sizes.append(max(1, top))
    return sum(sizes)


sts_only = os.environ.get('MARK_FLUSH_STS') == '1'
mode = os.environ.get('MARK_FLUSH_INPUT', 'partial')
if mode not in ('partial', 'stripes', 'empty'):
    raise RuntimeError('unknown flush input: ' + mode)
result = {'mode': mode, 'before': None, 'after': None, 'handshake_return': None,
          'flush_return': None, 'entered': False, 'signal_targets': []}


def stripe_heads(mark):
    vector = mark.dereference()['stripes']['stripes']['_M_impl']
    begin, end = vector['_M_start'], vector['_M_finish']
    return [int((begin + i).dereference()[kind]['head']['_M_b']['_M_p'])
            for i in range(int(end - begin)) for kind in ('published', 'overflowed')]


class FlushConsumer(gdb.Breakpoint):
    def __init__(self, location, value):
        super().__init__(location, internal=True)
        self.value = value

    def stop(self):
        if gdb.selected_thread().num != active_worker:
            return False
        # Release builds inline Flush and TryProactiveFlush into FollowWork.
        # Its two immediate successors consume the final boolean directly:
        # true -> next Drain, false -> TryTerminate (ZGC zMark.cpp:635-664).
        result['flush_return'] = self.value
        result['consumer_pc'] = hex(gdb.selected_frame().pc())
        result['consumer_stack'] = command('bt')
        return True


active_worker = None


class Signal(gdb.Breakpoint):
    def stop(self):
        if gdb.selected_thread().num == active_worker:
            result['signal_targets'].append(int(gdb.parse_and_eval('$rdi')))
        return False


class WorkerZero(gdb.Breakpoint):
    def stop(self):
        return int(gdb.parse_and_eval('workerId')) == 0


class Returned(gdb.Breakpoint):
    def stop(self):
        result['handshake_return'] = bool(int(gdb.parse_and_eval('$al')))
        return True


try:
    for option in ['pagination off', 'confirm off', 'breakpoint pending on',
                   'print thread-events off']:
        command('set ' + option)
    fixture = 'ZMarkFlush.ConcurrentWorkerPublishesPartialMutatorStack'
    command('set environment GC_UNIT_FILTER ' + fixture)
    command('set environment GC_UNIT_OTHER_VM_CHILD ' + fixture)
    source = Path(os.environ['MARK_FLUSH_SOURCE']).read_text().splitlines()
    start = next(i for i, line in enumerate(source) if 'GC_RUNTIME_OTHER_VM_TEST(ZMarkFlush,' in line)
    line = next(i + 1 for i in range(start, len(source))
                if 'const bool reached = ConcurrentGCBreakpoints::RunTo' in source[i])
    ready = gdb.Breakpoint('test_value_root_identity.cpp:' + str(line), temporary=True)
    command('run')
    if ready.is_valid():
        raise RuntimeError('test setup boundary not reached')
    controller = gdb.selected_thread()
    stack_address = None
    request = gdb.parse_and_eval('requests')
    if request.type.sizeof != gdb.lookup_type('unsigned int').sizeof:
        raise RuntimeError('unsupported test request representation')
    request_address = int(request.address)
    owner = None
    for thread in gdb.selected_inferior().threads():
        thread.switch()
        frame = gdb.newest_frame()
        while frame:
            try:
                stack = frame.read_var('stacks')
                while stack.type.code in (gdb.TYPE_CODE_PTR, gdb.TYPE_CODE_REF, gdb.TYPE_CODE_RVALUE_REF):
                    stack = stack.referenced_value()
                stack_address = int(stack.address)
                owner = thread
                break
            except (gdb.error, ValueError):
                frame = frame.older()
        if stack_address is not None:
            break
    controller.switch()
    if stack_address is None:
        raise RuntimeError('owner private stack not found')
    boundary = gdb.Breakpoint('MapleRuntime::ZGenerationOld::mark_follow()', temporary=True)
    command('continue')
    if boundary.is_valid():
        raise RuntimeError('mark-follow phase entry not reached')
    driver = gdb.selected_thread()
    seeded_line = next(i + 1 for i in range(start, len(source)) if 'before = stacks.Population();' in source[i])

    def produce_input(value):
        owner.switch()
        command('set {unsigned int}' + str(request_address) + ' = ' + str(value))
        input_line = (next(i + 1 for i in range(start, len(source))
                           if 'STRIPES_INPUT_READY' in source[i]) if value == 2 else seeded_line)
        seeded = gdb.Breakpoint('test_value_root_identity.cpp:' + str(input_line), temporary=True)
        command('set scheduler-locking on')
        command('continue')
        if seeded.is_valid():
            raise RuntimeError('owner input boundary not reached')

    if mode == 'partial':
        produce_input(1)
    driver.switch()
    command('set scheduler-locking off')
    entry = WorkerZero('MapleRuntime::ZMark::TryProactiveFlush(unsigned long)')
    command('continue')
    if gdb.selected_inferior().pid:
        result['entered'] = True
        entry.delete()
        result['before'] = population(stack_address)
        result['before_stacks'] = snapshot
        result['input_partial'] = bool(snapshot) and all(0 < item['top'] < item['capacity'] for item in snapshot)
        product = gdb.solib_name(gdb.selected_frame().pc())
        expected = Path(os.environ['GCV2_RUNTIME_LIB_DIR'], 'libcangjie-runtime.so').resolve()
        if Path(product).resolve() != expected:
            raise RuntimeError('unexpected product identity')
        result['product'] = str(expected)
        if sts_only:
            # Freeze peers only across the worker's entry -> synchronous wait
            # boundary so the product global-count delta identifies this worker.
            result['sts_before'] = int(gdb.parse_and_eval(
                'MapleRuntime::SuspendibleThreadSet::nthreads'))
            result['worker_stack'] = command('bt')
            joined = result['sts_before'] > 0
            print('ASSERT_PROACTIVE_WORKER_JOINED ' + json.dumps(
                {'passed': joined, 'nthreads': result['sts_before']}), flush=True)
            if not joined:
                # The independent Joiner control must not enter a Leaver
                # without membership. It does not certify the flush fix.
                result['passed'] = False
                Path(os.environ['MARK_FLUSH_RESULT']).write_text(json.dumps(result) + '\n')
                command('quit 1')
            command('set scheduler-locking on')
        handshake = gdb.Breakpoint('MapleRuntime::ZMark::HandshakeFlush(MapleRuntime::ZMark*)', temporary=True)
        command('continue')
        if handshake.is_valid():
            raise RuntimeError('proactive handshake not reached')
        if sts_only:
            execute = gdb.Breakpoint('MapleRuntime::Handshake::execute(MapleRuntime::HandshakeClosure*)',
                                     temporary=True, internal=True)
            command('continue')
            if execute.is_valid():
                raise RuntimeError('synchronous handshake entry not reached')
            result['sts_during'] = int(gdb.parse_and_eval(
                'MapleRuntime::SuspendibleThreadSet::nthreads'))
            result['handshake_stack'] = command('bt')
            result['passed'] = (result['sts_before'] > 0 and
                                result['sts_during'] == result['sts_before'] - 1)
            print('ASSERT_PROACTIVE_WORKER_OUTSIDE_STS ' + json.dumps(result, sort_keys=True), flush=True)
            Path(os.environ['MARK_FLUSH_RESULT']).write_text(json.dumps(result) + '\n')
            command('quit ' + ('0' if result['passed'] else '1'))
        mark = gdb.parse_and_eval('domain')
        worker = gdb.selected_thread()
        frame = gdb.newest_frame()
        while frame.type() == gdb.INLINE_FRAME:
            frame = frame.older()
        active_worker = gdb.selected_thread().num
        result['finalizer_condition'] = int(gdb.parse_and_eval('&MapleRuntime::ZCollectedHeap::_collected_heap->_finalizer_processor.wakeCondition'))
        Signal('pthread_cond_signal', internal=True)
        print('MARK_FLUSH_STACK ' + command('bt'), flush=True)
        returned = Returned('*' + hex(frame.older().pc()), temporary=True, internal=True)
        command('continue')
        if returned.is_valid():
            raise RuntimeError('handshake return boundary not reached')
        result['after'] = population(stack_address)
        result['handshake_stack'] = command('bt')
        if mode == 'stripes':
            produce_input(2)
            # Detach has released this owner's stack storage. Do not read the
            # saved private-stack address after the producer has exited.
            result['detached_input'] = True
            worker.switch()
        # Freeze other producers/consumers for the few instructions from the
        # handshake result to Flush's actual consumer. No product calls here.
        command('set scheduler-locking on')
        result['stripe_heads'] = stripe_heads(mark)
        result['stripes_nonempty'] = any(result['stripe_heads'])
        product_source = Path(os.environ['MARK_FLUSH_PRODUCT_SOURCE']).read_text().splitlines()
        drain_line = next(i + 1 for i, text in enumerate(product_source)
                          if 'context.SetStripe(stripes.StripeForWorker(nworkers, workerId));' in text)
        terminate_line = next(i + 1 for i, text in enumerate(product_source)
                              if 'if (terminate.TryTerminate(stripes,' in text)
        FlushConsumer('zMark.cpp:' + str(drain_line), True)
        FlushConsumer('zMark.cpp:' + str(terminate_line), False)
        command('continue')
    result['no_finalizer_signal'] = result.get('finalizer_condition') not in result['signal_targets']
    result['published'] = result['after'] == 0
    if mode == 'partial':
        result['input_valid'] = result.get('input_partial', False) and result['handshake_return'] is True
    elif mode == 'stripes':
        result['input_valid'] = (result['before'] == 0 and result['handshake_return'] is False
                                 and result.get('detached_input') is True
                                 and result.get('stripes_nonempty') is True)
    else:
        result['input_valid'] = (result['before'] == 0 and result['handshake_return'] is False
                                 and result.get('stripes_nonempty') is False)
    result['expected_return'] = bool(result['handshake_return'] or result.get('stripes_nonempty'))
    result['return_correct'] = (result['flush_return'] is not None and
                                result['flush_return'] == result['expected_return'])
    result['passed'] = all((result['no_finalizer_signal'], result['entered'],
                          result['input_valid'], result['published'], result['return_correct']))
    print('MARK_PROACTIVE_ASSERT ' + json.dumps(result, sort_keys=True), flush=True)
    Path(os.environ['MARK_FLUSH_RESULT']).write_text(json.dumps(result) + '\n')
    command('quit ' + ('0' if result['passed'] else '1'))
except Exception as error:
    print('MARK_PROACTIVE_ERROR ' + repr(error), flush=True)
    command('quit 2')
