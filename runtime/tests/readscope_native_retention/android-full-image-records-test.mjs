#!/usr/bin/env zx
// Offline only: retained text + explicit synthetic transport controls. No adb,
// emulator, SDK, backend, application or runtime process is launched.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {classifyFullImageExit} from './android-exit-records.mjs';

const [out, evidence, counterexample] = process.argv.slice(2);
assert(out && evidence && counterexample, 'output directory, retained d51c directory, reviewer counterexample required');
fs.mkdirSync(out, {recursive: true});
const counter = JSON.parse(fs.readFileSync(counterexample, 'utf8'));
const read = name => fs.readFileSync(path.join(evidence, name), 'utf8');
const source = mode => {
  const old = counter.counter.find(row => row.mode === mode);
  assert.equal(old.oldAssertionsPass, true, 'independent review actually observed old acceptance');
  return {mode, stdout: read(mode + '.stdout'), stderr: read(mode + '.stderr'),
    logcat: '', crash: read('crash-log.stdout'), adbRc: old.row.rc, adbSignal: old.row.signal};
};
const oldPreinit = source('preinit'), oldFini = source('runtime-fini');
assert.equal(classifyFullImageExit(oldPreinit).internalFatal, true);
assert.equal(classifyFullImageExit(oldFini).internalFatal, false, 'original normal record remains a fatal-detection control');
// The old successful record has no shell wait/PID/window evidence. Preserve it
// unchanged as UNKNOWN, never claim newly observed native success retroactively.
assert.equal(classifyFullImageExit(oldFini).outcome, 'UNKNOWN');
const captures = {stderr: true, logcat: {rc: 0, signal: null}, crash: {rc: 0, signal: null}};
const shell = '\nEXIT_SHELL child_pid=2400 realtime_start=1763240464.000999999 realtime_end=1763240465.000999999 native_wait_rc=0\n';
const good = {...oldFini, stdout: oldFini.stdout + shell, captures};
const fatal = '1763240464.000 2400 2401 F libc : FORTIFY: pthread_mutex_lock called on a destroyed mutex (0x1234)\n';
const variants = [
  ['existing-preinit', oldPreinit, 'FAIL', 'unaltered retained record'],
  ['existing-fini-missing-status', oldFini, 'UNKNOWN', 'unaltered retained normal record; status not observed'],
  ['normal-fini-transport-control', good, 'PASS', 'retained normal text plus explicitly synthetic PID/window/wait/capture control'],
  ['same-pid-first-ms', {...good, logcat: fatal}, 'FAIL', 'synthetic large-epoch first-millisecond boundary'],
  ['same-pid-end-ms', {...good, logcat: fatal.replace('0464.000', '0465.000')}, 'FAIL', 'synthetic end-millisecond boundary'],
  ['other-pid', {...good, logcat: fatal.replaceAll('2400', '3400')}, 'PASS', 'synthetic independent PID'],
  ['before-window', {...good, logcat: fatal.replace('0464.000', '0463.999')}, 'PASS', 'synthetic independent earlier window'],
  ['after-window', {...good, logcat: fatal.replace('0464.000', '0465.001')}, 'PASS', 'synthetic independent later window'],
  ['missing-wait', {...good, stdout: good.stdout.replace('native_wait_rc=0', 'missing_wait=0')}, 'UNKNOWN', 'synthetic missing native status'],
  ['duplicate-identity', {...good, stdout: good.stdout + shell}, 'UNKNOWN', 'synthetic ambiguous child'],
  ['backwards-window', {...good, stdout: good.stdout.replace('realtime_end=1763240465.000999999', 'realtime_end=1763240464.000000000')}, 'UNKNOWN', 'synthetic invalid range'],
  ['missing-logcat-capture', {...good, captures: {...captures, logcat: undefined}}, 'UNKNOWN', 'synthetic missing capture status'],
  ['failed-crash-capture', {...good, captures: {...captures, crash: {rc: 1, signal: null}}}, 'UNKNOWN', 'synthetic incomplete capture'],
  ['missing-stderr', {...good, stderr: undefined}, 'UNKNOWN', 'synthetic missing record'],
  ['same-pid-crash', {...good, crash: fatal}, 'FAIL', 'synthetic separate crash-buffer fatal'],
  ['own-generic-fatal', {...good, stderr: good.stderr + '\nfatal: native loader error\n'}, 'FAIL', 'synthetic invocation-owned fatal severity'],
  ['unattributed-fatal', {...good, logcat: 'FORTIFY: PID/timestamp missing\n'}, 'UNKNOWN', 'synthetic missing attribution'],
  ['native-nonzero-adb-zero', {...good, stdout: good.stdout.replace('native_wait_rc=0', 'native_wait_rc=134')}, 'FAIL', 'synthetic native status independent of adb'],
  ['adb-interrupted', {...good, adbSignal: 'SIGTERM'}, 'UNKNOWN', 'synthetic transport interruption; not native signal'],
  ['target-red', {...good, stdout: good.stdout.replace('elf-published-package executed=1 pass=1', 'elf-published-package executed=1 pass=0')}, 'FAIL', 'synthetic actual invariant red'],
  ['missing-target', {...good, stdout: good.stdout.replace('TARGET elf-published-package executed=1 pass=1\n', '')}, 'UNKNOWN', 'synthetic target not reached'],
  ['fatal-with-missing-status', {...oldPreinit, captures: undefined}, 'FAIL', 'unaltered known fatal not erased by missing fields'],
];
const rows = [];
const entry = path.join(path.dirname(fileURLToPath(import.meta.url)), 'android.mjs');
for (const [name, input, expected, provenance] of variants) {
  const classified = classifyFullImageExit(input);
  assert.equal(classified.outcome, expected, name);
  assert.equal(classified.pass, expected === 'PASS', name);
  const request = path.join(out, name + '.input.json'), result = path.join(out, name + '.result.json');
  fs.writeFileSync(request, JSON.stringify([input], null, 2));
  const args = [entry, '--replay-full-image-records', request, result];
  const started = new Date().toISOString();
  const r = spawnSync(process.execPath, args, {encoding: 'utf8', timeout: 10000});
  fs.writeFileSync(path.join(out, name + '.stdout'), r.stdout ?? '');
  fs.writeFileSync(path.join(out, name + '.stderr'), r.stderr ?? '');
  rows.push({name, command: process.execPath, args, started, ended: new Date().toISOString(),
    rc: r.status, signal: r.signal, error: r.error?.message ?? null, expected, provenance});
  fs.writeFileSync(path.join(out, 'commands.json'), JSON.stringify(rows, null, 2));
  assert.equal(r.status, expected === 'PASS' ? 0 : 1, name + ' formal entry success exit');
  assert.equal(r.signal, null, name);
  const saved = JSON.parse(fs.readFileSync(result, 'utf8'))[0];
  assert.equal(saved.outcome, expected, name + ' saved before rejection');
  if (expected !== 'PASS') assert(r.stderr.includes('ANDROID_FULL_IMAGE_EXIT_' + expected), name + ' exact gate');
}
console.log(JSON.stringify({offlineCases: rows.length, productExecutions: 0, deviceExecutions: 0,
  existingFatalRejected: true, originalNormalRecordNotPromoted: true, formalEntryExitsChecked: true}));
