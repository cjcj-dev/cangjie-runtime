"""Construct real held-exit and joined/unreaped worker states on native Linux."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import struct
import subprocess
import sys
from check_other_vm_teardown import runtime_workers, teardown_before_sentinel

TRACEME, PEEKTEXT, POKETEXT, CONT, SYSCALL = 0, 1, 4, 7, 24
SETOPTIONS, GETEVENTMSG, GETSIGINFO = 0x4200, 0x4201, 0x4202
GETREGSET, SETREGSET, GET_SYSCALL_INFO = 0x4204, 0x4205, 0x420e
WALL = 0x40000000
libc = ctypes.CDLL(None, use_errno=True)
libc.ptrace.restype = ctypes.c_long
libc.ptrace.argtypes = [ctypes.c_uint, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p]


def emit(kind, **values):
    print(kind + ' ' + json.dumps(values, sort_keys=True), flush=True)


class TraceError(OSError):
    pass


def trace(request, tid, address=0, data=0):
    ctypes.set_errno(0)
    result = libc.ptrace(request, tid, ctypes.c_void_p(address), ctypes.c_void_p(data))
    error = ctypes.get_errno()
    if result == -1 and error:
        emit('PTRACE_ERROR', request=request, tid=tid, errno=error)
        raise TraceError(error, f'request={request:#x} tid={tid}')
    return result


class X64Registers(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint64) for name in
                'r15 r14 r13 r12 rbp rbx r11 r10 r9 r8 rax rcx rdx rsi rdi orig_rax rip cs eflags rsp ss fs_base gs_base ds es fs gs'.split()]


class A64Registers(ctypes.Structure):
    _fields_ = [('regs', ctypes.c_uint64 * 31), ('sp', ctypes.c_uint64),
                ('pc', ctypes.c_uint64), ('pstate', ctypes.c_uint64)]


class Iovec(ctypes.Structure):
    _fields_ = [('base', ctypes.c_void_p), ('length', ctypes.c_size_t)]


class ABI:
    def __init__(self, machine):
        self.arm = machine == 'aarch64'
        self.register_type = A64Registers if self.arm else X64Registers
        self.audit_arch = 0xc00000b7 if self.arm else 0xc000003e
        self.futex = 98 if self.arm else 202
        self.pc_name = 'pc' if self.arm else 'rip'
        self.elf_machine = 183 if self.arm else 62

    def regset(self, tid, values=None):
        writing = values is not None
        values = values if writing else self.register_type()
        iov = Iovec(ctypes.addressof(values), ctypes.sizeof(values))
        trace(SETREGSET if writing else GETREGSET, tid, 1, ctypes.addressof(iov))
        raw = bytes(values)
        emit('REGSET', tid=tid, writing=writing, length=iov.length, expected=ctypes.sizeof(values), raw=raw.hex())
        if iov.length != ctypes.sizeof(values):
            raise RuntimeError('incomplete NT_PRSTATUS regset')
        return values

    def entry(self, tid):
        # UAPI header is 24 bytes; entry is nr + six u64 arguments (80 total).
        buf = ctypes.create_string_buffer(88)
        length = trace(GET_SYSCALL_INFO, tid, ctypes.sizeof(buf), ctypes.addressof(buf))
        raw = buf.raw
        op, arch = raw[0], struct.unpack_from('<I', raw, 4)[0]
        if length < 24 or length > len(raw) or arch != self.audit_arch:
            raise RuntimeError(f'unsupported syscall-info length={length} arch={arch:#x}')
        if op == 2:
            if length < 33:
                raise RuntimeError('short syscall exit info')
            return None
        if op != 1 or length < 80:
            raise RuntimeError(f'unsupported syscall-info op={op} length={length}')
        nr, *args = struct.unpack_from('<7Q', raw, 24)
        if nr == self.futex:
            emit('SYSCALL_ENTRY', tid=tid, length=length, arch=arch, nr=nr, args=args, raw=raw[:length].hex())
        return nr, args


def completion_address(elf, pid, abi):
    data = Path(elf).read_bytes()
    if data[:6] != b'\x7fELF\x02\x01':
        raise RuntimeError('expected little-endian ELF64')
    kind, machine = struct.unpack_from('<HH', data, 16)
    if kind not in (2, 3) or machine != abi.elf_machine:
        raise RuntimeError('ELF type/architecture mismatch')
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phsize, phnum = struct.unpack_from('<HH', data, 54)
    if phsize != 56 or phoff + phnum * phsize > len(data):
        raise RuntimeError('invalid ELF program headers')
    loads = [struct.unpack_from('<II6Q', data, phoff + i * phsize)
             for i in range(phnum) if struct.unpack_from('<I', data, phoff + i * phsize)[0] == 1]
    symbols = [line.split() for line in subprocess.check_output(
        ['nm', '--defined-only', '-C', elf], text=True).splitlines()
        if line.split(maxsplit=2)[-1] == 'MapleRuntime::GcUnit::CompleteTestRun(int)']
    if len(symbols) != 1:
        raise RuntimeError(f'completion symbol count={len(symbols)}')
    value = int(symbols[0][0], 16)
    page = os.sysconf('SC_PAGE_SIZE')
    maps = Path(f'/proc/{pid}/maps').read_text()
    emit('PRODUCT_MAPS', pid=pid, maps=maps)
    biases = set()
    elf_stat = Path(elf).stat()
    for line in maps.splitlines():
        fields = line.split(maxsplit=5)
        if len(fields) != 6 or fields[5] != elf or int(fields[4]) != elf_stat.st_ino:
            continue
        major, minor = (int(x, 16) for x in fields[3].split(':'))
        if os.makedev(major, minor) != elf_stat.st_dev:
            raise RuntimeError('ELF maps device mismatch')
        start = int(fields[0].split('-')[0], 16)
        offset = int(fields[2], 16)
        for _, flags, fileoff, vaddr, _, filesz, _, _ in loads:
            if offset == fileoff // page * page and ('x' in fields[1]) == bool(flags & 1):
                biases.add(start - vaddr // page * page)
    if len(biases) != 1:
        raise RuntimeError(f'ambiguous PT_LOAD/maps bias {biases}')
    bias = biases.pop()
    if kind == 2 and bias != 0:
        raise RuntimeError('ET_EXEC has nonzero bias')
    if not any(flags & 1 and vaddr <= value < vaddr + filesz
               for _, flags, _, vaddr, _, filesz, _, _ in loads):
        raise RuntimeError('completion symbol outside executable PT_LOAD')
    emit('ELF_BINDING', type=kind, bias=bias, symbol=value, address=bias+value)
    return bias + value


def task_record(pid, tid):
    base = Path(f'/proc/{pid}/task/{tid}')
    stat = base.joinpath('stat').read_text()
    name = stat[stat.index('(')+1:stat.rindex(')')]
    state = stat.rsplit(')', 1)[1].split()[0]
    tgid = int(next(line.split()[1] for line in base.joinpath('status').read_text().splitlines()
                    if line.startswith('Tgid:')))
    if tgid != pid:
        raise RuntimeError('worker TGID mismatch')
    emit('TASK_STATE', pid=pid, tid=tid, name=name, state=state, tgid=tgid)
    return name, state


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('elf')
    parser.add_argument('libdir')
    parser.add_argument('--live', action='store_true')
    args = parser.parse_args()
    machine = platform.machine()
    if sys.platform != 'linux' or machine not in ('x86_64', 'aarch64') or sys.byteorder != 'little' or ctypes.sizeof(ctypes.c_void_p) != 8:
        print('TEARDOWN_CONSTRUCTION_NOT_RUN unsupported ptrace architecture', flush=True)
        return 77
    abi = ABI(machine)
    elf, libdir = str(Path(args.elf).resolve()), str(Path(args.libdir).resolve())
    emit('CONSTRUCTION_DOMAIN', machine=machine, kernel=platform.release(), glibc=os.confstr('CS_GNU_LIBC_VERSION'), live=args.live)
    for artifact in (elf, libdir + '/libcangjie-runtime.so', libdir + '/libboundscheck.so'):
        print('TEARDOWN_IDENTITY sha256=%s path=%s' % (hashlib.sha256(Path(artifact).read_bytes()).hexdigest(), artifact), flush=True)
    pid = os.fork()
    if pid == 0:
        try:
            trace(TRACEME, 0)
        except TraceError as error:
            print(f'TEARDOWN_CONSTRUCTION_NOT_RUN stage=TRACEME errno={error.errno}', flush=True)
            os._exit(77)
        os.environ.update(LD_LIBRARY_PATH=libdir, GC_UNIT_OTHER_VM_CHILD='RuntimeWorkers.ActivePoolBeforeHarnessShutdown')
        os.execv(elf, [elf, '--gtest_filter=RuntimeWorkers.ActivePoolBeforeHarnessShutdown'])
    owned = {pid}
    held, joined = None, False
    workers, exits, reaped = set(), set(), set()
    stage = 'exec-stop'
    def wait(tid=-1):
        who, status = os.waitpid(tid, WALL)
        emit('WAIT_EVENT', tid=who, status=status, event=status >> 16)
        if who not in owned:
            raise RuntimeError(f'unowned wait tid={who}')
        if os.WIFEXITED(status) or os.WIFSIGNALED(status):
            reaped.add(who)
        return who, status
    try:
        _, status = wait(pid)
        if os.WIFEXITED(status) and os.WEXITSTATUS(status) == 77:
            return 77
        if not os.WIFSTOPPED(status) or os.WSTOPSIG(status) != signal.SIGTRAP:
            raise RuntimeError(f'product did not reach exec stop status={status:x}')
        trace(SETOPTIONS, pid, data=1 | 8 | 64 | 16 | 0x100000)
        stage = 'install-breakpoint'
        address = completion_address(elf, pid, abi)
        word_address = address & ~7
        shift = (address - word_address) * 8
        if abi.arm and address % 4:
            raise RuntimeError('unaligned A64 completion')
        mask = (0xffffffff if abi.arm else 0xff) << shift
        opcode = (0xd4200000 if abi.arm else 0xcc) << shift
        original = trace(PEEKTEXT, pid, word_address) & ((1 << 64)-1)
        patched = (original & ~mask) | opcode
        trace(POKETEXT, pid, word_address, patched)
        readback = trace(PEEKTEXT, pid, word_address) & ((1 << 64)-1)
        emit('BREAKPOINT_TEXT', address=address, word_address=word_address, original=original, patched=patched, readback=readback, shift=shift)
        if readback != patched:
            raise RuntimeError('breakpoint text write mismatch')
        stage = 'hold-and-join'
        trace(SYSCALL, pid)
        while True:
            tid, status = wait()
            if os.WIFEXITED(status) or os.WIFSIGNALED(status):
                if tid == pid:
                    raise RuntimeError('product exited before construction')
                continue
            if not os.WIFSTOPPED(status):
                raise RuntimeError('unexpected wait state')
            sig, event = os.WSTOPSIG(status), status >> 16
            if event == 3:
                child = ctypes.c_ulong()
                trace(GETEVENTMSG, tid, data=ctypes.addressof(child))
                owned.add(child.value)
                emit('CLONE', parent=tid, child=child.value)
                trace(SYSCALL if tid == pid else CONT, tid)
            elif event == 6:
                name, _ = task_record(pid, tid)
                if name.startswith('RuntimeWorker#'):
                    for path in Path(f'/proc/{pid}/task').glob('*/comm'):
                        if path.read_text().startswith('RuntimeWorker#'):
                            workers.add(int(path.parent.name))
                    workers.add(tid)
                    exits.add(tid)
                    if name == 'RuntimeWorker#0':
                        if held is not None:
                            raise RuntimeError('duplicate held worker')
                        held = tid
                        print(f'CONSTRUCT_HOLD_EXIT tid={tid} name={name}', flush=True)
                    else:
                        trace(CONT, tid)
                else:
                    trace(CONT, tid)
            elif tid == pid and sig == signal.SIGTRAP | 128:
                entry = abi.entry(tid)
                if entry and entry[0] == abi.futex:
                    params = entry[1]
                    expected = params[2]
                    target = Path(f'/proc/{pid}/task/{expected}/comm')
                    if expected and target.exists() and target.read_text().strip() == 'RuntimeWorker#0':
                        # glibc 2.39 pthread_join_common -> shared WAIT_BITSET,
                        # optional CLOCK_REALTIME; no timeout, MATCH_ANY bitset.
                        if params[1] not in (9, 9 | 256) or params[3:6] != [0, 0, 0xffffffff]:
                            raise RuntimeError(f'unsupported actual glibc join args={params}')
                        uaddr = params[0]
                        memory = trace(PEEKTEXT, pid, uaddr & ~7) & ((1 << 64)-1)
                        actual = (memory >> ((uaddr & 7)*8)) & 0xffffffff
                        if uaddr % 4 or actual != expected:
                            raise RuntimeError('join futex value mismatch')
                        joined = True
                        join_tid = expected
                        emit('JOIN_ABI', tid=expected, uaddr=uaddr, value=actual, args=params, syscall=entry[0])
                        print(f'CONSTRUCT_JOIN_WAIT tid={expected} syscall={entry[0]}', flush=True)
                    else:
                        trace(SYSCALL, tid)
                else:
                    trace(SYSCALL, tid)
            else:
                trace(SYSCALL if tid == pid else CONT, tid, data=0 if sig in (signal.SIGSTOP, signal.SIGTRAP) else sig)
            if held is not None and joined and workers and workers <= exits:
                if join_tid != held or held not in workers or not workers <= owned:
                    raise RuntimeError('worker/join identity mismatch')
                emit('WORKER_SET', workers=sorted(workers), exit_events=sorted(exits), held=held, tgid=pid)
                break
        name, state = task_record(pid, held)
        if name != 'RuntimeWorker#0' or state in ('Z', 'X'):
            raise RuntimeError('held live state not constructed')
        sample_passed = False
        if args.live:
            sample = runtime_workers(pid)
            if name not in sample:
                raise RuntimeError('live sample missing held worker')
            sample_passed = teardown_before_sentinel([sample])
            print(f'TEARDOWN_CONSTRUCT_PRE_EXIT tid={held} accepted={sample_passed}', flush=True)
        stage = 'completion-breakpoint'
        trace(CONT, pid)
        trace(CONT, held)
        waited, status = wait(pid)
        if not os.WIFSTOPPED(status) or os.WSTOPSIG(status) != signal.SIGTRAP or status >> 16:
            raise RuntimeError(f'completion breakpoint not reached status={status:x}')
        values = abi.regset(pid)
        pc = getattr(values, abi.pc_name)
        siginfo = ctypes.create_string_buffer(128)
        trace(GETSIGINFO, pid, data=ctypes.addressof(siginfo))
        code = struct.unpack_from('<i', siginfo.raw, 8)[0]
        emit('BREAKPOINT_STOP', pc=pc, expected=address if abi.arm else address+1, si_code=code, siginfo=siginfo.raw.hex())
        if pc != address + (0 if abi.arm else 1) or code not in ((1,) if abi.arm else (1, 128)):
            raise RuntimeError('wrong breakpoint PC/si_code')
        name, state = task_record(pid, held)
        print(f'COMPLETE_AFTER_JOIN held_tid={held} comm={name} state={state}', flush=True)
        if name != 'RuntimeWorker#0' or state != 'Z':
            raise RuntimeError('deterministic stale task not constructed')
        if not args.live:
            sample_passed = teardown_before_sentinel([runtime_workers(pid)])
        _, held_status = wait(held)
        if not os.WIFEXITED(held_status) or os.WEXITSTATUS(held_status) != 0:
            raise RuntimeError('held worker reap failed')
        task = Path(f'/proc/{pid}/task/{held}')
        print(f'AFTER_TRACER_REAP task_exists={task.exists()}', flush=True)
        stage = 'restore'
        trace(POKETEXT, pid, word_address, original)
        restored = trace(PEEKTEXT, pid, word_address) & ((1 << 64)-1)
        if restored != original:
            raise RuntimeError('text restoration mismatch')
        setattr(values, abi.pc_name, address)
        abi.regset(pid, values)
        check = abi.regset(pid)
        if bytes(check) != bytes(values):
            raise RuntimeError('register restoration mismatch')
        emit('BREAKPOINT_RESTORED', word=restored, original=original, pc=address)
        trace(CONT, pid)
        stage = 'normal-exit'
        while True:
            tid, status = wait()
            if os.WIFEXITED(status):
                if tid != pid:
                    continue
                if os.WEXITSTATUS(status) != 0:
                    raise RuntimeError(f'product exit rc={os.WEXITSTATUS(status)}')
                print('TEARDOWN_CONSTRUCT_EXECUTED product_rc=0', flush=True)
                break
            if os.WIFSIGNALED(status):
                raise RuntimeError(f'product signal={os.WTERMSIG(status)}')
            trace(CONT, tid)
        return 0 if sample_passed else 1
    except TraceError as error:
        print(f'TEARDOWN_CONSTRUCTION_NOT_RUN stage={stage} errno={error.errno} {error}', flush=True)
        return 77
    finally:
        # Only this fork and clone-event registered tracees belong to us.
        if pid not in reaped:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        while owned - reaped:
            try:
                tid, status = wait()
                if os.WIFSTOPPED(status):
                    trace(CONT, tid, data=signal.SIGKILL)
            except ChildProcessError:
                break
        emit('CONSTRUCTION_CLEANUP', owned=sorted(owned), reaped=sorted(reaped), stage=stage)


if __name__ == '__main__':
    sys.exit(main())
