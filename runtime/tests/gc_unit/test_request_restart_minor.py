# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Park only the minor driver before its lock; observe old completion's request.

Non-stop GDB schedules threads, but never writes product memory or calls a
product function. The existing allocation fixture supplies all requests.
Usage: python3 this-file ELF RUNTIME_SOURCE_ROOT, with GCV2_RUNTIME_LIB_DIR.
"""
import json
import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time

elf, source = sys.argv[1:]
program = r'''
import gdb, json, os
from pathlib import Path
class HoldMinor(gdb.Breakpoint):
    def stop(self):
        if gdb.selected_thread().name != 'ZDriverMinor':
            return False
        self.enabled = False
        print('REQUEST_MINOR_PARKED', flush=True)
        return True
class Restart(gdb.Breakpoint):
    def stop(self):
        stack = gdb.execute('bt', to_string=True)
        if 'RegionManager::RestartGC' not in stack:
            return False
        try:
            actual = Path(gdb.solib_name(gdb.newest_frame().pc())).resolve()
            expected = (Path(os.environ['GCV2_RUNTIME_LIB_DIR']) / 'libcangjie-runtime.so').resolve()
            if actual != expected:
                raise RuntimeError('wrong product SO')
            request = gdb.parse_and_eval('request')
            actual = tuple(int(request[x]) for x in ('_cause','_young_nworkers','_old_nworkers'))
            wanted = (5, int(gdb.parse_and_eval('MapleRuntime::ZYoungGCThreads')), 0)
            print('REQUEST_RESTART_MINOR_TARGET ' + json.dumps(dict(actual=actual, expected=wanted, origin=stack)), flush=True)
            print('REQUEST_RESTART_MINOR_PASS' if actual == wanted else 'REQUEST_RESTART_MINOR_FAIL', flush=True)
        except Exception as error:
            print('REQUEST_RESTART_MINOR_ERROR ' + repr(error), flush=True)
        self.enabled = False
        return True
HoldMinor('MapleRuntime::ZDriver::lock')
Restart('MapleRuntime::ZDriverMinor::collect')
'''
fixture = 'RequestWorkers.StallAfterYoungPrelude'
env = os.environ.copy()
env.update(GC_UNIT_FILTER=fixture, GC_UNIT_OTHER_VM_CHILD=fixture)
with tempfile.NamedTemporaryFile(mode='w', suffix='.gdb', dir=os.getcwd()) as commands:
    commands.write('set pagination off\nset confirm off\nset breakpoint pending on\nset non-stop on\n'
                   'set print thread-events off\npython\n' + program + '\nend\n')
    commands.flush()
    process = subprocess.Popen(['gdb', '-q', '--interpreter=mi2', '-x', commands.name, '--args', elf],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, bufsize=0)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    process.stdin.write(b'-exec-run\n')
    deadline = time.monotonic() + 60
    buffer = b''
    passed = False
    failed = False
    parked = False
    result = 2
    try:
        while time.monotonic() < deadline:
            for key, _ in selector.select(timeout=1):
                chunk = os.read(key.fd, 65536)
                if not chunk:
                    raise RuntimeError('gdb exited before fixture result')
                buffer += chunk
                while b'\n' in buffer:
                    raw, buffer = buffer.split(b'\n', 1)
                    line = raw.decode(errors='replace')
                    if line.startswith(('~"', '@"', '&"')):
                        try:
                            text = json.loads(line[1:])
                        except ValueError:
                            text = line
                        print(text, end='' if text.endswith('\n') else '\n', flush=True)
                        parked |= 'REQUEST_MINOR_PARKED' in text
                        passed |= 'REQUEST_RESTART_MINOR_PASS' in text
                        failed |= 'REQUEST_RESTART_MINOR_FAIL' in text or 'REQUEST_RESTART_MINOR_ERROR' in text
                    elif line.startswith('*stopped'):
                        if failed:
                            result = 1
                            raise StopIteration
                        if 'exited-normally' in line:
                            result = 0 if passed and parked else 2
                            raise StopIteration
                        if 'reason="exited"' in line or 'signal-received' in line:
                            print(line, flush=True)
                            result = 1
                            raise StopIteration
                        if passed:
                            process.stdin.write(b'-exec-continue --all\n')
                        elif not parked:
                            raise RuntimeError('unexpected breakpoint stop: ' + line)
                    elif not line.startswith(('=', '^', '*', '(gdb)')):
                        print(line, flush=True)
            if process.poll() is not None:
                raise RuntimeError('gdb terminated')
        else:
            raise RuntimeError('timed out awaiting product minor restart')
    except StopIteration:
        pass
    except Exception as error:
        print('REQUEST_RESTART_HARNESS_ERROR', repr(error), flush=True)
    finally:
        if process.poll() is None:
            process.stdin.write(b'-gdb-exit\n')
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        selector.close()
    print('REQUEST_RESTART_RESULT', result, 'parked=', parked, 'observed=', passed, flush=True)
    sys.exit(result)
