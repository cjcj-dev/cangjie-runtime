import argparse
import ctypes
import hashlib
import os
from pathlib import Path
import signal
import subprocess
import sys
from check_other_vm_teardown import runtime_workers, teardown_before_sentinel

libc = ctypes.CDLL(None, use_errno=True)
libc.ptrace.restype = ctypes.c_long


def trace(request, tid, address=0, data=0):
    ctypes.set_errno(0)
    result = libc.ptrace(request, tid, ctypes.c_void_p(address), ctypes.c_void_p(data))
    if result == -1 and ctypes.get_errno():
        raise OSError(ctypes.get_errno(), f'ptrace {request} tid={tid}')
    return result


class Registers(ctypes.Structure):
    _fields_ = [(name, ctypes.c_ulonglong) for name in
                'r15 r14 r13 r12 rbp rbx r11 r10 r9 r8 rax rcx rdx rsi rdi orig_rax rip cs eflags rsp ss fs_base gs_base ds es fs gs'.split()]


def registers(tid):
    values = Registers()
    trace(12, tid, data=ctypes.addressof(values))
    return values


parser = argparse.ArgumentParser()
parser.add_argument('elf')
parser.add_argument('libdir')
parser.add_argument('--live', action='store_true')
args = parser.parse_args()
if sys.platform != 'linux' or os.uname().machine != 'x86_64':
    print('TEARDOWN_CONSTRUCTION_NOT_RUN unsupported ptrace architecture', flush=True)
    sys.exit(77)
elf = str(Path(args.elf).resolve())
libdir = str(Path(args.libdir).resolve())
for artifact in (elf, libdir + '/libcangjie-runtime.so', libdir + '/libboundscheck.so'):
    print('TEARDOWN_IDENTITY sha256=%s path=%s' %
          (hashlib.sha256(Path(artifact).read_bytes()).hexdigest(), artifact), flush=True)
symbol = next(line.split()[0] for line in subprocess.check_output(['nm', '--defined-only', '-C', elf], text=True).splitlines()
              if 'MapleRuntime::GcUnit::CompleteTestRun(int)' in line)
pid = os.fork()
if pid == 0:
    try:
        trace(0, 0)
    except OSError as error:
        print(f'TEARDOWN_CONSTRUCTION_NOT_RUN ptrace unavailable: {error}', flush=True)
        os._exit(77)
    os.environ.update(LD_LIBRARY_PATH=libdir,
                     GC_UNIT_OTHER_VM_CHILD='RuntimeWorkers.ActivePoolBeforeHarnessShutdown')
    os.execv(elf, [elf, '--gtest_filter=RuntimeWorkers.ActivePoolBeforeHarnessShutdown'])

held = None
join_stop = False
sample_passed = False
runtime_exits = set()
runtime_tids = set()
try:
    _, initial_status = os.waitpid(pid, 0)
    if os.WIFEXITED(initial_status) and os.WEXITSTATUS(initial_status) == 77:
        sys.exit(77)
    if not os.WIFSTOPPED(initial_status):
        raise RuntimeError(f'product did not reach exec stop status={initial_status:x}')
    trace(0x4200, pid, data=1 | 8 | 64 | 16 | 0x100000)
    mapping = next(line for line in Path(f'/proc/{pid}/maps').read_text().splitlines()
                   if line.endswith(elf) and line.split()[2] == '00000000')
    base = int(mapping.split('-')[0], 16)
    address = base + int(symbol, 16)
    original = trace(1, pid, address) & ((1 << 64) - 1)
    trace(4, pid, address, (original & ~255) | 0xcc)
    trace(24, pid)
    while True:
        tid, status = os.waitpid(-1, 0x40000000)
        if os.WIFEXITED(status) or os.WIFSIGNALED(status):
            continue
        stopped_signal = os.WSTOPSIG(status)
        event = status >> 16
        if event == 3:
            trace(24 if tid == pid else 7, tid)
            continue
        if event == 6:
            comm = Path(f'/proc/{pid}/task/{tid}/comm').read_text().strip()
            if comm.startswith('RuntimeWorker#'):
                runtime_tids.update(int(path.parent.name) for path in Path(f'/proc/{pid}/task').glob('*/comm')
                                    if path.read_text().startswith('RuntimeWorker#'))
                runtime_exits.add(tid)
                if comm == 'RuntimeWorker#0':
                    held = tid
                    print(f'CONSTRUCT_HOLD_EXIT tid={tid} name={comm}', flush=True)
                else:
                    trace(7, tid)
            else:
                trace(7, tid)
        elif tid == pid and stopped_signal == signal.SIGTRAP | 128:
            values = registers(tid)
            target = Path(f'/proc/{pid}/task/{values.rdx}/comm')
            if values.orig_rax == 202 and target.exists() and target.read_text().strip() == 'RuntimeWorker#0':
                join_stop = True
                print(f'CONSTRUCT_JOIN_WAIT tid={values.rdx} syscall={values.orig_rax}', flush=True)
            else:
                trace(24, tid)
        else:
            trace(24 if tid == pid else 7, tid, data=0 if stopped_signal in (signal.SIGSTOP, signal.SIGTRAP) else stopped_signal)
        if held is not None and join_stop and runtime_tids <= runtime_exits:
            break
    if args.live:
        sample_passed = teardown_before_sentinel([runtime_workers(pid)])
        print(f'TEARDOWN_CONSTRUCT_PRE_EXIT tid={held} accepted={sample_passed}', flush=True)
    trace(7, pid)
    trace(7, held)
    waited, status = os.waitpid(pid, 0x40000000)
    values = registers(pid)
    if waited != pid or not os.WIFSTOPPED(status) or values.rip != address + 1:
        raise RuntimeError(f'completion breakpoint not reached status={status:x} rip={values.rip:x}')
    task = Path(f'/proc/{pid}/task/{held}')
    comm = task.joinpath('comm').read_text().strip()
    state = task.joinpath('stat').read_text().rsplit(')', 1)[1].split()[0]
    print(f'COMPLETE_AFTER_JOIN held_tid={held} comm={comm} state={state}', flush=True)
    if not comm.startswith('RuntimeWorker#') or state != 'Z':
        raise RuntimeError('deterministic stale task not constructed')
    if not args.live:
        sample_passed = teardown_before_sentinel([runtime_workers(pid)])
    os.waitpid(held, 0x40000000)
    print(f'AFTER_TRACER_REAP task_exists={task.exists()}', flush=True)
    trace(4, pid, address, original)
    values.rip = address
    trace(13, pid, data=ctypes.addressof(values))
    trace(7, pid)
    while True:
        tid, status = os.waitpid(-1, 0x40000000)
        if os.WIFEXITED(status):
            if tid != pid:
                continue
            if os.WEXITSTATUS(status) != 0:
                raise RuntimeError(f'product exit rc={os.WEXITSTATUS(status)}')
            print('TEARDOWN_CONSTRUCT_EXECUTED product_rc=0', flush=True)
            break
        if os.WIFSIGNALED(status):
            raise RuntimeError(f'product signal={os.WTERMSIG(status)}')
        trace(7, tid)
    sys.exit(0 if sample_passed else 1)
except OSError as error:
    if error.errno in (1, 13, 38):
        print(f'TEARDOWN_CONSTRUCTION_NOT_RUN ptrace unavailable: {error}', flush=True)
        sys.exit(77)
    raise
finally:
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    while True:
        try:
            tid, status = os.waitpid(-1, 0x40000000)
            if os.WIFSTOPPED(status):
                trace(7, tid, data=signal.SIGKILL)
        except ChildProcessError:
            break
