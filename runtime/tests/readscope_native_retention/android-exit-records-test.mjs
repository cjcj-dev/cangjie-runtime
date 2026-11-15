#!/usr/bin/env zx
import assert from 'node:assert/strict';
import {classifyExit} from './android-exit-records.mjs';
const input = {stdout: 'EXIT_EVENT pid=1456 realtime=100.000000000 name=start result=0\nEXIT_RUNTIME pid=1456 path=/arm/libcangjie-runtime.so\nEXIT_EVENT pid=1456 realtime=101.000000000 name=init result=0\nEXIT_EVENT pid=1456 realtime=102.000000000 name=return result=0\nEXIT_SHELL native_wait_rc=0\n', stderr: '', logcat: '', adbRc: 0, adbSignal: null, mode: 'return', remoteRuntime: '/arm/libcangjie-runtime.so'};
assert(classifyExit(input).pass);
// Match the real d51c failure shape despite adb's zero exit status.
const fatal = '103.000 1456 1624 F libc : FORTIFY: pthread_mutex_lock called on a destroyed mutex (0x7b33ef1eb08c)\n103.001 1456 1624 F libc : Fatal signal 6 (SIGABRT), code -1 (SI_QUEUE) in tid 1624 (schd-worker), pid 1456 (consumer)\n';
assert(!classifyExit({...input, logcat: fatal}).pass);
assert(classifyExit({...input, logcat: fatal}).internalFatal);
assert(classifyExit({...input, logcat: fatal.replaceAll('1456', '2456')}).pass);
assert(classifyExit({...input, logcat: fatal.replaceAll('103.', '099.')}).pass);
assert(!classifyExit({...input, stdout: ''}).pass);
assert(!classifyExit({...input, mode: 'fini'}).pass);
assert(!classifyExit({...input, adbRc: null, adbSignal: 'SIGTERM'}).pass);
assert(!classifyExit({...input, stdout: input.stdout.replace('native_wait_rc=0', 'native_wait_rc=134')}).pass);
assert(!classifyExit({...input, remoteRuntime: '/other/libcangjie-runtime.so'}).pass);
console.log('Android exit record controls: pass (pure synthetic records, no Android/model execution)');
