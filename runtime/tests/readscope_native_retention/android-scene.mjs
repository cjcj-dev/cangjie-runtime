#!/usr/bin/env zx
// Official-runner preparation only; debugger implementation belongs to Grok.
// No import of scene.mjs/capture.py: checking this file cannot start a debugger.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { spawn, spawnSync } from 'node:child_process';

const here = path.dirname(fileURLToPath(import.meta.url));
const bundle = path.join(here, 'android-scene');
const fixed = JSON.parse(fs.readFileSync(path.join(bundle, 'input.json')));
const origin = JSON.parse(fs.readFileSync(path.join(bundle, 'origin.json')));
const jobInterface = JSON.parse(fs.readFileSync(path.join(bundle, 'job-interface.json')));
const sha = p => createHash('sha256').update(fs.readFileSync(p)).digest('hex');
const json = (p, value) => fs.writeFileSync(p, JSON.stringify(value, null, 2) + '\n');

function interfaceBindings(out, sdk, deadlineMs = null) {
  return {
    SCENE_COMMANDS: path.join(out, 'scene-commands.json'),
    HOST_SYMBOLS: path.join(out, 'consumer'),
    RUNTIME_SO: path.join(path.dirname(out), 'readscope-android-scene-artifact',
      'x86_64/install/runtime/lib/linux_android31_x86_64_cjnative/libcangjie-runtime.so'),
    ANDROID_SDK_ROOT: sdk,
    ANDROID_SERIAL: 'emulator-5554',
    SCENE_DEADLINE_MS: deadlineMs === null ? '$COLLECTION_T0_PLUS_270000' : String(deadlineMs),
  };
}

// One call site supplies both the executable route and the absolute deadlines.
// No tool is started by constructing or checking this launch description.
export function sceneLaunch(out, sdk, env, sceneStarted) {
  assert(Number.isSafeInteger(sceneStarted));
  const hardDeadline = sceneStarted + fixed.totalSeconds * 1000;
  const bindings = interfaceBindings(out, sdk, hardDeadline - 30000);
  const sceneEnv = { ...env, ...bindings, SCENE_EXECUTE: '1', EXIT_SCENE_OUT: path.join(bundle, 'run') };
  delete sceneEnv.SCENE_OBSERVER;
  return { argv: [process.execPath, path.join(bundle, 'scene.mjs'), 'execute'],
    bindings, env: sceneEnv, sceneStarted, hardDeadline,
    limits: { naturalUntilMs: hardDeadline - 25000, deadlineMs: hardDeadline - 15000,
      termGraceMs: 5000, killGraceMs: 5000 } };
}

export function runtimeIdentity(so, notes) {
  const stat = fs.lstatSync(so);
  assert(stat.isFile() && !stat.isSymbolicLink(), 'RUNTIME_SO must be the installed artifact ELF');
  assert.equal(fs.realpathSync(so), so, 'RUNTIME_SO has no indirect alias');
  const bytes = fs.readFileSync(so);
  assert.equal(bytes.subarray(0, 4).toString('hex'), '7f454c46');
  assert.equal(bytes[4], 2, 'runtime ELF64');
  assert.equal(bytes[5], 1, 'runtime little endian');
  assert.equal(bytes.readUInt16LE(18), 62, 'runtime x86_64');
  assert.equal(sha(so), fixed.runtimeSha256, 'original installed runtime; no fallback rebuild');
  const ids = [...notes.matchAll(/^\s*Build ID:\s*([0-9a-fA-F]+)\s*$/gm)];
  assert(ids.length <= 1, 'ambiguous runtime Build ID');
  const buildId = ids[0]?.[1].toLowerCase() ?? null;
  assert(!buildId || (buildId.length % 2 === 0), 'invalid runtime Build ID');
  return { path: so, sha256: fixed.runtimeSha256, buildId,
    buildIdStatus: buildId ? 'PRESENT' : 'ABSENT',
    artifact: fixed.artifact, source: fixed.runtime,
    deviceIdentity: 'UNKNOWN; no target execution or module observation' };
}

function checkBundle() {
  for (const [name, entry] of Object.entries(origin.files)) {
    assert.equal(sha(path.join(bundle, name)), entry.sha256, `Grok file changed: ${name}`);
  }
  assert.equal(sha(path.join(bundle, 'job-interface.json')), origin.jobInterface.sha256);
  const plannedBindings = interfaceBindings('$RUNNER_TEMP/readscope-android-scene', '$ANDROID_HOME');
  assert.deepEqual(Object.keys(plannedBindings).sort(), [...jobInterface.requiredEnv].sort());
  assert(!('SCENE_OBSERVER' in plannedBindings), 'formal shell observer is not a diagnostic dependency');
  assert.equal(jobInterface.readyForNativeExecution, false);
  assert.equal(fixed.mode, 'preinit');
  assert.equal(fixed.nativeProcesses, 1);
  assert.equal(fixed.runtime, 'd51c34a06cfd980d0e2694514e2a41421ac41b83');
  const pendingBindings = origin.blockers.filter(row =>
    ['TARGET_REAP_TOKEN_AND_RC'].includes(row.id));
  return { state: 'PREPARED_NOT_EXECUTED', fixed, origin, jobInterface,
    plannedBindings, pendingBindings,
    interfaceStatus: pendingBindings.length ? 'INCOMPLETE_HOLD' : 'BOUND_NOT_EXECUTED',
    diagnosticContract: { resultSource: 'same-PID SBProcess state/ExitStatus/StopReason',
      shellWait: 'UNKNOWN', lldbRcIsBusinessRc: false, syntheticExitShell: false,
      deviceRuntimeIdentity: 'UNKNOWN; actual module identity must be verified before qualification',
      controlledDetachBusinessExit: 'UNKNOWN',
      targetCleanup: 'Read Grok same-instance cleanup result separately; missing identity or unconfirmed disappearance remains UNKNOWN' },
    nativeRuns: 0 };
}

function requireReady() {
  const checked = checkBundle();
  assert.equal(checked.pendingBindings.length, 0,
    `GROK_INTERFACE_INCOMPLETE: ${checked.pendingBindings.map(row => row.id).join(',')}`);
  // Byte-identical Grok package is bound, but final entry/independent review
  // and separate controller authorization are required before native execution.
  assert.equal(origin.readyForNativeExecution, true,
    `GROK_ENTRY_NOT_READY: ${origin.blockers.map(x => x.id).join(',')}`);
}

function context() {
  requireReady();
  assert.equal(process.env.GITHUB_ACTIONS, 'true', 'official runner only');
  assert.equal(process.env.RUNNER_OS, 'Linux');
  assert.equal(process.env.SCENE_ANDROID, 'true');
  for (const key of ['SCENE_OLD_ANDROID', 'SCENE_OLD_ARM64', 'SCENE_OLD_COMPARE']) {
    assert.equal(process.env[key], 'false', `conflicting workflow input: ${key}`);
  }
  const out = path.join(process.env.RUNNER_TEMP, 'readscope-android-scene');
  const sdk = process.env.ANDROID_HOME;
  assert(sdk && path.isAbsolute(sdk));
  const androidUser = path.join(out, '.android');
  const env = { ...process.env, ANDROID_SDK_ROOT: sdk,
    ANDROID_SDK_HOME: out, ANDROID_USER_HOME: androidUser,
    ANDROID_EMULATOR_HOME: androidUser, ANDROID_AVD_HOME: path.join(androidUser, 'avd'),
    CMAKE_BUILD_PARALLEL_LEVEL: '2', CANGJIE_BUILD_JOBS: '2', TMPDIR: out };
  return { out, sdk, env };
}

function runner(out, env) {
  const rows = [];
  const run = (name, args, options = {}) => {
    assert(!rows.some(x => x.name === name), `duplicate command ${name}`);
    const row = { name, args, cwd: out, started: new Date().toISOString() };
    rows.push(row);
    const stdout = fs.openSync(path.join(out, `${name}.stdout`), 'wx');
    const stderr = fs.openSync(path.join(out, `${name}.stderr`), 'wx');
    const result = spawnSync(args[0], args.slice(1), { cwd: options.cwd || out, env,
      input: options.input, stdio: [options.input === undefined ? 'ignore' : 'pipe', stdout, stderr],
      timeout: options.timeout ?? 300000 });
    fs.closeSync(stdout); fs.closeSync(stderr);
    Object.assign(row, { rc: result.status, signal: result.signal,
      error: result.error?.message, ended: new Date().toISOString() });
    json(path.join(out, 'commands.json'), rows);
    if (!options.allowFailure) {
      assert.equal(row.rc, 0, name); assert.equal(row.signal, null, name);
    }
    return fs.readFileSync(path.join(out, `${name}.stdout`), 'utf8');
  };
  return { run, rows };
}

// Own only the ChildProcess created at this call site. 'exit' is not 'close':
// pipes may still be open, and sending a signal is not proof of a reap.
export function ownChild(child) {
  const owned = { child, exit: null, close: null, error: null, signals: [] };
  owned.closed = new Promise(resolve => {
    child.once('error', error => { owned.error = error.message; });
    child.once('exit', (rc, signal) => { owned.exit = { rc, signal }; });
    child.once('close', (rc, signal) => {
      owned.close = { rc, signal, at: Date.now() };
      resolve(owned.close);
    });
  });
  return owned;
}

async function waitClosedUntil(owned, deadlineMs) {
  if (owned.close) return owned.close;
  const remaining = deadlineMs - Date.now();
  if (remaining <= 0) return null;
  let timer;
  try {
    return await Promise.race([owned.closed,
      new Promise(resolve => { timer = setTimeout(() => resolve(null), remaining); })]);
  } finally { clearTimeout(timer); }
}

export async function settleOwnedChild(owned, {
  naturalUntilMs, deadlineMs, termGraceMs = 1000, killGraceMs = 1000,
}) {
  assert(naturalUntilMs <= deadlineMs);
  const natural = await waitClosedUntil(owned, naturalUntilMs);
  const signal = name => {
    // Never signal an exited process merely because its pipes have not closed.
    if (owned.close || owned.exit || !Number.isInteger(owned.child.pid) || Date.now() >= deadlineMs) return;
    const row = { signal: name, at: Date.now() };
    try { row.accepted = owned.child.kill(name); } catch (error) { row.error = error.message; }
    owned.signals.push(row);
  };
  if (!natural && !owned.close) {
    signal('SIGTERM');
    await waitClosedUntil(owned, Math.min(Date.now() + termGraceMs, deadlineMs - killGraceMs));
    if (!owned.close) {
      signal('SIGKILL');
      await waitClosedUntil(owned, deadlineMs);
    }
  }
  return { pid: owned.child.pid ?? null, exit: owned.exit, close: owned.close,
    error: owned.error, signals: owned.signals, timedOut: !natural,
    reaped: owned.close !== null, deadlineMs, observedAt: Date.now() };
}

export function asyncRunner(out, env) {
  const rows = [];
  return async (name, args, limits) => {
    assert(!rows.some(row => row.name === name), `duplicate command ${name}`);
    const row = { name, args, cwd: out, started: new Date().toISOString() };
    rows.push(row);
    if (Date.now() >= limits.deadlineMs) {
      Object.assign(row, { skipped: true, reason: 'DEADLINE', rc: null, signal: null, reaped: false });
      json(path.join(out, 'commands.json'), rows);
      return row;
    }
    const stdout = fs.openSync(path.join(out, `${name}.stdout`), 'wx');
    const stderr = fs.openSync(path.join(out, `${name}.stderr`), 'wx');
    try {
      const owned = ownChild(spawn(args[0], args.slice(1), { cwd: out, env,
        stdio: ['ignore', stdout, stderr] }));
      Object.assign(row, await settleOwnedChild(owned, limits));
      row.rc = row.close?.rc ?? null;
      row.signal = row.close?.signal ?? null;
    } catch (error) { row.error = error.stack; }
    finally {
      fs.closeSync(stdout); fs.closeSync(stderr);
      row.ended = new Date().toISOString();
      json(path.join(out, 'commands.json'), rows);
    }
    return row;
  };
}

function files(root) {
  return fs.readdirSync(root, { withFileTypes: true }).flatMap(entry => {
    const name = path.join(root, entry.name);
    assert(!entry.isSymbolicLink(), `unexpected artifact/source symlink: ${name}`);
    return entry.isDirectory() ? files(name) : [name];
  });
}

function prepareBoundsDirectory(bounds) {
  try {
    fs.mkdirSync(bounds);
  } catch (error) {
    if (error.code !== 'EEXIST') throw error;
  }
  const entry = fs.lstatSync(bounds);
  assert(entry.isDirectory() && !entry.isSymbolicLink(), 'fixed bounds dependency must be a real directory');
  assert.equal(fs.readdirSync(bounds).length, 0, 'fresh fixed dependency only');
}

function prepare() {
  const { out, sdk, env } = context();
  fs.mkdirSync(out); // Existing/partial output is never accepted as a cache.
  const diskPath = fs.realpathSync(process.env.RUNNER_TEMP);
  const disk = fs.statfsSync(diskPath, { bigint: true });
  const availableBytes = disk.bavail * disk.bsize;
  const requiredBytes = 16n * 1024n ** 3n;
  json(path.join(out, 'disk-admission.json'), { path: diskPath,
    availableBytes: String(availableBytes), requiredBytes: String(requiredBytes),
    observedAt: new Date().toISOString() });
  assert(availableBytes >= requiredBytes, 'SCENE_PREPARE_INSUFFICIENT_DISK');
  const { run, rows } = runner(out, env);
  json(path.join(out, 'input.json'), checkBundle());
  const checkout = path.resolve(here, '../../..');
  const source = path.join(out, 'fixed-runtime');
  run('fixed-fetch', ['git', '-C', checkout, 'fetch', '--no-tags', 'origin', fixed.runtime]);
  run('fixed-worktree', ['git', '-C', checkout, 'worktree', 'add', '--detach', source, fixed.runtime]);
  assert.equal(run('fixed-source', ['git', '-C', source, 'rev-parse', 'HEAD']).trim(), fixed.runtime);
  const runtime = path.join(source, 'runtime');
  const test = path.join(runtime, 'tests/readscope_native_retention');
  for (const [file, expected] of [['elf-fixture.cpp', fixed.fixtureBlob], ['elf-consumer.cpp', fixed.consumerBlob]]) {
    assert.equal(run(`blob-${file}`, ['git', '-C', source, 'rev-parse', `${fixed.runtime}:runtime/tests/readscope_native_retention/${file}`]).trim(), expected);
  }
  const bounds = path.join(runtime, 'third_party/third_party_bounds_checking_function');
  prepareBoundsDirectory(bounds);
  run('bounds-init', ['git', 'init', bounds]);
  run('bounds-remote', ['git', '-C', bounds, 'remote', 'add', 'origin',
    'https://gitcode.com/openharmony/third_party_bounds_checking_function.git']);
  run('bounds-fetch', ['git', '-C', bounds, 'fetch', '--depth', '1', 'origin', fixed.bounds]);
  run('bounds-checkout', ['git', '-C', bounds, 'checkout', '--detach', fixed.bounds]);
  const artifact = path.join(process.env.RUNNER_TEMP, 'readscope-android-scene-artifact');
  files(artifact); // Reject symlinks before selecting any executable input.
  const identity = JSON.parse(fs.readFileSync(path.join(artifact, 'identity.json')));
  assert.equal(identity.ref, fixed.runtime);
  assert.equal(identity.ndkVersion, fixed.ndk);
  assert.equal(identity.built.find(x => x.arch === 'x86_64').runtime.sha256, fixed.runtimeSha256);
  const lib = path.join(artifact, 'x86_64/install/runtime/lib/linux_android31_x86_64_cjnative');
  const so = path.join(lib, 'libcangjie-runtime.so');
  assert.equal(sha(so), fixed.runtimeSha256, 'use original normal installed SO; no fallback rebuild');
  run('sdk-packages', [path.join(sdk, 'cmdline-tools/latest/bin/sdkmanager'), '--install',
    `ndk;${fixed.ndk}`, 'platform-tools', 'emulator', fixed.systemImage],
  { input: 'y\n'.repeat(8), timeout: 1200000 });
  const ndk = path.join(sdk, 'ndk', fixed.ndk);
  assert(fs.readFileSync(path.join(ndk, 'source.properties'), 'utf8').includes(`Pkg.Revision = ${fixed.ndk}`));
  const tools = path.join(ndk, 'toolchains/llvm/prebuilt/linux-x86_64/bin');
  run('compiler-version', [path.join(tools, 'clang'), '--version']);
  const debuggerTools = {
    host: path.join(tools, 'lldb'),
    wrapper: path.join(tools, 'lldb.sh'),
    server: path.join(ndk, 'toolchains/llvm/prebuilt/linux-x86_64/lib/clang/18/lib/linux/x86_64/lldb-server'),
  };
  for (const key of ['host', 'server']) {
    const bytes = fs.readFileSync(debuggerTools[key]);
    assert.equal(bytes.subarray(0, 4).toString('hex'), '7f454c46', `${key} ELF`);
    assert.equal(bytes[4], 2, `${key} ELF64`);
    assert.equal(bytes[5], 1, `${key} little endian`);
    assert.equal(bytes.readUInt16LE(18), 62, `${key} x86_64`);
    run(`${key}-elf`, [path.join(tools, 'llvm-readelf'), '-hW', debuggerTools[key]]);
  }
  assert(fs.statSync(debuggerTools.wrapper).isFile());
  run('host-lldb-dependencies', ['ldd', debuggerTools.host]);
  json(path.join(out, 'debugger-tools.json'), { paths: debuggerTools,
    hashes: Object.fromEntries(Object.entries(debuggerTools).map(([k, p]) => [k, sha(p)])),
    actualVersions: 'NOT_RUN; Grok driver must read before native launch',
    remoteAndroid: 'NOT_VERIFIED; only Grok driver may verify platform list',
    requiredPlatform: 'remote-android', noAlternateDebugger: true });
  // The original staging log.h is a byte copy, not a generated configuration:
  // fixed CJThread/src/base/log/CMakeLists.txt: file(COPY include/log.h ...).
  const headers = path.join(out, 'runtime-staging/include');
  fs.mkdirSync(headers, { recursive: true });
  run('normal-log-header-copy', ['cmake', '-E', 'copy',
    path.join(runtime, 'src/CJThread/src/base/log/include/log.h'), path.join(headers, 'log.h')]);
  assert.equal(sha(path.join(headers, 'log.h')), sha(path.join(runtime, 'src/CJThread/src/base/log/include/log.h')));
  const includes = ['src', 'include', 'src/Loader/BinaryFile',
    'src/CJThread/src/runtime/schedule/include', 'third_party/third_party_bounds_checking_function/include']
    .map(p => path.join(runtime, p)).concat(headers);
  const common = [path.join(tools, 'x86_64-linux-android31-clang++'), '-std=gnu++17', '-O2', ...includes.map(p => `-I${p}`)];
  run('fixture-link', [...common, '-shared', '-fPIC', '-Wl,-Bsymbolic-functions',
    path.join(test, 'elf-fixture.cpp'), `-L${lib}`, '-lcangjie-runtime', '-ldl', '-o', path.join(out, 'fixture.so')]);
  run('consumer-link', [...common, path.join(test, 'elf-consumer.cpp'),
    `-L${lib}`, '-lcangjie-runtime', '-ldl', '-o', path.join(out, 'consumer')]);
  run('fixture-identity', [path.join(tools, 'llvm-readelf'), '-hndW', path.join(out, 'fixture.so')]);
  run('consumer-identity', [path.join(tools, 'llvm-readelf'), '-hndW', path.join(out, 'consumer')]);
  assert.equal(so, interfaceBindings(out, sdk).RUNTIME_SO);
  const runtimeNotes = run('runtime-identity', [path.join(tools, 'llvm-readelf'), '-hndW', so]);
  const runtimeInput = runtimeIdentity(so, runtimeNotes);
  json(path.join(out, 'module-inputs.json'), {
    runtimeSource: fixed.runtime, runtime: runtimeInput,
    fixture: { path: path.join(out, 'fixture.so'), sha256: sha(path.join(out, 'fixture.so')), originalElf: 'NOT_SAVED' },
    consumer: { path: path.join(out, 'consumer'), sha256: sha(path.join(out, 'consumer')), originalElf: 'NOT_SAVED' },
    sourceBlobs: { fixture: fixed.fixtureBlob, consumer: fixed.consumerBlob },
    relinkOnly: true, runtimeRebuilt: false, mode: 'preinit', originalFlags: true,
    dynamicIdentity: 'NOT_RUN; must be captured by Grok in actual target', nativeRuns: 0,
  });
  // Route record only, never an extra launch. Grok owns the actual call below.
  rows.push({ name: 'preinit', executed: false, args: [path.join(sdk, 'platform-tools/adb'),
    '-s', 'emulator-5554', 'shell', 'env', 'LD_LIBRARY_PATH=/data/local/tmp/readscope-lifetime',
    '/data/local/tmp/readscope-lifetime/consumer', '/data/local/tmp/readscope-lifetime/fixture.so', 'preinit'] });
  json(path.join(out, 'scene-commands.json'), rows);
}

async function execute() {
  const { out, sdk, env } = context();
  const { run } = runner(path.join(out, 'execution'), env);
  const bindings = interfaceBindings(out, sdk);
  const inputs = JSON.parse(fs.readFileSync(path.join(out, 'module-inputs.json')));
  const readelf = path.join(sdk, 'ndk', fixed.ndk, 'toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf');
  const runtimeInput = runtimeIdentity(bindings.RUNTIME_SO,
    run('runtime-before-scene', [readelf, '-nW', bindings.RUNTIME_SO]));
  assert.equal(inputs.runtime.path, bindings.RUNTIME_SO);
  assert.equal(inputs.runtime.sha256, runtimeInput.sha256);
  assert.equal(inputs.runtime.buildId, runtimeInput.buildId);
  assert.notEqual(bindings.RUNTIME_SO, bindings.HOST_SYMBOLS);
  // The fixed original SO has no GNU Build ID. Absence is recorded, not a
  // rejection: Grok must correlate the actual same-PID module/maps with the
  // deployed file's identity. A derived LLDB UUID is not a GNU Build ID.
  // No self-test/calibrate here: Grok self-test assumes official tools absent.
  const adb = path.join(sdk, 'platform-tools/adb');
  fs.mkdirSync(env.ANDROID_AVD_HOME, { recursive: true });
  run('create-avd', [path.join(sdk, 'cmdline-tools/latest/bin/avdmanager'), 'create', 'avd',
    '--name', 'readscope-scene', '--path', path.join(env.ANDROID_AVD_HOME, 'readscope-scene.avd'),
    '--package', fixed.systemImage, '--device', 'pixel_2'], { input: 'no\n' });
  const emulatorTool = path.join(sdk, 'emulator/emulator');
  run('avd-manager-list', [path.join(sdk, 'cmdline-tools/latest/bin/avdmanager'), 'list', 'avd']);
  assert(run('emulator-avd-list', [emulatorTool, '-list-avds']).split(/\r?\n/).includes('readscope-scene'));
  const stdout = fs.openSync(path.join(out, 'emulator.stdout'), 'wx');
  const stderr = fs.openSync(path.join(out, 'emulator.stderr'), 'wx');
  const emulator = spawn(emulatorTool, ['-avd', 'readscope-scene', '-port', '5554', '-no-window',
    '-no-audio', '-no-boot-anim', '-no-snapshot', '-gpu', 'swiftshader_indirect', '-memory', '2048'],
  { env, stdio: ['ignore', stdout, stderr] });
  const emulatorOwned = ownChild(emulator);
  let sceneStarted = null;
  let primaryError = null;
  let emulatorResult = null;
  const cleanupResults = [];
  let cleanupError = null;
  try {
    run('wait-device', [adb, '-s', 'emulator-5554', 'wait-for-device'], { timeout: 180000 });
    const deadline = Date.now() + fixed.bootSeconds * 1000;
    let booted = false;
    while (Date.now() < deadline) {
      const r = spawnSync(adb, ['-s', 'emulator-5554', 'shell', 'getprop', 'sys.boot_completed'], { env, encoding: 'utf8', timeout: 5000 });
      if (r.status === 0 && r.stdout.trim() === '1') { booted = true; break; }
      await new Promise(resolve => setTimeout(resolve, 2000));
    }
    assert(booted, 'boot deadline; no retry');
    run('device-identity', [adb, '-s', 'emulator-5554', 'shell', 'getprop']);
    run('remote-directory', [adb, '-s', 'emulator-5554', 'shell', 'mkdir', '-p', '/data/local/tmp/readscope-lifetime']);
    const artifact = path.join(process.env.RUNNER_TEMP, 'readscope-android-scene-artifact');
    const lib = path.join(artifact, 'x86_64/install/runtime/lib/linux_android31_x86_64_cjnative');
    const deploy = [path.join(out, 'fixture.so'), path.join(out, 'consumer'),
      ...files(lib).filter(p => p.endsWith('.so')),
      path.join(sdk, `ndk/${fixed.ndk}/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/x86_64-linux-android/libc++_shared.so`)];
    assert.equal(new Set(deploy.map(p => path.basename(p))).size, deploy.length);
    json(path.join(out, 'deployment.json'), deploy.map(p => ({ path: p, sha256: sha(p) })));
    for (const [i, p] of deploy.entries()) run(`push-${i}`, [adb, '-s', 'emulator-5554', 'push', p, '/data/local/tmp/readscope-lifetime/']);
    run('chmod-consumer', [adb, '-s', 'emulator-5554', 'shell', 'chmod', '700', '/data/local/tmp/readscope-lifetime/consumer']);
    run('clear-log', [adb, '-s', 'emulator-5554', 'logcat', '-b', 'all', '-c']);
    const symbols = fs.lstatSync(bindings.HOST_SYMBOLS);
    assert(symbols.isFile() && !symbols.isSymbolicLink(), 'HOST_SYMBOLS must be the new local consumer ELF');
    assert.equal(fs.realpathSync(bindings.HOST_SYMBOLS), bindings.HOST_SYMBOLS, 'HOST_SYMBOLS has no indirect alias');
    assert.equal(sha(bindings.HOST_SYMBOLS), inputs.consumer.sha256, 'relinked consumer unchanged');
    assert.equal(fs.readFileSync(bindings.HOST_SYMBOLS).subarray(0, 4).toString('hex'), '7f454c46');
    assert.equal(inputs.runtime.sha256, fixed.runtimeSha256);
    // One 300s collection-and-cleanup budget; the inner absolute deadline
    // includes its cleanup. Wrapper TERM/close and emulator cleanup are separate.
    sceneStarted = Date.now();
    const launch = sceneLaunch(out, sdk, env, sceneStarted);
    json(path.join(out, 'scene-bindings.json'), { ...launch.bindings,
      sceneStarted, hardDeadline: launch.hardDeadline,
      HOST_SYMBOLS_SHA256: inputs.consumer.sha256,
      remoteExecutable: '/data/local/tmp/readscope-lifetime/consumer',
      remoteFixture: '/data/local/tmp/readscope-lifetime/fixture.so',
      remoteLibraryPath: '/data/local/tmp/readscope-lifetime',
      runtimeSha256: fixed.runtimeSha256,
      runtime: runtimeInput,
      remoteRuntime: '/data/local/tmp/readscope-lifetime/libcangjie-runtime.so',
      recorderOutput: path.join(bundle, 'run'),
      actualLoadedIdentity: 'NOT_RUN; Grok must verify in this native execution' });
    const scene = asyncRunner(path.join(out, 'grok'), launch.env);
    const result = await scene('scene', launch.argv, launch.limits);
    const hardDeadline = launch.hardDeadline;
    json(path.join(out, 'scene-wrapper-result.json'), { ...result, sceneStarted, hardDeadline,
      innerDeadline: Number(launch.bindings.SCENE_DEADLINE_MS), nativeBusinessExit: 'UNKNOWN; read Grok records separately' });
    assert(result.reaped && !result.timedOut && !result.error, 'scene wrapper did not close normally');
    assert.equal(result.rc, 0, 'scene Node rc; not a native business rc');
    assert.equal(result.signal, null);
  } catch (error) {
    primaryError = error;
    json(path.join(out, 'execution-error.json'), { message: error.message, stack: error.stack });
  } finally {
    // Before scene launch there is no native sample; still bound preparation
    // cleanup. After launch every cleanup command shares the original deadline.
    const deadlineMs = sceneStarted === null ? Date.now() + 15000 : sceneStarted + fixed.totalSeconds * 1000;
    try {
      fs.mkdirSync(path.join(out, 'cleanup'));
      const cleanup = asyncRunner(path.join(out, 'cleanup'), env);
      const collect = (name, args) => {
        const deadline = Math.min(Date.now() + 1500, deadlineMs - 5000);
        return cleanup(name, args, { naturalUntilMs: Math.min(Date.now() + 1000, deadline),
          deadlineMs: deadline, termGraceMs: 250, killGraceMs: 250 });
      };
      cleanupResults.push(await collect('logcat', [adb, '-s', 'emulator-5554', 'logcat', '-d', '-b', 'all', '-v', 'epoch']));
      cleanupResults.push(await collect('crash', [adb, '-s', 'emulator-5554', 'logcat', '-d', '-b', 'crash', '-v', 'epoch']));
      cleanupResults.push(await collect('stop-emulator', [adb, '-s', 'emulator-5554', 'emu', 'kill']));
    } catch (error) {
      cleanupError = error.message;
      json(path.join(out, 'cleanup-error.json'), { message: error.message, stack: error.stack });
    } finally {
      emulatorResult = await settleOwnedChild(emulatorOwned, {
        naturalUntilMs: Math.min(Date.now() + 2000, deadlineMs - 3000), deadlineMs,
        termGraceMs: 1000, killGraceMs: 1000 });
      json(path.join(out, 'emulator-result.json'), { ...emulatorResult,
        sceneStarted, preparationOnly: sceneStarted === null, primaryError: primaryError?.message });
      json(path.join(out, 'cleanup-summary.json'), { deadlineMs, commands: cleanupResults,
        error: cleanupError, emulatorReaped: emulatorResult.reaped,
        confirmed: !cleanupError && cleanupResults.length === 3 && cleanupResults.every(row => row.reaped && !row.timedOut && !row.error && row.rc === 0) && emulatorResult.reaped });
      fs.closeSync(stdout); fs.closeSync(stderr);
    }
  }
  if (primaryError) throw primaryError;
  assert(emulatorResult?.reaped, 'EMULATOR_CLOSE_UNKNOWN; cleanup not confirmed');
  assert(!cleanupError && cleanupResults.length === 3 &&
    cleanupResults.every(row => row.reaped && !row.timedOut && !row.error && row.rc === 0),
  'CLEANUP_UNKNOWN; command state/records missing or cleanup incomplete');
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
const mode = process.argv[2];
try {
  if (mode === '--check-bundle') console.log(JSON.stringify(checkBundle(), null, 2));
  else if (mode === '--require-ready') requireReady();
  else if (mode === '--prepare') prepare();
  else if (mode === '--execute') {
    const { out } = context();
    fs.mkdirSync(path.join(out, 'execution')); fs.mkdirSync(path.join(out, 'grok'));
    await execute();
  } else throw Error('usage: android-scene.mjs --check-bundle|--require-ready|--prepare|--execute');
} catch (error) {
  console.error(error.stack); process.exitCode = 1;
}
}
