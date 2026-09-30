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
    emit('SHUTDOWN_IDENTITY', elf=digest(elf), so=digest(library), library=str(library))
    debugger = MI(elf, output + '.gdb.log')
    results = {}
    try:
        for setting in ['pagination off', 'confirm off', 'non-stop on', 'mi-async on',
                        'print thread-events off', 'args native']:
            debugger.console('set ' + setting)
        debugger.console('handle SIGUSR1 nostop noprint pass')
        checkpoint = debugger.breakpoint('ShutdownCheckpoint')
        debugger.cmd('-exec-run')
        event = debugger.stop()
        if event_field(event, 'bkptno') != checkpoint:
            raise RuntimeError('Host did not return from FiniCJRuntime')
        current = event_field(event, 'thread-id')
        maps = Path('/proc/' + str(debugger.pid) + '/maps').read_text()
        Path(output + '.maps').write_text(maps)
        if str(library) not in maps:
            raise RuntimeError('Loaded runtime identity differs')
        exit_signal = debugger.number('shutdownExitSignal', current)
        pending = debugger.number(
            '*(int*)&MapleRuntime::g_pendingSignals[' + str(exit_signal) + ']', current)
        terminal = debugger.number('*(bool*)&MapleRuntime::VMExit::vmExited', current)
        retained = debugger.number('MapleRuntime::Runtime::runtime != 0', current)
        entered = debugger.number('*(bool*)&shutdownCallbackEntered', current)
        returned = debugger.number('*(bool*)&shutdownCallbackReturned', current)
        results = dict(exit_notification=pending == 1, terminal=terminal == 1,
                       retained_runtime=retained == 1, callback_in_flight=entered == 1 and returned == 0)
        for target, passed in results.items():
            emit('SHUTDOWN_TARGET_EXECUTED', target=target, passed=passed,
                 pending=pending, terminal=terminal, retained=retained,
                 entered=entered, returned=returned)
        debugger.delete(checkpoint)
        debugger.resume(current)
        event = debugger.stop()
        if 'exited-normally' not in event and 'exit-code="0"' not in event:
            raise RuntimeError('Host exit failed: ' + event)
        Path(output + '.json').write_text(json.dumps(results, indent=2) + '\n')
        return 0 if all(results.values()) else 1
    finally:
        debugger.console('set confirm off')
        debugger.cmd('-gdb-exit')
        debugger.log.close()


if __name__ == '__main__':
    sys.exit(main())
