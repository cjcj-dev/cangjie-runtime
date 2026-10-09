"""Fail closed on the actual three-process teardown runner records."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import struct

TEST = 'RuntimeWorkers.ActivePoolBeforeHarnessShutdown'
SENTINEL = 'GC_UNIT_OTHER_VM_OKIDOKI ' + TEST


def verify_join_identity(records, ws, phase):
    """Consume the actual create -> clone -> join -> wait -> clear chain."""
    def require(condition, reason):
        if not condition:
            print(f'ASSERT_TEARDOWN_JOIN_IDENTITY phase={phase} FAIL reason={reason}')
            raise ValueError(f'{phase}: join identity: {reason}')
    joins = records('JOIN_ABI')
    require(len(joins) == 1, 'unique-wait')
    join = joins[0]
    bindings = [r for r in records('PTHREAD_CLONE') if r['tid'] == ws['held']]
    require(len(bindings) == 1, 'unique-held-creation')
    binding = bindings[0]
    require(join['tid'] == ws['held'] and join['caller'] == ws['tgid']
            and join['handle'] == binding['handle'], 'join-target-handle')
    require(join['parent_tid'] == binding['parent_tid']
            and join['child_tid'] == binding['child_tid'] == join['uaddr'] == join['args'][0],
            'wait-clear-address')
    require(join['value'] == join['args'][2] and join['value'] > 0
            and join['args'][1] in (9, 265) and join['args'][3:] == [0, 0, 0xffffffff],
            'actual-shared-wait-value')
    calls = records('PTHREAD_JOIN_ENTRY')
    require(any(r['caller'] == ws['tgid'] and r['handle'] == join['handle']
                and r['target'] == ws['held'] for r in calls), 'public-join-call')
    require(any(r['parent'] == binding['caller'] and r['child'] == binding['tid']
                for r in records('CLONE')) and binding['parent_value'] == ws['held'],
            'kernel-clone-tid')
    require(any(r['caller'] == binding['caller'] and r['output'] == binding['output']
                for r in records('PTHREAD_CREATE_ENTRY')), 'public-create-output')
    clones = records('CLONE_ABI')
    require(any(all(r[k] == binding[k] for k in ('caller', 'syscall', 'args', 'flags',
                                                'parent_tid', 'child_tid', 'clone3_raw'))
                for r in clones), 'actual-clone-arguments')
    domain = records('CONSTRUCTION_DOMAIN')
    require(len(domain) == 1 and domain[0]['machine'] in ('x86_64', 'aarch64'), 'actual-abi')
    arm = domain[0]['machine'] == 'aarch64'
    if binding['syscall'] == 435:
        require(binding['args'][1] >= 64, 'clone3-size')
        raw = bytes.fromhex(binding['clone3_raw'])
        require(len(raw) == 64, 'clone3-raw-size')
        flags, _, child_tid, parent_tid, *_ = struct.unpack('<8Q', raw)
    else:
        require(binding['syscall'] == (220 if arm else 56), 'clone-syscall-abi')
        flags, _, parent_tid = binding['args'][:3]
        child_tid = binding['args'][4] if arm else binding['args'][3]
    require((flags, parent_tid, child_tid) == (binding['flags'], binding['parent_tid'], binding['child_tid'])
            and flags & 0x310100 == 0x310100, 'kernel-clear-contract')
    entries = records('SYSCALL_ENTRY')
    require(any(r['tid'] == binding['caller'] and r['nr'] == binding['syscall']
                and r['args'] == binding['args'] and r['length'] >= 80 for r in entries), 'raw-clone-entry')
    require(any(r['tid'] == join['caller'] and r['nr'] == join['syscall']
                and r['args'] == join['args'] and r['length'] >= 80 for r in entries)
            and join['syscall'] == (98 if arm else 202), 'raw-wait-entry')
    cleared = records('JOIN_CLEARED')
    require(cleared == [dict(tid=ws['held'], handle=join['handle'], child_tid=join['uaddr'], value=0)],
            'nonreap-kernel-clear')
    print(f'ASSERT_TEARDOWN_JOIN_IDENTITY phase={phase} PASS')


def verify(directory, cut=False):
    directory = Path(directory)
    results = {}
    for phase, stem in [('gdb', 'teardown'), ('exited', 'teardown-exited'), ('live', 'teardown-live')]:
        log = (directory / (stem + '.log')).read_text()
        rc = int((directory / (stem + '.rc')).read_text().strip())
        expected = 1 if phase == 'live' or phase == 'exited' and cut else 0
        if rc != expected:
            raise ValueError(f'{phase}: rc={rc}, expected={expected}')
        for text in ('[  RUN   ] ' + TEST, SENTINEL):
            if text not in log.splitlines():
                raise ValueError(f'{phase}: missing {text}')
        assertion = 'ASSERT_TEARDOWN_BEFORE_SENTINEL samples=1 ' + ('FAIL' if expected else 'PASS')
        if log.splitlines().count(assertion) != 1:
            raise ValueError(f'{phase}: missing exact target assertion {assertion}')
        if phase == 'gdb':
            for text in ('ASSERT_TEARDOWN_LIVE samples=1 PASS',
                         'ASSERT_TEARDOWN_POOL_STOPPED samples=1 PASS',
                         'ASSERT_TEARDOWN_EXECUTED exits=[0] errors=[] PASS'):
                if log.splitlines().count(text) != 1:
                    raise ValueError('gdb: missing ' + text)
        else:
            if log.splitlines().count('TEARDOWN_CONSTRUCT_EXECUTED product_rc=0') != 1:
                raise ValueError(f'{phase}: product completion missing')
            complete = re.findall(r'^COMPLETE_AFTER_JOIN held_tid=(\d+) comm=RuntimeWorker#0 state=Z$', log, re.M)
            held = re.findall(r'^CONSTRUCT_HOLD_EXIT tid=(\d+) name=RuntimeWorker#0$', log, re.M)
            join = re.findall(r'^CONSTRUCT_JOIN_WAIT tid=(\d+) syscall=(?:98|202)$', log, re.M)
            if len(held) != 1 or complete != held or join != held:
                raise ValueError(f'{phase}: incomplete held/join/exited identity')
            if phase == 'live' and f'TEARDOWN_CONSTRUCT_PRE_EXIT tid={held[0]} accepted=False' not in log.splitlines():
                raise ValueError('live: missing actual rejected sample')
            def records(kind):
                return [json.loads(line[len(kind)+1:]) for line in log.splitlines() if line.startswith(kind + ' ')]
            sets = records('WORKER_SET')
            if len(sets) != 1:
                raise ValueError(f'{phase}: worker set missing')
            ws = sets[0]
            if (not ws['workers'] or len(set(ws['workers'])) != len(ws['workers'])
                    or len(set(ws['exit_events'])) != len(ws['exit_events'])
                    or set(ws['workers']) != set(ws['exit_events'])) or int(held[0]) != ws['held'] or ws['held'] not in ws['workers']:
                raise ValueError(f'{phase}: incomplete worker exit events')
            pools = re.findall(r'^RUNTIME_WORKERS_LIVE created=(\d+) active=(\d+)$', log, re.M)
            names = ws['names']
            if (len(pools) != 1 or tuple(map(int, pools[0])) != (ws['created'], ws['active'])
                    or not 0 < ws['active'] <= ws['created']
                    or len(ws['workers']) != ws['created']
                    or set(map(int, names)) != set(ws['workers'])
                    or Counter(names.values()) != Counter(f'RuntimeWorker#{i}'[:15] for i in range(ws['created']))):
                raise ValueError(f'{phase}: full product worker pool missing')
            # Names establish the product family/multiplicity; kernel TIDs establish
            # identity. Every member must belong to this clone tree and have its
            # own observed PTRACE_EVENT_EXIT with the same TGID/name.
            owned = {ws['tgid']}
            for clone in records('CLONE'):
                if clone['parent'] not in owned or clone['child'] in owned:
                    raise ValueError(f'{phase}: invalid clone ownership')
                owned.add(clone['child'])
            if not set(ws['workers']) <= owned or ws['tgid'] in ws['workers']:
                raise ValueError(f'{phase}: worker outside clone ownership')
            exit_tids = [r['tid'] for r in records('WAIT_EVENT') if r['event'] == 6]
            states = records('TASK_STATE')
            for tid in ws['workers']:
                observations = [r for r in states if r['tid'] == tid]
                if (exit_tids.count(tid) != 1 or not observations
                        or any(r['pid'] != ws['tgid'] or r['tgid'] != ws['tgid']
                               or r['name'] != names[str(tid)] for r in observations)):
                    raise ValueError(f'{phase}: worker TID/TGID/exit identity mismatch')
            ready = records('HELD_EXIT_READY')
            if len(ready) != 1 or ready[0] != dict(tid=ws['held'], si_pid=ws['held'], si_code=1, si_status=0, state='Z', reaped=False):
                raise ValueError(f'{phase}: exact non-reap exit readiness missing')
            verify_join_identity(records, ws, phase)
            lines = log.splitlines()
            def position(prefix):
                hits = [i for i, line in enumerate(lines) if line.startswith(prefix)]
                if len(hits) != 1:
                    raise ValueError(f'{phase}: missing/duplicate ordered event {prefix}')
                return hits[0]
            sequence = ['JOIN_ABI ', 'WORKER_SET ', 'HELD_EXIT_READY ', 'JOIN_CLEARED ', 'COMPLETE_AFTER_JOIN ',
                        'AFTER_TRACER_REAP task_exists=False', 'BREAKPOINT_RESTORED ',
                        SENTINEL, 'TEARDOWN_CONSTRUCT_EXECUTED product_rc=0', 'CONSTRUCTION_CLEANUP ']
            if phase == 'live':
                sequence.insert(2, 'TEARDOWN_CONSTRUCT_PRE_EXIT ')
            positions = list(map(position, sequence))
            if positions != sorted(positions):
                raise ValueError(f'{phase}: exit readiness/completion order invalid')
            target_pos = lines.index(assertion)
            if phase == 'live':
                if not position('WORKER_SET ') < target_pos < position('TEARDOWN_CONSTRUCT_PRE_EXIT '):
                    raise ValueError('live: target outside pre-exit sample')
            elif not position('COMPLETE_AFTER_JOIN ') < target_pos < position('AFTER_TRACER_REAP '):
                raise ValueError('exited: target outside unreaped exit sample')
            states = records('TASK_STATE')
            if not any(r['tid'] == ws['held'] and r['tgid'] == ws['tgid'] and r['state'] not in ('Z', 'X') for r in states):
                raise ValueError(f'{phase}: actual held live state missing')
            if not any(r['tid'] == ws['held'] and r['tgid'] == ws['tgid'] and r['state'] == 'Z' for r in states):
                raise ValueError(f'{phase}: actual held exited state missing')
            regsets = records('REGSET')
            domain = records('CONSTRUCTION_DOMAIN')
            if len(domain) != 1:
                raise ValueError(f'{phase}: ABI domain missing')
            length = 272 if domain[0]['machine'] == 'aarch64' else 216
            if len(regsets) < 3 or any(r['writing'] or r['length'] != length or len(bytes.fromhex(r['raw'])) != length for r in regsets):
                raise ValueError(f'{phase}: complete read-only register observations missing')
            if regsets[-1]['raw'] != regsets[-2]['raw']:
                raise ValueError(f'{phase}: inferior registers changed at completion')
            restores = records('BREAKPOINT_RESTORED')
            if len(restores) != 1 or restores[0]['text_written'] or restores[0]['registers_written']:
                raise ValueError(f'{phase}: non-mutating breakpoint cleanup missing')
            hardware = records('HARDWARE_BREAKPOINTS')
            main_hw = [r for r in hardware if r['tid'] == ws['tgid']]
            if len(main_hw) != 4 or main_hw[-1]['addresses'] != [] or records('BREAKPOINT_TEXT'):
                raise ValueError(f'{phase}: hardware observer cleanup missing')
            providers = records('PTHREAD_PROVIDER')
            if sorted(r['symbol'] for r in providers) != ['pthread_create', 'pthread_join']:
                raise ValueError(f'{phase}: actual loaded pthread providers missing')
            for kind, symbol in [('PTHREAD_CREATE_ENTRY', 'pthread_create'), ('PTHREAD_JOIN_ENTRY', 'pthread_join')]:
                provider = next(r for r in providers if r['symbol'] == symbol)
                if any(r['pc'] != provider['address'] for r in records(kind)):
                    raise ValueError(f'{phase}: public pthread entry/provider mismatch')
            cleanup = records('CONSTRUCTION_CLEANUP')
            if len(cleanup) != 1 or set(cleanup[0]['owned']) != set(cleanup[0]['reaped']):
                raise ValueError(f'{phase}: owned process cleanup incomplete')
        results[phase] = {'rc': rc, 'target': assertion}
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('directory', type=Path)
    parser.add_argument('--cut', action='store_true')
    args = parser.parse_args()
    try:
        result = verify(args.directory, args.cut)
    except (ValueError, OSError, KeyError, IndexError) as error:
        print('TEARDOWN_RECORDS_REJECT ' + str(error))
        return 1
    print('TEARDOWN_RECORDS_ACCEPT ' + json.dumps(result, sort_keys=True))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
