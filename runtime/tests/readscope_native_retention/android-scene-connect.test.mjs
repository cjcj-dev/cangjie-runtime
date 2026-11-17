#!/usr/bin/env zx
// Sealed replay of the formal scene functions. No LLDB/adb/native process runs.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { createHash } from 'node:crypto';

const bundle = path.join(path.dirname(new URL(import.meta.url).pathname), 'android-scene');
function functions() {
  const text = fs.readFileSync(process.env.SCENE_CONNECT_SOURCE || path.join(bundle, 'scene.mjs'), 'utf8');
  const end = text.indexOf('\nfunction readProc(');
  assert(end > 0);
  const definitions = text.slice(0, end).replace(/^#![^\n]*\n/, '').replace(/^import [^\n]+;\n/gm, '')
    .replace('const HERE = dirname(new URL(import.meta.url).pathname);', `const HERE = ${JSON.stringify(bundle)};`);
  const env = { HOST_SYMBOLS: '/symbols/consumer', ANDROID_PLATFORM_LOCAL_PORT: 'prior-value' };
  const api = vm.runInNewContext(definitions + '\n({drive, scripted, procDump, parseIdentity})', {
    join: path.join, createHash, readFileSync: fs.readFileSync, Date, process: { env },
    spawn: () => { throw Error('Native tools forbidden in connect replay'); },
  });
  return { ...api, env };
}
async function replay({ serial = 'emulator-5554', steps = {}, ioSetup } = {}) {
  const api = functions();
  const io = api.scripted({ platform: 'remote-android', serial: 'emulator-5554\tdevice', steps });
  const snapshots = [];
  const run = io.run.bind(io);
  io.run = async (step, argv, options) => {
    snapshots.push({ step, port: api.env.ANDROID_PLATFORM_LOCAL_PORT });
    return run(step, argv, options);
  };
  if (ioSetup) ioSetup(io, api);
  const result = await api.drive(io, '/sdk', serial);
  return { api, result, snapshots };
}

test('official loopback argv is replaced in the production drive, without a manual forward', async () => {
  // run43862034682 launch.argv contained this exact command and returned rc1.
  const officialCommand = 'platform connect connect://127.0.0.1:5039';
  const { result, snapshots, api } = await replay();
  const launch = result.log.find(row => row.step === 'launch');
  assert(launch, 'TARGET: production drive reaches the recorded launch boundary');
  assert(!launch.argv.includes(officialCommand), 'TARGET: old official loopback URL must be replaced');
  assert(launch.argv.includes('platform connect connect://emulator-5554:5039'));
  assert(!result.log.some(row => row.step === 'forward'), 'LLDB owns forward creation');
  assert.equal(snapshots.find(row => row.step === 'launch').port, '5039');
  assert(snapshots.filter(row => row.step !== 'launch').every(row => row.port === 'prior-value'));
  assert.equal(api.env.ANDROID_PLATFORM_LOCAL_PORT, 'prior-value');
  const probe = result.log.findIndex(row => row.step === 'cleanup-probe');
  const killed = result.log.findIndex(row => row.step === 'cleanup-server');
  const reaped = result.log.findIndex(row => row.step === 'reap-host');
  assert(probe >= 0 && probe < killed && killed < reaped, 'device identity and reap precede host adb reap');
  assert.equal(result.sceneSuccess, false);
  assert.equal(result.businessExit, null);
});

test('wrong device and loopback serials do not reach the launch boundary', async () => {
  for (const serial of ['127.0.0.1', 'localhost', 'emulator-9999', 'emulator-5554;invalid']) {
    const { result } = await replay({ serial });
    assert(!result.log.some(row => row.step === 'launch'), serial);
    assert.equal(result.sceneSuccess, false);
    assert.equal(result.businessExit, null);
  }
});

test('missing, reused and wrong-socket server identities never authorize a device kill', async () => {
  const { procDump } = functions();
  const cases = [
    ['gone', { rc: 0, stdout: 'DEVICE_PID=200\nDEVICE_START=99\nDEVICE_NOW=\nCMDLINE=\n', stderr: 'missing /proc/200' }],
    ['reused', { rc: 0, stdout: procDump('200', '100'), stderr: '' }],
    ['wrong socket', { rc: 0, stdout: procDump('200', '99', '1111', '2222'), stderr: '' }],
    ['wrong command', { rc: 0, stdout: procDump().replace('/lldb-server', '/other-server'), stderr: '' }],
  ];
  for (const [name, record] of cases) {
    const { result } = await replay({ steps: { 'cleanup-probe': record } });
    assert(!result.log.some(row => row.step === 'cleanup-server'), name);
    assert.equal(result.meta.cleanup, 'UNCONFIRMED', name);
    assert.equal(result.outcome, 'CLEANUP_UNCONFIRMED', name);
  }
  const { result } = await replay({ steps: { 'server-identity': { rc: 0, stdout: procDump('200', '99', '1111', '2222') } } });
  assert.equal(result.outcome, 'SERVER_UNCONFIRMED');
  assert(!result.log.some(row => row.step === 'launch' || row.step === 'cleanup-server'));
});

test('failed forward removal uses the actual list, not an unrelated listener', async () => {
  for (const [name, list, unconfirmed] of [
    ['gone', { rc: 0, stdout: '' }, false],
    ['other serial', { rc: 0, stdout: 'other-device tcp:5039 tcp:5039\n' }, false],
    ['still owned', { rc: 0, stdout: 'emulator-5554 tcp:5039 tcp:5039\n' }, true],
    ['unknown list', { rc: 1, stdout: '' }, true],
    ['timed-out list', { rc: null, timedOut: true, stdout: '' }, true],
  ]) {
    const { result } = await replay({ steps: {
      'cleanup-forward': { rc: 1, stdout: '', stderr: 'not found' }, 'cleanup-forward-list': list,
    } });
    assert(result.log.some(row => row.step === 'cleanup-forward-list'), name);
    assert.equal(result.meta.cleanup === 'UNCONFIRMED', unconfirmed, name);
    assert.equal(result.sceneSuccess, false);
  }
});

test('launch exception restores the LLDB-only environment and still checks device first', async () => {
  const { result, api } = await replay({ ioSetup(io) {
    const run = io.run.bind(io);
    io.run = (step, argv, options) => {
      if (step === 'launch') throw Error('sealed launch failure');
      return run(step, argv, options);
    };
  } });
  assert.equal(api.env.ANDROID_PLATFORM_LOCAL_PORT, 'prior-value');
  assert.equal(result.outcome, 'DRIVER_EXCEPTION');
  assert(result.log.findIndex(row => row.step === 'cleanup-server') < result.log.findIndex(row => row.step === 'reap-host'));
  assert.equal(result.businessExit, null);
});
