#!/usr/bin/env zx
// Real drive -> generated signal shell, sealed /proc and kill/sleep functions.
// No adb, server, LLDB, native consumer or real signal is executed.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import vm from 'node:vm';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';

const bundle = path.join(path.dirname(new URL(import.meta.url).pathname), 'android-scene');
const fixtureRoot = process.env.SCENE_OWNER_FIXTURES || fs.mkdtempSync(path.join(os.tmpdir(), 'readscope-owner-'));
function production() {
  const source = fs.readFileSync(process.env.SCENE_OWNER_SOURCE || path.join(bundle, 'scene.mjs'), 'utf8');
  const end = source.indexOf('\nfunction readProc(');
  assert(end > 0);
  const definitions = source.slice(0, end).replace(/^#![^\n]*\n/, '').replace(/^import [^\n]+;\n/gm, '')
    .replace('const HERE = dirname(new URL(import.meta.url).pathname);', `const HERE = ${JSON.stringify(bundle)};`);
  return vm.runInNewContext(definitions + '\n({drive, scripted, procDump})', {
    join: path.join, createHash, readFileSync: fs.readFileSync, Date,
    process: { env: { HOST_SYMBOLS: '/symbols/consumer' } },
    spawn: () => { throw Error('Native tools forbidden in owner replay'); },
  });
}
function signalShell(shell, mode) {
  fs.mkdirSync(fixtureRoot, { recursive: true });
  const dir = fs.mkdtempSync(path.join(fixtureRoot, mode + '-'));
  assert(/^[A-Za-z0-9_./-]+$/.test(dir));
  const proc = path.join(dir, 'proc'), pid = path.join(proc, '200');
  fs.mkdirSync(path.join(pid, 'fd'), { recursive: true });
  fs.mkdirSync(path.join(proc, 'net'), { recursive: true });
  function stat(start) {
    const fields = Array(22).fill('0'); fields[0] = '200'; fields[1] = '(lldb-server)'; fields[2] = 'S'; fields[21] = start;
    return fields.join(' ') + '\n';
  }
  fs.writeFileSync(path.join(pid, 'stat'), stat(mode === 'reuse-before-signal' ? '100' : '99'));
  fs.writeFileSync(path.join(pid, 'cmdline'), '/data/local/tmp/readscope-lifetime/lldb-server\0platform\0--listen\0*:5039\0');
  fs.symlinkSync('socket:[5555]', path.join(pid, 'fd', '3'));
  fs.writeFileSync(path.join(proc, 'net', 'tcp'), '1: 00000000:13AF 00000000:0000 0A 0 0 0 2000 0 5555 1\n');
  fs.writeFileSync(path.join(proc, 'net', 'tcp6'), '');
  const events = path.join(dir, 'events');
  const mutation = mode === 'gone-after-signal' ? `rm -r ${pid};` :
    mode === 'reuse-after-signal' ? `printf '%s\\n' '${stat('100').trim()}' > ${pid}/stat;` : '';
  const prelude = `kill() { printf 'SIGNAL %s\\n' "$*" >> ${events}; ${mutation} return ${mode === 'signal-failure' ? 1 : 0}; }\n` +
    `sleep() { printf 'WAIT\\n' >> ${events}; }\n`;
  const body = shell.replaceAll('/proc', proc);
  assert(!/\/proc(?:\/|\$)/.test(body.replaceAll(proc, '/sealed')));
  const argv = ['/bin/bash', '-c', prelude + body];
  const r = spawnSync(argv[0], argv.slice(1), { encoding: 'utf8', timeout: 5000,
    env: { PATH: '/usr/bin:/bin', LANG: 'C' } });
  assert.equal(r.error, undefined);
  const result = { mode, rc: r.status, signal: r.signal, stdout: r.stdout, stderr: r.stderr,
    events: fs.existsSync(events) ? fs.readFileSync(events, 'utf8') : '', originalShell: shell };
  fs.writeFileSync(path.join(dir, 'result.json'), JSON.stringify(result, null, 2) + '\n');
  return result;
}
async function replay(mode) {
  const api = production();
  const io = api.scripted({ platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: {} });
  const run = io.run.bind(io);
  let shell;
  io.run = async (step, argv, options) => {
    if (step !== 'cleanup-server') return run(step, argv, options);
    // Probe was still 200/start99; only the second, signaling shell sees start100.
    shell = signalShell(argv.at(-1), mode);
    return { rc: shell.rc, stdout: shell.stdout, stderr: shell.stderr };
  };
  const result = await api.drive(io, '/sdk', 'emulator-5554');
  assert(shell, 'same-owner probe must reach the production cleanup-server shell');
  assert(result.log.findIndex(x => x.step === 'cleanup-probe') < result.log.findIndex(x => x.step === 'cleanup-server'));
  assert(result.log.findIndex(x => x.step === 'cleanup-server') < result.log.findIndex(x => x.step === 'reap-host'));
  assert.equal(result.businessExit, null);
  assert.equal(result.sceneSuccess, false);
  return { result, shell };
}

test('probe-to-signal PID reuse is rejected by the production signal shell', async () => {
  const { result, shell } = await replay('reuse-before-signal');
  assert.equal(shell.events, '', 'TARGET: device cleanup signal must refuse starttime changed after the probe');
  assert.equal(shell.rc, 3);
  assert.equal(shell.stdout, 'OWNER_MISMATCH\n');
  assert.equal(result.meta.cleanup, 'UNCONFIRMED');
  assert.equal(result.outcome, 'CLEANUP_UNCONFIRMED');
});

test('same owner reaches TERM and all bounded waits without claiming a reap', async () => {
  const { result, shell } = await replay('same-owner');
  assert.equal(shell.events.split('\n').filter(x => x === 'SIGNAL -TERM 200').length, 1);
  assert.equal(shell.events.split('\n').filter(x => x === 'WAIT').length, 25);
  assert.equal(shell.rc, 4);
  assert.equal(shell.stdout, 'NOT_REAPED\n');
  assert(!shell.originalShell.includes('echo REAPED'));
  assert.equal(result.meta.cleanup, 'UNCONFIRMED');
});

test('signal failure and post-signal disappearance or reuse never become server REAPED', async () => {
  for (const [mode, stdout] of [
    ['signal-failure', 'OWNER_ABSENT\n'],
    ['gone-after-signal', 'OWNER_ABSENT\n'],
    ['reuse-after-signal', 'OWNER_MISMATCH\n'],
  ]) {
    const { result, shell } = await replay(mode);
    assert.equal(shell.events, 'SIGNAL -TERM 200\n', mode);
    assert.equal(shell.rc, 3, mode);
    assert.equal(shell.stdout, stdout, mode);
    assert(!/\bREAPED\b/.test(shell.stdout), mode);
    assert.equal(result.meta.cleanup, 'UNCONFIRMED', mode);
    assert.equal(result.outcome, 'CLEANUP_UNCONFIRMED', mode);
  }
});
