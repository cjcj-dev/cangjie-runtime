#!/usr/bin/env python3
"""Observe SCHD_STOP completion before the real TLS shutdown boundary."""
import os
from pathlib import Path
import sys
from check_native_detach import MI, digest, emit, event_field


def main():
    elf, libdir, log = sys.argv[1:]
    so = Path(libdir).resolve() / 'libcangjie-runtime.so'
    os.environ.update(LD_LIBRARY_PATH=str(so.parent), GC_NATIVE_GDB='1',
                      GC_UNIT_FILTER='NativeOwner1286.ShutdownUnlinksLateNative',
                      GC_UNIT_OTHER_VM_CHILD='NativeOwner1286.ShutdownUnlinksLateNative')
    os.environ.pop('GC_UNIT_LIST_TESTS', None)
    emit('NATIVE_SHUTDOWN_IDENTITY', elf=digest(elf), so=digest(so), library=str(so))
    mi = MI(elf, log)
    try:
        for setting in ['pagination off', 'confirm off', 'non-stop on', 'mi-async on', 'print thread-events off']:
            mi.console('set ' + setting)
        mi.console('handle SIGUSR1 nostop noprint pass')
        ready = mi.breakpoint('native1286_shutdown_ready')
        mi.cmd('-exec-run')
        event = mi.stop()
        if event_field(event, 'bkptno') != ready:
            raise RuntimeError('Shutdown fixture not ready')
        main_thread = event_field(event, 'thread-id')
        maps = Path('/proc/' + str(mi.pid) + '/maps').read_text()
        if str(so) not in maps:
            raise RuntimeError('Wrong product library')
        mi.delete(ready)
        stop = mi.breakpoint('MRT_StopGCWork')
        tls = mi.breakpoint('CangjieRuntime.cpp:274')
        mi.resume(main_thread)
        event = mi.stop()
        observed_stop = event_field(event, 'bkptno') == stop
        heap = 'MapleRuntime::ZCollectedHeap::_collected_heap'
        active = mi.number(heap + '->_gc_thread_running._M_base._M_i', main_thread)
        phase = mi.number('MapleRuntime::ConcurrentGCBreakpoints::stopped', main_thread)
        if observed_stop:
            emit('NATIVE_SHUTDOWN_ACTIVE', gc_running=active, phase_stopped=phase)
            mi.expression('native1286_shutdown_release._M_i = 1', main_thread)
            mi.delete(stop)
            mi.resume(main_thread)
            event = mi.stop()
        if event_field(event, 'bkptno') != tls:
            raise RuntimeError('TLS boundary not reached: ' + event)
        running = mi.number(heap + '->_gc_thread_running._M_base._M_i', main_thread)
        major = mi.number(heap + '->_driver_major', main_thread)
        minor = mi.number(heap + '->_driver_minor', main_thread)
        workers = mi.number(heap + '->_runtime_workers._workers._created_workers._M_i', main_thread)
        passed = observed_stop and active == 1 and phase == 1 and running == 0 and major == 0 and minor == 0 and workers == 0
        emit('NATIVE_SHUTDOWN_STOP_BEFORE_TLS', executed=1, passed=passed, stop_entered=observed_stop,
             gc_running=running, major=major, minor=minor, runtime_workers=workers)
        if not passed:
            return 1
        mi.delete(tls)
        mi.resume(main_thread)
        event = mi.stop()
        if 'exit-code="0"' not in event and 'reason="exited-normally"' not in event:
            raise RuntimeError('Fixture did not complete: ' + event)
        return 0
    finally:
        try:
            mi.console('kill')
        except Exception:
            pass
        mi.process.terminate()
        mi.process.wait(timeout=10)
        mi.log.close()


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as error:
        emit('NATIVE_SHUTDOWN_HARNESS_ERROR', error=str(error))
        raise SystemExit(2)
