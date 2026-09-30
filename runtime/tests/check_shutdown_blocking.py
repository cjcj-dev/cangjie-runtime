import json
import re
import time
from pathlib import Path

from check_native_detach import emit, event_field


def observe_blocking(debugger, thread, output):
    info = debugger.cmd('-thread-info ' + thread)
    native = re.search(r'LWP (\d+)', info)
    if native is None:
        raise RuntimeError('Missing native thread identity: ' + info)
    task = Path('/proc') / str(debugger.pid) / 'task' / native[1]

    def sample():
        stat = (task / 'stat').read_text()
        fields = stat.rsplit(')', 1)[1].split()
        return dict(stat=stat.strip(), state=fields[0],
                    ticks=int(fields[11]) + int(fields[12]),
                    wchan=(task / 'wchan').read_text().strip(),
                    time_ns=time.monotonic_ns())

    debugger.resume(thread)
    time.sleep(0.25)
    before = sample()
    time.sleep(0.25)
    after = sample()
    passed = (before['state'] == after['state'] == 'S' and
              'futex' in before['wchan'] and 'futex' in after['wchan'] and
              before['ticks'] == after['ticks'])
    Path(output + '.blocking.json').write_text(json.dumps(
        dict(tid=int(native[1]), before=before, after=after, passed=passed)) + '\n')
    emit('TERMINAL_BLOCKING_TARGET_EXECUTED', passed=passed, tid=int(native[1]),
         before=before, after=after)
    info = debugger.cmd('-thread-info ' + thread)
    if 'state="stopped"' not in info:
        debugger.cmd('-exec-interrupt --thread ' + thread)
    event = debugger.stop()
    if event_field(event, 'thread-id') != thread:
        raise RuntimeError('Blocking observation rendezvous failed: ' + event)
    debugger.cmd('-stack-list-frames --thread ' + thread)
    return passed
