#!/usr/bin/env python3
"""Non-stop GDB observation of real native detach versus the old mark-end.

Only test scheduling flags are written. Product TLS, stacks, STS state and
published lists are read without replacement or injected product callbacks.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import subprocess
import threading
import time


def emit(tag, **fields):
    print(tag + ' ' + json.dumps(fields, sort_keys=True), flush=True)


class MI:
    def __init__(self, elf, log):
        self.process = subprocess.Popen(['gdb', '-nx', '-q', '--interpreter=mi2', elf],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.lines = queue.Queue()
        self.stops = []
        self.token = 0
        self.pid = None
        self.log = open(log, 'w')
        def reader():
            for line in self.process.stdout:
                self.lines.put(line.rstrip('\n'))
            self.lines.put(None)
        threading.Thread(target=reader, daemon=True).start()

    def line(self, timeout=90):
        line = self.lines.get(timeout=timeout)
        if line is None:
            raise RuntimeError('GDB exited before completion')
        self.log.write(line + '\n')
        self.log.flush()
        match = re.search(r'thread-group-started,.*pid="(\d+)"', line)
        if match:
            self.pid = int(match[1])
        if line.startswith('*stopped'):
            self.stops.append(line)
        if line.startswith('=thread-group-exited'):
            self.stops.append(line)
        return line

    def cmd(self, command):
        self.token += 1
        token = str(self.token)
        self.log.write('COMMAND ' + command + '\n')
        self.process.stdin.write(token + command + '\n')
        self.process.stdin.flush()
        while True:
            line = self.line()
            if line.startswith(token + '^error'):
                raise RuntimeError(command + ': ' + line)
            if line.startswith(token + '^'):
                return line

    def console(self, command):
        return self.cmd('-interpreter-exec console ' + json.dumps(command))

    def stop(self):
        deadline = time.monotonic() + 90
        while not self.stops:
            self.line(max(0.1, deadline - time.monotonic()))
        event = self.stops.pop(0)
        emit('GDB_STOP', event=event)
        return event

    def expression(self, expression, thread=None):
        context = '' if thread is None else '--thread ' + thread + ' '
        result = self.cmd('-data-evaluate-expression ' + context + json.dumps(expression))
        match = re.search(r'value=("(?:[^"\\]|\\.)*")', result)
        if not match:
            raise RuntimeError('No expression result: ' + result)
        return json.loads(match[1])

    def number(self, expression, thread=None):
        value = self.expression('(unsigned long long)(' + expression + ')', thread)
        return int(value.split()[0], 0)

    def breakpoint(self, location, condition=None, thread=None):
        command = '-break-insert -f '
        if condition:
            command += '-c ' + json.dumps(condition) + ' '
        if thread:
            command += '-p ' + thread + ' '
        result = self.cmd(command + json.dumps(location))
        return re.search(r'number="([0-9]+)', result)[1]

    def resume(self, thread):
        self.cmd('-exec-continue --thread ' + thread)

    def delete(self, bp):
        self.cmd('-break-delete ' + bp)


def event_field(event, name):
    match = re.search(name + r'="([^"]*)"', event)
    if not match:
        raise RuntimeError('Missing ' + name + ': ' + event)
    return match[1]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('elf')
    parser.add_argument('libdir')
    parser.add_argument('log')
    parser.add_argument('--reverse', action='store_true')
    parser.add_argument('--young', action='store_true')
    args = parser.parse_args()
    os.environ['LD_LIBRARY_PATH'] = str(Path(args.libdir).resolve())
    os.environ['GC_NATIVE_GDB'] = '1'
    os.environ['GC_UNIT_FILTER'] = 'NativeOwner1286.FinalPublication'
    os.environ['GC_UNIT_OTHER_VM_CHILD'] = 'NativeOwner1286.FinalPublication'
    for name in ['GC_UNIT_LIST_TESTS', 'GC_UNIT_TALLY_FILE']:
        os.environ.pop(name, None)
    so = Path(args.libdir).resolve() / 'libcangjie-runtime.so'
    emit('NATIVE_IDENTITY', elf=digest(args.elf), so=digest(so), library=str(so))
    mi = MI(args.elf, args.log)
    try:
        mi.console('set pagination off')
        mi.console('set confirm off')
        mi.console('set non-stop on')
        mi.console('set mi-async on')
        mi.console('set print thread-events off')
        mi.console('handle SIGUSR1 nostop noprint pass')
        ready = mi.breakpoint('native1286_ready')
        mi.cmd('-exec-run')
        event = mi.stop()
        if event_field(event, 'bkptno') != ready:
            raise RuntimeError('Fixture did not reach ready: ' + event)
        main_thread = event_field(event, 'thread-id')
        maps = Path('/proc/' + str(mi.pid) + '/maps').read_text()
        loaded = [line for line in maps.splitlines() if str(so) in line]
        if not loaded:
            raise RuntimeError('Expected SO missing from running process maps')
        emit('NATIVE_PRODUCT_MAPS', entries=loaded)
        mi.delete(ready)
        owner = mi.number('native1286_data', main_thread)
        native = mi.number('native1286_data->managedOwner', main_thread)
        if native != 0:
            raise RuntimeError('Target is a managed owner')
        phase_thread = main_thread
        producer_thread = None
        if args.young:
            young_end = mi.breakpoint('MapleRuntime::ZGenerationYoung::pause_mark_end')
            produced = mi.breakpoint('native1286_produced')
            mi.resume(main_thread)
            event = mi.stop()
            if event_field(event, 'bkptno') != young_end:
                raise RuntimeError('Real young mark-end not reached')
            phase_thread = event_field(event, 'thread-id')
            mi.delete(young_end)
            mi.expression('native1286_start_producer._M_i = 1', phase_thread)
            event = mi.stop()
            if event_field(event, 'bkptno') != produced:
                raise RuntimeError('Young native production not reached')
            producer_thread = event_field(event, 'thread-id')
            mi.delete(produced)
        generation = 1 if args.young else 0
        if args.reverse:
            if producer_thread:
                mi.resume(producer_thread)
            flush = mi.breakpoint('MapleRuntime::ThreadGCData::FlushMarkStacks',
                                  'this == native1286_data && (int)domain.generation == ' + str(generation))
            mi.resume(phase_thread)
            event = mi.stop()
            if event_field(event, 'bkptno') != flush:
                raise RuntimeError('Native mark-end consumer not reached')
            driver = event_field(event, 'thread-id')
            mi.delete(flush)
            join_wait = mi.breakpoint('SuspendibleThreadSet.cpp:36')
            publication = mi.breakpoint('MapleRuntime::MarkStripeStackList::Push')
            mi.expression('native1286_allow_exit._M_i = 1', driver)
            event = mi.stop()
            safe = event_field(event, 'bkptno') == join_wait
            suspended = mi.number('MapleRuntime::SuspendibleThreadSet::suspendAll._M_base._M_i', driver)
            emit('NATIVE_PAUSE_DETACH_EXCLUSION', executed=1, passed=safe and suspended == 1,
                 owner=owner, suspend_all=suspended, event=event)
            if not safe or suspended != 1:
                return 1
            exiting = event_field(event, 'thread-id')
            mi.delete(join_wait)
            mi.resume(driver)
            event = mi.stop()
            if event_field(event, 'bkptno') != publication:
                raise RuntimeError('Mark-end publication not reached')
            publisher = event_field(event, 'thread-id')
        else:
            removal = mi.breakpoint('MapleRuntime::CleanThreadLocalData::RemoveFromList',
                                    '&this->nativeData == native1286_data')
            mi.expression('native1286_allow_exit._M_i = 1', phase_thread)
            if producer_thread:
                mi.resume(producer_thread)
            event = mi.stop()
            if event_field(event, 'bkptno') != removal:
                raise RuntimeError('Natural native remove not reached')
            exiting = event_field(event, 'thread-id')
            mi.delete(removal)
            publication = mi.breakpoint('MapleRuntime::MarkStripeStackList::Push', thread=exiting)
            mi.resume(exiting)
            event = mi.stop()
            if event_field(event, 'bkptno') != publication:
                raise RuntimeError('Detach publication not reached')
            publisher = exiting
            # Stop only the native publisher; the driver must actually enter
            # its pause and reach either the waiting branch or this consumer.
            wait = mi.breakpoint('SuspendibleThreadSet.cpp:80')
            flush = mi.breakpoint('MapleRuntime::ThreadGCData::FlushMarkStacks',
                                  'this == native1286_data')
            mi.resume(phase_thread)
            event = mi.stop()
            driver = event_field(event, 'thread-id')
            admitted = mi.number('MapleRuntime::SuspendibleThreadSet::nthreads', driver)
            stopped = mi.number('MapleRuntime::SuspendibleThreadSet::nthreadsStopped', driver)
            suspended = mi.number('MapleRuntime::SuspendibleThreadSet::suspendAll._M_base._M_i', driver)
            safe = event_field(event, 'bkptno') == wait and admitted > stopped and suspended == 1
            emit('NATIVE_DETACH_MARK_END_EXCLUSION', executed=1, passed=safe, owner=owner,
                 admitted=admitted, stopped=stopped, suspend_all=suspended, event=event)
            if not safe:
                return 1
            mi.delete(wait)
            mi.delete(flush)
        mi.delete(publication)
        stack = mi.number('stack', publisher)
        mi.expression('$native_list = this', publisher)
        mi.expression('$native_stack = stack', publisher)
        mi.cmd('-exec-finish --thread ' + publisher)
        mi.stop()
        node = mi.number('$native_list->head._M_b._M_p', publisher)
        matches = 0
        seen = set()
        while node:
            if node in seen:
                raise RuntimeError('Published list contains a cycle')
            seen.add(node)
            expr = '((MapleRuntime::MarkStripeStackListNode*)' + str(node) + ')'
            matches += mi.number(expr + '->stack', publisher) == stack
            node = mi.number(expr + '->next', publisher)
        emit('NATIVE_STACK_SINGLE_PUBLICATION', executed=1, passed=matches == 1,
             stack=stack, references=matches)
        if matches != 1:
            return 1
        if args.young:
            selected = mi.breakpoint('MapleRuntime::ZGenerationYoung::concurrent_select_relocation_set')
        mi.resume(publisher)
        mi.resume(exiting if args.reverse else driver)
        event = mi.stop()
        if args.young:
            if event_field(event, 'bkptno') != selected:
                raise RuntimeError('Young mark completion not reached')
            completion_thread = event_field(event, 'thread-id')
            mi.delete(selected)
            checked = mi.breakpoint('native1286_checked')
            mi.expression('native1286_check_now._M_i = 1', completion_thread)
            event = mi.stop()
            if event_field(event, 'bkptno') != checked:
                raise RuntimeError('Young result consumer not reached')
            observer = event_field(event, 'thread-id')
            marked = mi.number('native1286_marked', observer)
            removed = mi.number('native1286_removed', observer)
            private = mi.number('native1286_private', observer)
            before = mi.number('native1286_before', observer)
            passed = marked == 1 and removed == 1 and private == 1 and before == 0
            emit('NATIVE_YOUNG_RESULT', executed=1, passed=passed, marked=marked, removed=removed, private=private, before=before)
            if not passed:
                return 1
            mi.delete(checked)
            mi.resume(observer)
            mi.resume(completion_thread)
            event = mi.stop()
        if 'reason="exited-normally"' not in event and 'exit-code="0"' not in event:
            raise RuntimeError('Fixture did not complete: ' + event)
        emit('NATIVE_FIXTURE_EXIT', rc=0)
        return 0
    except Exception:
        try:
            mi.cmd('-exec-interrupt --all')
            mi.stop()
            mi.console('thread apply all bt 8')
        except Exception:
            pass
        raise
    finally:
        try:
            mi.console('kill')
        except Exception:
            pass
        mi.process.terminate()
        mi.process.wait(timeout=10)
        mi.log.close()


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as error:
        emit('NATIVE_HARNESS_ERROR', error=str(error))
        raise SystemExit(2)
