"""Fail closed on the actual three-process teardown runner records."""
import argparse
import json
from pathlib import Path
import re

TEST = 'RuntimeWorkers.ActivePoolBeforeHarnessShutdown'
SENTINEL = 'GC_UNIT_OTHER_VM_OKIDOKI ' + TEST


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
            if not ws['workers'] or set(ws['workers']) != set(ws['exit_events']) or int(held[0]) != ws['held'] or ws['held'] not in ws['workers']:
                raise ValueError(f'{phase}: incomplete worker exit events')
            pools = re.findall(r'^RUNTIME_WORKERS_LIVE created=(\d+) active=(\d+)$', log, re.M)
            names = ws['names']
            if (len(pools) != 1 or tuple(map(int, pools[0])) != (ws['created'], ws['active'])
                    or not 0 < ws['active'] <= ws['created']
                    or len(ws['workers']) != ws['created']
                    or set(map(int, names)) != set(ws['workers'])
                    or set(names.values()) != {f'RuntimeWorker#{i}' for i in range(ws['created'])}):
                raise ValueError(f'{phase}: full product worker pool missing')
            ready = records('HELD_EXIT_READY')
            if len(ready) != 1 or ready[0] != dict(tid=ws['held'], si_pid=ws['held'], si_code=1, si_status=0, state='Z', reaped=False):
                raise ValueError(f'{phase}: exact non-reap exit readiness missing')
            lines = log.splitlines()
            def position(prefix):
                hits = [i for i, line in enumerate(lines) if line.startswith(prefix)]
                if len(hits) != 1:
                    raise ValueError(f'{phase}: missing/duplicate ordered event {prefix}')
                return hits[0]
            sequence = ['WORKER_SET ', 'HELD_EXIT_READY ', 'COMPLETE_AFTER_JOIN ',
                        'AFTER_TRACER_REAP task_exists=False', 'BREAKPOINT_RESTORED ',
                        SENTINEL, 'TEARDOWN_CONSTRUCT_EXECUTED product_rc=0', 'CONSTRUCTION_CLEANUP ']
            if phase == 'live':
                sequence.insert(1, 'TEARDOWN_CONSTRUCT_PRE_EXIT ')
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
            if len(regsets) != 3 or any(r['length'] != length or len(bytes.fromhex(r['raw'])) != length for r in regsets):
                raise ValueError(f'{phase}: full register read/write/restore missing')
            entries, joins = records('SYSCALL_ENTRY'), records('JOIN_ABI')
            if len(joins) != 1 or not any(r['args'] == joins[0]['args'] and r['nr'] == joins[0]['syscall'] and r['length'] >= 80 for r in entries):
                raise ValueError(f'{phase}: raw syscall entry/join missing')
            texts, restores = records('BREAKPOINT_TEXT'), records('BREAKPOINT_RESTORED')
            if len(texts) != 1 or len(restores) != 1 or texts[0]['patched'] != texts[0]['readback'] or restores[0]['word'] != texts[0]['original']:
                raise ValueError(f'{phase}: breakpoint text restore missing')
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
