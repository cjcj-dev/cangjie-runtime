"""Construct real held-exit and joined/unreaped worker states on native Linux."""
import argparse
from collections import Counter
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import re
import threading
import struct
import subprocess
import sys
from check_other_vm_teardown import runtime_workers, teardown_before_sentinel

TRACEME, PEEKTEXT, PEEKUSER, POKEUSER, CONT, SYSCALL = 0, 1, 3, 6, 7, 24
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


class X64User(ctypes.Structure):
    # Linux x86_64 UAPI struct user (sys/user.h), not a libc thread layout.
    _fields_ = [('regs', X64Registers), ('fpvalid', ctypes.c_int),
                ('fpregs', ctypes.c_uint64 * 64), ('sizes', ctypes.c_uint64 * 5),
                ('signal', ctypes.c_uint64), ('reserved', ctypes.c_int),
                ('ar0', ctypes.c_uint64), ('fpstate', ctypes.c_uint64),
                ('magic', ctypes.c_uint64), ('comm', ctypes.c_char * 32),
                ('debugreg', ctypes.c_uint64 * 8)]


class A64DebugRegister(ctypes.Structure):
    _fields_ = [('address', ctypes.c_uint64), ('control', ctypes.c_uint32),
                ('pad', ctypes.c_uint32)]


class A64DebugState(ctypes.Structure):
    _fields_ = [('info', ctypes.c_uint32), ('pad', ctypes.c_uint32),
                ('registers', A64DebugRegister * 16)]


def read_memory(tid, address, size):
    if not address or size <= 0:
        raise RuntimeError('invalid observed memory range')
    start, end = address & ~7, (address + size + 7) & ~7
    data = b''.join((trace(PEEKTEXT, tid, word) & ((1 << 64)-1)).to_bytes(8, 'little')
                    for word in range(start, end, 8))
    return data[address-start:address-start+size]


def read_integer(tid, address, size=4):
    return int.from_bytes(read_memory(tid, address, size), 'little')


class ABI:
    def __init__(self, machine):
        self.arm = machine == 'aarch64'
        self.register_type = A64Registers if self.arm else X64Registers
        self.audit_arch = 0xc00000b7 if self.arm else 0xc000003e
        self.futex = 98 if self.arm else 202
        self.pc_name = 'pc' if self.arm else 'rip'
        self.elf_machine = 183 if self.arm else 62
        self.clone = 220 if self.arm else 56

    def regset(self, tid):
        values = self.register_type()
        iov = Iovec(ctypes.addressof(values), ctypes.sizeof(values))
        trace(GETREGSET, tid, 1, ctypes.addressof(iov))
        raw = bytes(values)
        emit('REGSET', tid=tid, writing=False, length=iov.length, expected=ctypes.sizeof(values), raw=raw.hex())
        if iov.length != ctypes.sizeof(values):
            raise RuntimeError('incomplete NT_PRSTATUS regset')
        return values

    def argument0(self, values):
        return values.regs[0] if self.arm else values.rdi

    def breakpoints(self, tid, addresses):
        # Execution hardware breakpoints need no inferior text, PC or result
        # writes. They are installed only in our startup-traced main thread.
        if len(addresses) > 4 or len(set(addresses)) != len(addresses):
            raise RuntimeError('invalid teardown breakpoint set')
        if self.arm:
            state = A64DebugState()
            iov = Iovec(ctypes.addressof(state), ctypes.sizeof(state))
            trace(GETREGSET, tid, 0x402, ctypes.addressof(iov))  # NT_ARM_HW_BREAK
            count = state.info & 0xff
            if count < len(addresses) or iov.length < 8 + 16 * count:
                raise RuntimeError('insufficient actual A64 hardware breakpoints')
            for i in range(count):
                state.registers[i].address = addresses[i] if i < len(addresses) else 0
                # Linux arch_hw_breakpoint_ctrl: LEN_4, execute, EL0, enabled.
                state.registers[i].control = (15 << 5) | (2 << 1) | 1 if i < len(addresses) else 0
            iov.length = 8 + 16 * count
            trace(SETREGSET, tid, 0x402, ctypes.addressof(iov))
            observed = A64DebugState()
            check = Iovec(ctypes.addressof(observed), ctypes.sizeof(observed))
            trace(GETREGSET, tid, 0x402, ctypes.addressof(check))
            if bytes(observed.registers)[:16*count] != bytes(state.registers)[:16*count]:
                raise RuntimeError('A64 hardware breakpoint readback mismatch')
        else:
            offset = X64User.debugreg.offset
            trace(POKEUSER, tid, offset + 7 * 8, 0)
            for i in range(4):
                address = addresses[i] if i < len(addresses) else 0
                trace(POKEUSER, tid, offset + i * 8, address)
                if trace(PEEKUSER, tid, offset + i * 8) != address:
                    raise RuntimeError('x64 hardware breakpoint address mismatch')
            control = sum(1 << (2*i) for i in range(len(addresses)))
            trace(POKEUSER, tid, offset + 7 * 8, control)
            if trace(PEEKUSER, tid, offset + 7 * 8) != control:
                raise RuntimeError('x64 hardware breakpoint control mismatch')
        emit('HARDWARE_BREAKPOINTS', tid=tid, addresses=addresses)

    def clone_arguments(self, tid, entry):
        nr, args = entry
        if nr == self.clone:
            flags, _, parent_tid = args[:3]
            child_tid = args[4] if self.arm else args[3]
        elif nr == 435:  # clone3: versioned Linux UAPI, first 64 bytes.
            if args[1] < 64:
                raise RuntimeError('short actual clone3 arguments')
            raw = read_memory(tid, args[0], 64)
            flags, _, child_tid, parent_tid, *_ = struct.unpack('<8Q', raw)
        else:
            return None
        required = 0x100 | 0x10000 | 0x100000 | 0x200000  # VM/THREAD/PARENT_SETTID/CHILD_CLEARTID
        if flags & required != required or not parent_tid or not child_tid or parent_tid % 4 or child_tid % 4:
            raise RuntimeError('unsupported actual pthread clone contract')
        result = dict(syscall=nr, args=args, flags=flags, parent_tid=parent_tid, child_tid=child_tid,
                      clone3_raw=raw.hex() if nr == 435 else None)
        emit('CLONE_ABI', caller=tid, **result)
        return result

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
        if nr in (self.futex, self.clone, 435):
            emit('SYSCALL_ENTRY', tid=tid, length=length, arch=arch, nr=nr, args=args, raw=raw[:length].hex())
        return nr, args


def symbol_address(elf, pid, abi, symbol, dynamic=False):
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
    command = ['nm', '--defined-only', '-C'] + (['-D'] if dynamic else []) + [elf]
    symbols = [line.split(maxsplit=2) for line in subprocess.check_output(command, text=True).splitlines()
               if line.split(maxsplit=2)[-1].split('@')[0] == symbol]
    values = {int(fields[0], 16) for fields in symbols if fields[1] in ('T', 'W', 't')}
    if len(values) != 1:
        raise RuntimeError(f'{symbol} executable symbol addresses={sorted(values)}')
    value = values.pop()
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
        raise RuntimeError('symbol outside executable PT_LOAD')
    emit('ELF_BINDING', path=elf, type=kind, bias=bias, symbol=value, name=symbol,
         aliases=symbols, address=bias+value)
    return bias + value


def pthread_addresses(pid, abi, libdir):
    maps = Path(f'/proc/{pid}/maps').read_text().splitlines()
    paths = {line.split(maxsplit=5)[5] for line in maps if len(line.split(maxsplit=5)) == 6
             and line.split(maxsplit=5)[5].startswith('/')}
    for name in ('libcangjie-runtime.so', 'libboundscheck.so'):
        path = str(Path(libdir, name).resolve())
        if path not in paths:
            raise RuntimeError('actual product library not loaded: ' + path)
        emit('PRODUCT_LOADED', path=path, sha256=hashlib.sha256(Path(path).read_bytes()).hexdigest())
    result = {}
    for symbol in ('pthread_create', 'pthread_join'):
        providers = []
        for path in sorted(paths):
            if not re.fullmatch(r'lib(?:c|pthread)(?:-[\d.]+)?\.so(?:\.\d+)*', Path(path).name):
                continue
            symbols = subprocess.check_output(['nm', '-D', '--defined-only', path], text=True)
            if any(line.split()[-1].split('@')[0] == symbol for line in symbols.splitlines()):
                providers.append((path, symbol_address(path, pid, abi, symbol, dynamic=True)))
        if len(providers) != 1:
            raise RuntimeError(f'ambiguous loaded {symbol} provider: {providers}')
        path, result[symbol] = providers[0]
        emit('PTHREAD_PROVIDER', symbol=symbol, path=path, address=result[symbol],
             sha256=hashlib.sha256(Path(path).read_bytes()).hexdigest())
    return result


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
    if os.environ.get('LD_PRELOAD') or os.environ.get('LD_AUDIT'):
        raise RuntimeError('unsupported interposed pthread provider')
    emit('CONSTRUCTION_DOMAIN', machine=machine, kernel=platform.release(), glibc=os.confstr('CS_GNU_LIBC_VERSION'), live=args.live)
    for artifact in (elf, libdir + '/libcangjie-runtime.so', libdir + '/libboundscheck.so'):
        print('TEARDOWN_IDENTITY sha256=%s path=%s' % (hashlib.sha256(Path(artifact).read_bytes()).hexdigest(), artifact), flush=True)
    output_read, output_write = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(output_read)
        os.dup2(output_write, 1)
        os.dup2(output_write, 2)
        os.close(output_write)
        try:
            trace(TRACEME, 0)
        except TraceError as error:
            print(f'TEARDOWN_CONSTRUCTION_NOT_RUN stage=TRACEME errno={error.errno}', flush=True)
            os._exit(77)
        os.environ.update(LD_LIBRARY_PATH=libdir, GC_UNIT_OTHER_VM_CHILD='RuntimeWorkers.ActivePoolBeforeHarnessShutdown')
        os.execv(elf, [elf, '--gtest_filter=RuntimeWorkers.ActivePoolBeforeHarnessShutdown'])
    os.close(output_write)
    pool_records = []
    pool_ready = threading.Event()
    def forward_output():
        with os.fdopen(output_read) as stream:
            for line in stream:
                print(line, end='', flush=True)
                match = re.fullmatch(r'RUNTIME_WORKERS_LIVE created=(\d+) active=(\d+)\n', line)
                if match:
                    pool_records.append(tuple(map(int, match.groups())))
                    pool_ready.set()
    reader = threading.Thread(target=forward_output, daemon=True)
    reader.start()
    owned = {pid}
    held, joined = None, False
    workers, exits, reaped = set(), set(), set()
    worker_names = {}
    create_output, pending_clone, pthreads = {}, {}, {}
    current_join = None
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
        stage = 'bind-loaded-pthreads'
        address = symbol_address(elf, pid, abi, 'MapleRuntime::GcUnit::CompleteTestRun(int)')
        main_address = symbol_address(elf, pid, abi, 'main')
        abi.breakpoints(pid, [main_address])
        trace(CONT, pid)
        _, status = wait(pid)
        if not os.WIFSTOPPED(status) or os.WSTOPSIG(status) != signal.SIGTRAP or status >> 16:
            raise RuntimeError('main hardware breakpoint not reached')
        if getattr(abi.regset(pid), abi.pc_name) != main_address:
            raise RuntimeError('wrong actual main entry PC')
        symbols = pthread_addresses(pid, abi, libdir)
        abi.breakpoints(pid, [symbols['pthread_create'], symbols['pthread_join'], address])
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
                if child.value in owned:
                    raise RuntimeError('duplicate clone identity')
                owned.add(child.value)
                emit('CLONE', parent=tid, child=child.value)
                if tid == pid:
                    if tid not in create_output or tid not in pending_clone:
                        raise RuntimeError('clone without actual pthread_create/ABI observation')
                    clone = pending_clone.pop(tid)
                    output = create_output.pop(tid)
                    handle = read_integer(tid, output, 8)
                    parent_value = read_integer(tid, clone['parent_tid'])
                    child_value = read_integer(tid, clone['child_tid'])
                    if not handle or handle in pthreads or parent_value != child.value:
                        raise RuntimeError('pthread output/clone TID identity mismatch')
                    pthreads[handle] = dict(tid=child.value, handle=handle, **clone)
                    emit('PTHREAD_CLONE', caller=tid, parent_value=parent_value,
                         child_value=child_value, output=output, **pthreads[handle])
                trace(SYSCALL if tid == pid else CONT, tid)
            elif event == 6:
                name, _ = task_record(pid, tid)
                if name.startswith('RuntimeWorker#'):
                    workers.add(tid)
                    if tid in worker_names:
                        raise RuntimeError('duplicate worker exit identity')
                    worker_names[tid] = name
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
                if entry and entry[0] in (abi.clone, 435):
                    pending_clone[tid] = abi.clone_arguments(tid, entry)
                if entry and entry[0] == abi.futex:
                    params = entry[1]
                    expected = params[2]
                    # Bind the public pthread_join argument to this clone's
                    # output handle and kernel clear-child-tid address. expected
                    # is the value loaded by libc, never a task identity.
                    if current_join and current_join['tid'] == held and params[0] == current_join['child_tid']:
                        if params[1] not in (9, 9 | 256) or params[3:6] != [0, 0, 0xffffffff]:
                            raise RuntimeError(f'unsupported actual glibc join args={params}')
                        uaddr = params[0]
                        actual = read_integer(pid, uaddr)
                        if uaddr % 4 or not actual or actual != expected:
                            raise RuntimeError('join futex value mismatch')
                        joined = True
                        join_tid = current_join['tid']
                        emit('JOIN_ABI', tid=join_tid, handle=current_join['handle'],
                             caller=pid, parent_tid=current_join['parent_tid'],
                             child_tid=current_join['child_tid'], uaddr=uaddr,
                             value=actual, args=params, syscall=entry[0])
                        print(f'CONSTRUCT_JOIN_WAIT tid={join_tid} syscall={entry[0]}', flush=True)
                    else:
                        trace(SYSCALL, tid)
                else:
                    trace(SYSCALL, tid)
            elif tid == pid and sig == signal.SIGTRAP and event == 0:
                values = abi.regset(tid)
                pc = getattr(values, abi.pc_name)
                siginfo = ctypes.create_string_buffer(128)
                trace(GETSIGINFO, tid, data=ctypes.addressof(siginfo))
                if struct.unpack_from('<i', siginfo.raw, 8)[0] != 4:  # TRAP_HWBKPT
                    raise RuntimeError('unexpected non-hardware trap during construction')
                arg0 = abi.argument0(values)
                if pc == symbols['pthread_create']:
                    create_output[tid] = arg0
                    emit('PTHREAD_CREATE_ENTRY', caller=tid, output=arg0, pc=pc)
                elif pc == symbols['pthread_join']:
                    current_join = pthreads.get(arg0)
                    emit('PTHREAD_JOIN_ENTRY', caller=tid, handle=arg0, pc=pc,
                         target=current_join['tid'] if current_join else None)
                    if current_join is None:
                        raise RuntimeError('pthread_join target lacks actual creation identity')
                else:
                    raise RuntimeError('unexpected function breakpoint before construction')
                trace(SYSCALL, tid)
            else:
                if tid != pid and sig == signal.SIGSTOP:
                    abi.breakpoints(tid, [])
                trace(SYSCALL if tid == pid else CONT, tid, data=0 if sig in (signal.SIGSTOP, signal.SIGTRAP) else sig)
            if held is not None and joined:
                # The product emitted this before initiating shutdown; the reader
                # drains it independently of ptrace. Outer timeout bounds blocking.
                pool_ready.wait()
                if len(pool_records) != 1 or not 0 < pool_records[0][1] <= pool_records[0][0]:
                    raise RuntimeError('invalid actual runtime worker pool')
                created, active = pool_records[0]
                if len(workers) < created:
                    continue
                if len(workers) != created or Counter(worker_names.values()) != Counter(f'RuntimeWorker#{i}'[:15] for i in range(created)):
                    raise RuntimeError('incomplete actual runtime worker set')
                if join_tid != held or held not in workers or not workers <= owned:
                    raise RuntimeError('worker/join identity mismatch')
                emit('WORKER_SET', workers=sorted(workers), exit_events=sorted(exits), held=held, tgid=pid, created=created, active=active, names=worker_names)
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
        stage = 'held-exit-ready'
        trace(CONT, held)
        ready = os.waitid(os.P_PID, held, os.WEXITED | os.WNOWAIT | WALL)
        if ready.si_pid != held or ready.si_code != os.CLD_EXITED or ready.si_status != 0:
            raise RuntimeError(f'unexpected held exit event: {ready}')
        name, state = task_record(pid, held)
        if name != 'RuntimeWorker#0' or state != 'Z' or held in reaped:
            raise RuntimeError('held exit readiness is not unreaped Z')
        emit('HELD_EXIT_READY', tid=held, si_pid=ready.si_pid, si_code=ready.si_code,
             si_status=ready.si_status, state=state, reaped=False)
        cleared = read_integer(pid, current_join['child_tid'])
        emit('JOIN_CLEARED', tid=held, handle=current_join['handle'],
             child_tid=current_join['child_tid'], value=cleared)
        if cleared != 0:
            raise RuntimeError('actual join wait address not cleared after non-reap exit')
        # Remaining joins execute normally. Keep only the completion observer.
        abi.breakpoints(pid, [address])
        stage = 'completion-breakpoint'
        trace(CONT, pid)
        waited, status = wait(pid)
        if not os.WIFSTOPPED(status) or os.WSTOPSIG(status) != signal.SIGTRAP or status >> 16:
            raise RuntimeError(f'completion breakpoint not reached status={status:x}')
        values = abi.regset(pid)
        pc = getattr(values, abi.pc_name)
        siginfo = ctypes.create_string_buffer(128)
        trace(GETSIGINFO, pid, data=ctypes.addressof(siginfo))
        code = struct.unpack_from('<i', siginfo.raw, 8)[0]
        emit('BREAKPOINT_STOP', pc=pc, expected=address, si_code=code, siginfo=siginfo.raw.hex())
        if pc != address or code != 4:
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
        abi.breakpoints(pid, [])
        check = abi.regset(pid)
        if bytes(check) != bytes(values):
            raise RuntimeError('observer changed inferior registers')
        emit('BREAKPOINT_RESTORED', pc=address, text_written=False, registers_written=False)
        trace(CONT, pid)
        stage = 'normal-exit'
        while True:
            tid, status = wait()
            if os.WIFEXITED(status):
                if tid != pid:
                    continue
                if os.WEXITSTATUS(status) != 0:
                    raise RuntimeError(f'product exit rc={os.WEXITSTATUS(status)}')
                reader.join()
                if len(pool_records) != 1:
                    raise RuntimeError('duplicate product pool record')
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
