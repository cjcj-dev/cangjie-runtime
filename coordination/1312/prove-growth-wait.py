#!/usr/bin/env python3
"""Hold the real processor before its first table operation (no table lock).

Non-stop GDB freezes only that thread. The old helper returns and latches 503;
the new helper reaches its yield with the same 503/good state. Release both
threads after recording that decision. No inferior state is written.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--mode', choices=('old', 'new'), required=True)
p.add_argument('--elf', required=True)
p.add_argument('--source-text', required=True)
p.add_argument('--lib', required=True)
p.add_argument('--out', required=True)
a = p.parse_args()
out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
lines = Path(a.source_text).read_text().splitlines()
if a.mode == 'old':
    decision_line = next(i for i, line in enumerate(lines, 1) if 'const unsigned grownState =' in line)
else:
    start = next(i for i, line in enumerate(lines) if 'bool WaitDedupSize(' in line)
    decision_line = next(i + 1 for i in range(start, len(lines)) if 'std::this_thread::yield();' in lines[i])
env = os.environ.copy(); env['LD_LIBRARY_PATH'] = a.lib
proc = subprocess.Popen(['gdb', '-nx', '-q', '--interpreter=mi2', '--args', a.elf],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, bufsize=1, env=env)
transcript = (out / 'gdb-mi.log').open('w')
serial = 0
stops = []
result = {'mode': a.mode, 'elf_sha256': hashlib.sha256(Path(a.elf).read_bytes()).hexdigest(),
          'so_sha256': hashlib.sha256((Path(a.lib)/'libcangjie-runtime.so').read_bytes()).hexdigest(),
          'source_sha256': hashlib.sha256(Path(a.source_text).read_bytes()).hexdigest(),
          'decision_line': decision_line, 'inferior_rc': None, 'qualified': False}


def read():
    line = proc.stdout.readline()
    if not line:
        raise RuntimeError('GDB closed before completing the construction')
    transcript.write(line); transcript.flush()
    if line.startswith('*stopped,'):
        stops.append(line.strip())
    return line.strip()


def command(text):
    global serial
    serial += 1
    token = str(serial)
    proc.stdin.write(token + text + '\n'); proc.stdin.flush()
    while True:
        line = read()
        if line.startswith(token + '^error'):
            raise RuntimeError(line)
        if line.startswith(token + '^'):
            return line


def stop():
    while not stops:
        read()
    return stops.pop(0)


def field(text, name):
    m = re.search(r'(?:^|[,{])' + re.escape(name) + r'=("(?:[^"\\]|\\.)*")', text)
    return json.loads(m.group(1)) if m else None


def value(expression):
    answer = command('-data-evaluate-expression ' + json.dumps(expression))
    return int(field(answer, 'value'), 0)


try:
    command('-gdb-set pagination off')
    command('-gdb-set confirm off')
    command('-gdb-set non-stop on')
    command('-gdb-set mi-async on')
    command('-interpreter-exec console "handle SIGSEGV nostop noprint pass"')
    command('-gdb-set environment GC_UNIT_OTHER_VM_CHILD=StringDedup.ResizeThenOldCallbacksShrink')
    command('-exec-arguments --gtest_filter=StringDedup.ResizeThenOldCallbacksShrink')
    held_bp = field(command('-break-insert -f "MapleRuntime::StringDedup::Processor::run_thread()"'), 'number')
    decision_bp = field(command('-break-insert -f "test_string_dedup.cpp:' + str(decision_line) + '"'), 'number')
    command('-exec-run')
    threads = {}
    while len(threads) < 2:
        event = stop()
        bp = field(event, 'bkptno')
        if field(event, 'reason') != 'breakpoint-hit':
            raise RuntimeError('unexpected stop: ' + event)
        if bp == held_bp:
            threads['processor'] = field(event, 'thread-id')
        elif bp == decision_bp:
            threads['decision'] = field(event, 'thread-id')
        else:
            raise RuntimeError('unexpected breakpoint: ' + event)
    command('-thread-select ' + threads['processor'])
    owner = value('(unsigned long)&this->owner')
    table = '((MapleRuntime::StringDedup*)%d)->table' % owner
    result['buckets_before_release'] = value(table + '.numberOfBuckets')
    result['entries_before_release'] = value(table + '.numberOfEntries')
    result['state_before_release'] = value('(int)' + table + '.deadState._M_i')
    result['threads'] = threads
    command('-thread-select ' + threads['decision'])
    if a.mode == 'old':
        result['latched_buckets'] = value('grownBuckets')
        result['helper_returned'] = bool(value('(int)grown'))
    else:
        result['require_growth'] = bool(value('(int)requireGrowthComplete'))
        result['waiting_count'] = value('count')
        result['helper_waiting'] = True
    command('-break-disable ' + held_bp + ' ' + decision_bp)
    command('-exec-continue --thread ' + threads['processor'])
    command('-exec-continue --thread ' + threads['decision'])
    event = stop()
    reason = field(event, 'reason')
    if reason == 'exited-normally':
        result['inferior_rc'] = 0
    elif reason == 'exited':
        result['inferior_rc'] = int(field(event, 'exit-code'), 8)
    else:
        raise RuntimeError('unexpected final stop: ' + event)
    common = result['buckets_before_release'] == 503 and result['entries_before_release'] == 7200 and \
        result['state_before_release'] == 0
    if a.mode == 'old':
        result['qualified'] = common and result['helper_returned'] and result['latched_buckets'] == 503 and result['inferior_rc'] == 1
    else:
        result['qualified'] = common and result['require_growth'] and result['waiting_count'] == 7200 and result['inferior_rc'] == 0
except Exception as exc:
    result['error'] = str(exc)
finally:
    (out/'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, sort_keys=True))
    if proc.poll() is None:
        proc.stdin.write('-gdb-exit\n'); proc.stdin.flush()
        proc.wait(timeout=10)
    transcript.close()
raise SystemExit(0 if result['qualified'] else 1)
