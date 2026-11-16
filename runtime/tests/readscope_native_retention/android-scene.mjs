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

function interfaceBindings(out, sdk) {
  return {
    SCENE_COMMANDS: path.join(out, 'scene-commands.json'),
    SCENE_OBSERVER: path.join(here, 'android-exit-records.mjs'),
    HOST_SYMBOLS: path.join(out, 'consumer'),
    ANDROID_SDK_ROOT: sdk,
    ANDROID_SERIAL: 'emulator-5554',
  };
}

function checkBundle() {
  for (const [name, entry] of Object.entries(origin.files)) {
    assert.equal(sha(path.join(bundle, name)), entry.sha256, `Grok file changed: ${name}`);
  }
  assert.equal(sha(path.join(bundle, 'job-interface.json')), origin.jobInterface.sha256);
  const plannedBindings = interfaceBindings('$RUNNER_TEMP/readscope-android-scene', '$ANDROID_HOME');
  assert.deepEqual(Object.keys(plannedBindings).sort(), [...jobInterface.requiredEnv].sort());
  assert.equal(sha(plannedBindings.SCENE_OBSERVER), fixed.observerSha256, 'exact shared 761b observer');
  assert.equal(jobInterface.readyForNativeExecution, false);
  assert.equal(fixed.mode, 'preinit');
  assert.equal(fixed.nativeProcesses, 1);
  assert.equal(fixed.runtime, 'd51c34a06cfd980d0e2694514e2a41421ac41b83');
  assert.equal(fixed.observer, '761b0ad956d975488b984e419ef4a803711e2635');
  return { state: 'PREPARED_NOT_EXECUTED', fixed, origin, jobInterface,
    plannedBindings, nativeRuns: 0 };
}

function requireReady() {
  checkBundle();
  // The corrected, byte-identical bundle still needs Grok's final entry check
  // and separate controller authorization before any native execution.
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

function files(root) {
  return fs.readdirSync(root, { withFileTypes: true }).flatMap(entry => {
    const name = path.join(root, entry.name);
    assert(!entry.isSymbolicLink(), `unexpected artifact/source symlink: ${name}`);
    return entry.isDirectory() ? files(name) : [name];
  });
}

function prepare() {
  const { out, sdk, env } = context();
  fs.mkdirSync(out); // Existing/partial output is never accepted as a cache.
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
  assert.equal(fs.readdirSync(bounds).length, 0, 'fresh fixed dependency only');
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
  run('runtime-identity', [path.join(tools, 'llvm-readelf'), '-hndW', so]);
  json(path.join(out, 'module-inputs.json'), {
    runtimeSource: fixed.runtime, runtime: { path: so, sha256: sha(so) },
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
  const closed = new Promise(resolve => {
    emulator.once('error', error => resolve({ error: error.message }));
    emulator.once('close', (rc, signal) => resolve({ rc, signal }));
  });
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
    const bindings = interfaceBindings(out, sdk);
    const inputs = JSON.parse(fs.readFileSync(path.join(out, 'module-inputs.json')));
    const symbols = fs.lstatSync(bindings.HOST_SYMBOLS);
    assert(symbols.isFile() && !symbols.isSymbolicLink(), 'HOST_SYMBOLS must be the new local consumer ELF');
    assert.equal(fs.realpathSync(bindings.HOST_SYMBOLS), bindings.HOST_SYMBOLS, 'HOST_SYMBOLS has no indirect alias');
    assert.equal(sha(bindings.HOST_SYMBOLS), inputs.consumer.sha256, 'relinked consumer unchanged');
    assert.equal(fs.readFileSync(bindings.HOST_SYMBOLS).subarray(0, 4).toString('hex'), '7f454c46');
    assert.equal(sha(bindings.SCENE_OBSERVER), fixed.observerSha256);
    assert.equal(inputs.runtime.sha256, fixed.runtimeSha256);
    json(path.join(out, 'scene-bindings.json'), { ...bindings,
      HOST_SYMBOLS_SHA256: inputs.consumer.sha256,
      SCENE_OBSERVER_SHA256: fixed.observerSha256,
      remoteExecutable: '/data/local/tmp/readscope-lifetime/consumer',
      remoteFixture: '/data/local/tmp/readscope-lifetime/fixture.so',
      remoteLibraryPath: '/data/local/tmp/readscope-lifetime',
      runtimeSha256: fixed.runtimeSha256,
      recorderOutput: path.join(bundle, 'run'),
      actualLoadedIdentity: 'NOT_RUN; Grok must verify in this native execution' });
    const sceneEnv = { ...env, ...bindings, SCENE_EXECUTE: '1', EXIT_SCENE_OUT: path.join(bundle, 'run') };
    const scene = runner(path.join(out, 'grok'), sceneEnv);
    scene.run('scene', [process.execPath, path.join(bundle, 'scene.mjs'), 'execute'], { timeout: fixed.totalSeconds * 1000 });
  } finally {
    run('logcat', [adb, '-s', 'emulator-5554', 'logcat', '-d', '-b', 'all', '-v', 'epoch'], { allowFailure: true, timeout: 10000 });
    run('crash', [adb, '-s', 'emulator-5554', 'logcat', '-d', '-b', 'crash', '-v', 'epoch'], { allowFailure: true, timeout: 10000 });
    run('stop-emulator', [adb, '-s', 'emulator-5554', 'emu', 'kill'], { allowFailure: true, timeout: 10000 });
    const ended = await Promise.race([closed, new Promise(resolve => setTimeout(() => resolve(null), 10000))]);
    if (ended === null) emulator.kill('SIGTERM');
    json(path.join(out, 'emulator-result.json'), { pid: emulator.pid, ended, explicitTerminate: ended === null });
    fs.closeSync(stdout); fs.closeSync(stderr);
  }
}

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
