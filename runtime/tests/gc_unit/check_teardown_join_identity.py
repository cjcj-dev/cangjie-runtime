"""Clear-word identity of the held RuntimeWorker#0 join.

Tid-word glibc waits with the kernel tid. joinstate glibc waits with 1 or 2.
Both are accepted only at that worker's CLONE_CHILD_CLEARTID address.
"""
import difflib
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import check_teardown_exit as probe
import check_teardown_records as records

GUARD = (
    '    # IDENTITY_GUARD: only the captured CLONE_CHILD_CLEARTID word can be the held worker.\n'
    '    if len(owners) != 1:\n'
    '        return None\n'
)
# Weakening used only by the cut arm: a lone known worker plus val 1/2 qualifies
# even when the futex address is not its clear word. Restored before the process exits.
GUARD_CUT = (
    '    # IDENTITY_GUARD_CUT: val 1/2 no longer has to name the captured clear word.\n'
    '    if len(owners) != 1 and not (len(clear_by_tid) == 1 and val in (1, 2)):\n'
    '        return None\n'
    '    if len(owners) != 1:\n'
    '        owners = list(clear_by_tid)\n'
)
SOURCE = HERE / 'check_teardown_exit.py'
HELD = 'RuntimeWorker#0'


def expect(condition, detail):
    if not condition:
        raise AssertionError(detail)


def held_comm(worker):
    def comm(tid):
        if tid != worker:
            raise AssertionError(f'comm of unrelated tid {tid}')
        return HELD
    return comm


def word_is(value):
    def word(addr):
        return value
    return word


def tid_word_held_accepts():
    # 22.04 shape: uaddr is the tid word, val is the kernel tid.
    uaddr, tid = 140594742929680, 12752
    got = probe.match_held_worker_join(
        [uaddr, 265, tid, 0, 0, 0xffffffff], {tid: uaddr}, held_comm(tid), word_is(tid))
    expect(got == tid, got)


def joinstate_1_held_accepts():
    got = probe.match_held_worker_join(
        [0x5000, 265, 1, 0, 0, 0xffffffff], {12587: 0x5000}, held_comm(12587), word_is(1))
    expect(got == 12587, got)


def joinstate_2_held_accepts():
    got = probe.match_held_worker_join(
        [0x5000, 9 | 256, 2, 0, 0, 0xffffffff], {12587: 0x5000}, held_comm(12587), word_is(2))
    expect(got == 12587, got)


def op9_tid_still_accepted():
    got = probe.match_held_worker_join(
        [0x5000, 9, 12587, 0, 0, 0xffffffff], {12587: 0x5000}, held_comm(12587), word_is(12587))
    expect(got == 12587, got)


def unrelated_joinstate_not_held():
    # One captured worker, but this val=1 futex is a different address.
    seen = []
    got = probe.match_held_worker_join(
        [0x2000, 265, 1, 0, 0, 0xffffffff], {12752: 0x1000},
        lambda tid: seen.append(('comm', tid)) or HELD,
        lambda addr: seen.append(('word', addr)) or 1)
    expect(got is None, got)
    expect(seen == [], seen)


def unrelated_tid_not_held():
    got = probe.match_held_worker_join(
        [0x2000, 265, 12752, 0, 0, 0xffffffff], {12752: 0x1000}, held_comm(12752), word_is(12752))
    expect(got is None, got)


def other_worker_joinstate_not_held():
    clear = {10: 0x1000, 20: 0x2000}

    def comm(tid):
        return {10: 'ZStat', 20: HELD}[tid]

    got = probe.match_held_worker_join(
        [0x1000, 265, 1, 0, 0, 0xffffffff], clear, comm, word_is(1))
    expect(got is None, got)
    got = probe.match_held_worker_join(
        [0x3000, 265, 1, 0, 0, 0xffffffff], clear, comm, word_is(1))
    expect(got is None, got)
    got = probe.match_held_worker_join(
        [0x2000, 265, 2, 0, 0, 0xffffffff], clear, comm, word_is(2))
    expect(got == 20, got)


def private_val1_not_join():
    # 26.04 also has FUTEX_WAIT|FUTEX_PRIVATE (op 129) with val 1. Not a join.
    got = probe.match_held_worker_join(
        [0x5000, 129, 1, 0, 0, 0x5004], {12587: 0x5000}, held_comm(12587), word_is(1))
    expect(got is None, got)


def unknown_value_fails_closed():
    try:
        probe.match_held_worker_join(
            [0x5000, 265, 3, 0, 0, 0xffffffff], {12587: 0x5000}, held_comm(12587), word_is(3))
    except RuntimeError as error:
        expect('unknown clear-word join value' in str(error), error)
        return
    raise AssertionError('detached/unknown value accepted')


def exited_value_fails_closed():
    try:
        probe.match_held_worker_join(
            [0x5000, 265, 0, 0, 0, 0xffffffff], {12587: 0x5000}, held_comm(12587), word_is(0))
    except RuntimeError as error:
        expect('unknown clear-word join value' in str(error), error)
        return
    raise AssertionError('cleared word accepted')


def word_mismatch_fails_closed():
    try:
        probe.match_held_worker_join(
            [0x5000, 265, 1, 0, 0, 0xffffffff], {12587: 0x5000}, held_comm(12587), word_is(2))
    except RuntimeError as error:
        expect('value mismatch' in str(error), error)
        return
    raise AssertionError('stale word accepted')


def ambiguous_clear_fails_closed():
    try:
        probe.match_held_worker_join(
            [0x5000, 265, 1, 0, 0, 0xffffffff], {10: 0x5000, 11: 0x5000}, held_comm(10), word_is(1))
    except RuntimeError as error:
        expect('ambiguous' in str(error), error)
        return
    raise AssertionError('ambiguous clear word accepted')


def clone3_child_tid_offset():
    child, parent = 0xABC0, 0xDEF0
    blob = struct.pack('<4Q', 0x3D0F00, 0, child, parent)
    seen = []

    def read_blob(addr, size):
        seen.append((addr, size))
        return blob

    got = probe.decode_clear_uaddr(False, 435, [0x100000, 88, 0, 0, 0, 0], read_blob)
    expect(got == child, got)
    expect(seen == [(0x100000, 32)], seen)
    pending = {}
    probe.note_clone_entry(pending, 7, False, 435, [0x100000, 88, 0, 0, 0, 0], read_blob)
    table = {}
    probe.bind_clear_word(table, 12587, pending.pop(7))
    expect(table == {12587: child}, table)


def clone3_without_cleartid():
    blob = struct.pack('<4Q', 0x1D0F00, 0, 1, 0xDEF0)
    got = probe.decode_clear_uaddr(False, 435, [0x100000, 32, 0, 0, 0, 0], lambda addr, size: blob)
    expect(got is probe.NO_CLEAR, got)


def clone3_short_rejected():
    try:
        probe.decode_clear_uaddr(False, 435, [0x100000, 24, 0, 0, 0, 0], lambda addr, size: b'')
    except RuntimeError as error:
        expect('malformed clone3' in str(error), error)
        return
    raise AssertionError('short clone3 accepted')


def legacy_clone_x86_child_tid():
    child, parent, tls = 0x1110, 0x2220, 0x3330
    args = [0x3D0F00, 0x4000, parent, child, tls, 0]

    def fail_read(addr, size):
        raise AssertionError('legacy clone has no clone_args blob')

    got = probe.decode_clear_uaddr(False, 56, args, fail_read)
    expect(got == child, got)


def legacy_clone_aarch64_child_tid():
    child, parent, tls = 0x1110, 0x2220, 0x3330
    args = [0x3D0F00, 0x4000, parent, tls, child, 0]
    got = probe.decode_clear_uaddr(True, 220, args, lambda addr, size: b'')
    expect(got == child, got)
    try:
        probe.decode_clear_uaddr(True, 56, args, lambda addr, size: b'')
    except RuntimeError as error:
        expect('not a clone syscall' in str(error), error)
        return
    raise AssertionError('x86 clone number accepted on aarch64')


def records_tid_accept():
    records.held_clear_join(
        [{'child': 12752, 'clear_uaddr': 0x1000}],
        {'tid': 12752, 'uaddr': 0x1000, 'value': 12752}, 12752)


def records_joinstate_accept():
    for value in (1, 2):
        records.held_clear_join(
            [{'child': 12587, 'clear_uaddr': 0x5000}],
            {'tid': 12587, 'uaddr': 0x5000, 'value': value}, 12587)


def records_unrelated_uaddr_reject():
    try:
        records.held_clear_join(
            [{'child': 12587, 'clear_uaddr': 0x5000}],
            {'tid': 12587, 'uaddr': 0x6000, 'value': 1}, 12587)
    except ValueError:
        return
    raise AssertionError('unrelated join uaddr accepted')


def records_val_is_not_tid():
    try:
        records.held_clear_join(
            [{'child': 12587, 'clear_uaddr': 0x5000}],
            {'tid': 1, 'uaddr': 0x5000, 'value': 1}, 12587)
    except ValueError:
        return
    raise AssertionError('joinstate value recorded as the worker tid')


def records_bad_value_reject():
    for value in (0, 3, 7):
        try:
            records.held_clear_join(
                [{'child': 12587, 'clear_uaddr': 0x5000}],
                {'tid': 12587, 'uaddr': 0x5000, 'value': value}, 12587)
        except ValueError:
            continue
        raise AssertionError(f'value {value} accepted')


CASES = [
    tid_word_held_accepts,
    joinstate_1_held_accepts,
    joinstate_2_held_accepts,
    op9_tid_still_accepted,
    unrelated_joinstate_not_held,
    unrelated_tid_not_held,
    other_worker_joinstate_not_held,
    private_val1_not_join,
    unknown_value_fails_closed,
    exited_value_fails_closed,
    word_mismatch_fails_closed,
    ambiguous_clear_fails_closed,
    clone3_child_tid_offset,
    clone3_without_cleartid,
    clone3_short_rejected,
    legacy_clone_x86_child_tid,
    legacy_clone_aarch64_child_tid,
    records_tid_accept,
    records_joinstate_accept,
    records_unrelated_uaddr_reject,
    records_val_is_not_tid,
    records_bad_value_reject,
]


def suite():
    failed = []
    for case in CASES:
        try:
            case()
        except Exception as error:
            failed.append(case.__name__)
            print(f'FAIL {case.__name__} {type(error).__name__}: {error}', flush=True)
        else:
            print(f'PASS {case.__name__}', flush=True)
    print('SUITE_FAILED ' + ','.join(failed), flush=True)
    return 1 if failed else 0


HELPER = r"""
#define _GNU_SOURCE
#include <pthread.h>
#include <sys/prctl.h>
#include <unistd.h>

static void *named(void *arg) {
    prctl(PR_SET_NAME, arg, 0, 0, 0);
    usleep(300000);
    return 0;
}

int main(void) {
    pthread_t zstat, worker;
    if (pthread_create(&zstat, 0, named, "ZStat")) return 2;
    if (pthread_create(&worker, 0, named, "RuntimeWorker#0")) return 3;
    if (pthread_join(zstat, 0)) return 4;
    if (pthread_join(worker, 0)) return 5;
    return 0;
}
"""


def live(evidence):
    """Real glibc clone3/clone plus pthread_join, traced with the harness decoder."""
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    evidence.mkdir(parents=True, exist_ok=True)
    source = evidence / 'join_helper.c'
    helper = evidence / 'join_helper'
    source.write_text(HELPER)
    compiled = subprocess.run(
        ['gcc', '-O2', '-pthread', '-o', str(helper), str(source)],
        capture_output=True, text=True)
    (evidence / 'helper-compile.log').write_text(compiled.stdout + compiled.stderr)
    if compiled.returncode != 0:
        raise RuntimeError(f'helper compile rc={compiled.returncode}')
    pid = os.fork()
    if pid == 0:
        try:
            probe.trace(probe.TRACEME, 0)
        except probe.TraceError as error:
            os._exit(77)
        os.execv(str(helper), [str(helper)])
        os._exit(127)
    accepted = []
    saw_zstat = False
    clone_blobs = {}
    pending = {}
    clear_by_tid = {}

    def alarm(_signum, _frame):
        raise TimeoutError('live join probe timed out')

    signal.signal(signal.SIGALRM, alarm)
    signal.alarm(15)
    try:
        who, status = os.waitpid(pid, 0)
        if not os.WIFSTOPPED(status) or os.WSTOPSIG(status) != signal.SIGTRAP:
            raise RuntimeError(f'helper did not stop at exec status={status:#x}')
        abi = probe.ABI('aarch64' if os.uname().machine == 'aarch64' else 'x86_64')
        probe.trace(probe.SETOPTIONS, pid, data=1 | 8 | 64 | 0x100000)
        probe.trace(probe.SYSCALL, pid)
        steps = 0
        helper_rc = None
        while helper_rc is None:
            steps += 1
            if steps > 100000:
                raise RuntimeError('live join probe did not finish')
            who, status = os.waitpid(-1, probe.WALL)
            if os.WIFEXITED(status) or os.WIFSIGNALED(status):
                if who == pid:
                    helper_rc = os.WEXITSTATUS(status) if os.WIFEXITED(status) else 128 + os.WTERMSIG(status)
                continue
            if not os.WIFSTOPPED(status):
                raise RuntimeError(f'unexpected wait status={status:#x}')
            sig, event = os.WSTOPSIG(status), status >> 16
            if event == 3:
                child = __import__('ctypes').c_ulong()
                probe.trace(probe.GETEVENTMSG, who, data=__import__('ctypes').addressof(child))
                if who in pending:
                    clear = pending.pop(who)
                else:
                    stopped = abi.try_syscall(who)
                    if not stopped or stopped[0] not in (abi.clone, abi.clone3):
                        raise RuntimeError('clone clear word not captured')
                    clear = probe.decode_clear_uaddr(
                        abi.arm, stopped[0], stopped[1], lambda addr, size: probe.peek_bytes(who, addr, size))
                probe.bind_clear_word(clear_by_tid, child.value, clear)
                if clear is not probe.NO_CLEAR and who in clone_blobs:
                    blob = clone_blobs[who]
                    child_ptr = struct.unpack_from('<Q', blob, 16)[0]
                    if clear != child_ptr:
                        raise RuntimeError(f'clear word {clear:#x} is not clone_args child_tid {child_ptr:#x}')
                probe.trace(probe.SYSCALL if who == pid else probe.CONT, who)
            elif event == 6:
                probe.trace(probe.CONT, who)
            elif who == pid and sig == signal.SIGTRAP | 128:
                entry = abi.entry(who)
                if entry and entry[0] == abi.clone3:
                    uargs, size = entry[1][0], entry[1][1]
                    clone_blobs[who] = probe.peek_bytes(who, uargs, 32)
                if entry and entry[0] in (abi.clone, abi.clone3):
                    probe.note_clone_entry(
                        pending, who, abi.arm, entry[0], entry[1],
                        lambda addr, size: probe.peek_bytes(who, addr, size))
                    probe.trace(probe.SYSCALL, who)
                elif entry and entry[0] == abi.futex and probe.join_shaped(entry[1]):
                    params = entry[1]
                    worker = probe.match_held_worker_join(
                        params, clear_by_tid,
                        lambda worker_tid: probe.task_comm(pid, worker_tid),
                        lambda uaddr: probe.futex_word(pid, uaddr))
                    owners = [tid for tid, addr in clear_by_tid.items() if addr == params[0]]
                    if len(owners) == 1 and probe.task_comm(pid, owners[0]) == 'ZStat':
                        saw_zstat = True
                        if worker is not None:
                            raise RuntimeError('ZStat join accepted as RuntimeWorker#0')
                    if worker is not None:
                        accepted.append((worker, params[0], params[2], clear_by_tid[worker]))
                    probe.trace(probe.SYSCALL, who)
                else:
                    probe.trace(probe.SYSCALL, who)
            else:
                data = 0 if sig in (signal.SIGSTOP, signal.SIGTRAP) else sig
                probe.trace(probe.SYSCALL if who == pid else probe.CONT, who, data=data)
        if helper_rc != 0:
            raise RuntimeError(f'helper rc={helper_rc}')
        if len(accepted) != 1:
            raise RuntimeError(f'accepted joins={accepted}')
        worker, uaddr, value, clear = accepted[0]
        if uaddr != clear or probe.task_comm(pid, worker) not in (HELD, None):
            raise RuntimeError(f'accepted identity tid={worker} uaddr={uaddr:#x} clear={clear:#x}')
        if value != worker and value not in (1, 2):
            raise RuntimeError(f'accepted value {value}')
        if not saw_zstat:
            raise RuntimeError('ZStat join was not observed')
        # The worker is reaped by its joiner, so comm may already be gone. The
        # captured clear word and the value protocol are the identity.
        print(f'LIVE_JOIN tid={worker} uaddr={uaddr} value={value} clear={clear} '
              f'zstat=1 glibc={os.confstr("CS_GNU_LIBC_VERSION")}', flush=True)
    finally:
        signal.alarm(0)
        if pid:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                while True:
                    os.waitpid(-1, probe.WALL)
            except ChildProcessError:
                pass


def run_suite(label, evidence):
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    proc = subprocess.run(
        [sys.executable, str(Path(__file__).resolve()), '--suite'],
        capture_output=True, text=True, env=env)
    log = evidence / f'{label}.log'
    log.write_text(proc.stdout + proc.stderr)
    print(proc.stdout, end='')
    failed = ''
    for line in proc.stdout.splitlines():
        if line.startswith('SUITE_FAILED '):
            failed = line[len('SUITE_FAILED '):]
    return proc.returncode, failed, log


def main():
    if '--suite' in sys.argv:
        return suite()
    evidence = Path('/root/cj_build/agent_scratch/impl_teardown_join_grok_1108')
    evidence.mkdir(parents=True, exist_ok=True)
    original = SOURCE.read_bytes()
    if original.decode().count(GUARD) != 1:
        raise RuntimeError('identity guard is not unique')
    try:
        rc, failed, _ = run_suite('suite-green-1', evidence)
        if rc != 0 or failed:
            raise RuntimeError(f'green suite rc={rc} failed={failed}')
        cut = original.decode().replace(GUARD, GUARD_CUT, 1)
        SOURCE.write_text(cut)
        (evidence / 'cut.diff').write_text(''.join(difflib.unified_diff(
            original.decode().splitlines(True), cut.splitlines(True),
            fromfile='a/runtime/tests/gc_unit/check_teardown_exit.py',
            tofile='b/runtime/tests/gc_unit/check_teardown_exit.py')))
        rc, failed, _ = run_suite('suite-cut', evidence)
        if failed != 'unrelated_joinstate_not_held':
            raise RuntimeError(f'cut red was not precise rc={rc} failed={failed}')
    finally:
        SOURCE.write_bytes(original)
    if SOURCE.read_bytes() != original:
        raise RuntimeError('guard restoration mismatch')
    rc, failed, _ = run_suite('suite-green-2', evidence)
    if rc != 0 or failed:
        raise RuntimeError(f'restored suite rc={rc} failed={failed}')
    live(evidence)
    print('JOIN_IDENTITY_OK', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
