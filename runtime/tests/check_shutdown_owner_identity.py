import json
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).parent / 'gc_unit'))
from check_native_detach import MI, digest, emit, event_field


def main():
    elf, libdir, output = sys.argv[1:]
    library = Path(libdir).resolve() / 'libcangjie-runtime.so'
    os.environ['LD_LIBRARY_PATH'] = str(library.parent)
    emit('OWNER_IDENTITY', elf=digest(elf), so=digest(library), library=str(library))
    debugger = MI(elf, output + '.gdb.log')
    try:
        for setting in ['pagination off', 'confirm off', 'non-stop on', 'mi-async on',
                        'print thread-events off']:
            debugger.console('set ' + setting)
        ready = debugger.breakpoint('ShutdownOwnerReuseReady')
        debugger.cmd('-exec-run')
        event = debugger.stop()
        if event_field(event, 'bkptno') != ready:
            raise RuntimeError('Owner-reuse input did not reach its entry')
        current = event_field(event, 'thread-id')
        owner = debugger.number('shutdownOwnerPthread', current)
        reused = debugger.number('shutdownReusedPthread', current)
        fini = debugger.number('shutdownOwnerResult', current)
        maps = Path('/proc/' + str(debugger.pid) + '/maps').read_text()
        Path(output + '.maps').write_text(maps)
        if owner != reused or fini != 0 or str(library) not in maps:
            raise RuntimeError('Owner identity precondition failed')
        emit('OWNER_REUSE_PRECONDITION', owner=owner, reused=reused, equal=True, fini=fini)
        blocked = debugger.breakpoint('MapleRuntime::MutatorManager::MutatorManagementRLock', thread=current)
        created = debugger.breakpoint('MapleRuntime::CangjieRuntime::CreateSubSchedulerAndInit', thread=current)
        debugger.delete(ready)
        debugger.resume(current)
        event = debugger.stop()
        stopped = event_field(event, 'bkptno') == blocked
        terminal = debugger.number('*(bool*)&MapleRuntime::VMExit::vmExited', current)
        owner_token = debugger.number('MapleRuntime::VMExit::shutdownThread', current)
        current_token = debugger.number('MapleRuntime::nativeThreadIdentity', current)
        waiting = False
        if stopped:
            debugger.delete(blocked)
            yielding = debugger.breakpoint('sched_yield', thread=current)
            debugger.resume(current)
            event = debugger.stop()
            frames = debugger.cmd('-stack-list-frames --thread ' + current)
            waiting = event_field(event, 'bkptno') == yielding and 'LockRead' in frames
        passed = stopped and waiting and terminal == 1 and owner_token != 0 and current_token == 0
        emit('OWNER_REUSE_TARGET_EXECUTED', passed=passed, terminal=terminal,
             owner_token=owner_token, current_token=current_token, lock_wait=waiting,
             new_owner_creation=event_field(event, 'bkptno') == created)
        Path(output + '.json').write_text(json.dumps(dict(passed=passed, owner=owner,
                                                        reused=reused, terminal=terminal)) + '\n')
        debugger.expression('shutdownOwnerObservationDone = 1', current)
        event = debugger.stop()
        if 'exit-code="0"' not in event and 'exited-normally' not in event:
            raise RuntimeError('Owner-reuse host did not complete: ' + event)
        return 0 if passed else 1
    finally:
        debugger.cmd('-gdb-exit')
        debugger.log.close()


if __name__ == '__main__':
    sys.exit(main())
