#!/usr/bin/env zx
// Preparation contract: two frozen normal runtime productions, three native
// executions total. Run only on the separately authorized official GHA job.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import {fileURLToPath} from 'node:url';
import {spawn, spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {classifyExit} from './android-exit-records.mjs';

assert.equal(process.platform, 'linux');
assert.equal(process.env.GITHUB_ACTIONS, 'true');
assert.equal(process.env.READSCOPE_ANDROID_EXIT_COMPARE, 'fixed-three-children-v1');
const here = path.dirname(fileURLToPath(import.meta.url));
const repository = path.resolve(here, '../../..');
const out = path.join(process.env.RUNNER_TEMP, 'readscope-android-exit');
fs.mkdirSync(out); // A reused output is an error, never a restored success.
const refs = {base: '8e25120c25fe0868511c1d3f2f47a9e74ccbf687', candidate: 'd51c34a06cfd980d0e2694514e2a41421ac41b83'};
const boundsRef = '2ae82839ecaaa7d031e66ccbd2076d671acfd615';
const android = process.env.ANDROID_HOME;
assert(android && path.isAbsolute(android));
const ndkVersion = '27.3.13750724';
const ndk = path.join(android, 'ndk', ndkVersion);
const toolchain = path.join(ndk, 'toolchains/llvm/prebuilt/linux-x86_64');
const tools = toolchain + '/bin', adb = android + '/platform-tools/adb';
const androidUser = out + '/.android', avdHome = androidUser + '/avd';
fs.mkdirSync(avdHome, {recursive: true});
const env = {...process.env, ANDROID_SDK_HOME: out, ANDROID_USER_HOME: androidUser,
  ANDROID_EMULATOR_HOME: androidUser, ANDROID_AVD_HOME: avdHome, TMPDIR: out,
  CANGJIE_BUILD_JOBS: '2', CMAKE_BUILD_PARALLEL_LEVEL: '2',
  GC_UNIT_GATE_SKIP: '1', SCCACHE_GHA_ENABLED: 'true'};
const rows = [], cases = [];
const save = () => fs.writeFileSync(out + '/commands.json', JSON.stringify(rows, null, 2));
function run(name, args, {input, expected = 0, timeout = 2700000} = {}) {
  const row = {name, args, cwd: out, started: new Date().toISOString(), expected};
  rows.push(row); save();
  const a = fs.openSync(out + '/' + name + '.stdout', 'wx');
  const b = fs.openSync(out + '/' + name + '.stderr', 'wx');
  const result = spawnSync(args[0], args.slice(1), {cwd: out, env, input,
    stdio: [input === undefined ? 'ignore' : 'pipe', a, b], timeout});
  fs.closeSync(a); fs.closeSync(b);
  Object.assign(row, {rc: result.status, signal: result.signal, error: result.error?.message,
    ended: new Date().toISOString()}); save();
  console.log(name, row.rc, row.signal);
  if (expected !== null) {
    assert.equal(row.rc, expected, name); assert.equal(row.signal, null, name);
  }
  return fs.readFileSync(out + '/' + name + '.stdout', 'utf8');
}
const files = root => fs.readdirSync(root, {withFileTypes: true}).flatMap(e =>
  e.isDirectory() ? files(root + '/' + e.name) : [root + '/' + e.name]);
const identity = p => ({path: p, sha256: createHash('sha256').update(fs.readFileSync(p)).digest('hex')});
const harness = run('harness-ref', ['git', '-C', repository, 'rev-parse', 'HEAD']).trim();
run('fetch-fixed-runtime', ['git', '-C', repository, 'fetch', '--no-tags', '--depth=1', 'origin', ...Object.values(refs)]);
// One runtime Git root (the normal checkout), with both immutable sources as
// linked worktrees. Bounds likewise has one root and two linked worktrees.
const bounds = out + '/bounds-root';
run('bounds-init', ['git', 'init', bounds]);
run('bounds-remote', ['git', '-C', bounds, 'remote', 'add', 'origin', 'https://gitcode.com/openharmony/third_party_bounds_checking_function']);
run('bounds-fetch', ['git', '-C', bounds, 'fetch', '--depth=1', 'origin', boundsRef]);
run('sdk-packages', [android + '/cmdline-tools/latest/bin/sdkmanager', '--install',
  'ndk;' + ndkVersion, 'platform-tools', 'emulator', 'system-images;android-31;google_apis;x86_64'], {input: 'y\n'.repeat(8)});
run('compiler-version', [tools + '/clang', '--version']);
run('emulator-version', [android + '/emulator/emulator', '-version']);
run('ndk-package', ['cat', ndk + '/source.properties']);
const built = {};
for (const [arm, ref] of Object.entries(refs)) {
  const source = out + '/' + arm + '-source';
  run(arm + '-worktree', ['git', '-C', repository, 'worktree', 'add', '--detach', source, ref]);
  assert.equal(run(arm + '-ref', ['git', '-C', source, 'rev-parse', 'HEAD']).trim(), ref);
  const dependency = source + '/runtime/third_party/third_party_bounds_checking_function';
  run(arm + '-bounds', ['git', '-C', bounds, 'worktree', 'add', '--detach', dependency, boundsRef]);
  const runtime = source + '/runtime', build = out + '/' + arm + '/build', install = out + '/' + arm + '/install';
  const cc = tools + '/x86_64-linux-android31-clang';
  run(arm + '-configure', ['cmake', '-S', runtime, '-B', build, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release', '-DANDROID_FLAG=2', '-DANDROID_ROOT=' + ndk + '/toolchains',
    '-DCMAKE_ANDROID_API=31', '-DOHOS_FLAG=0', '-DWINDOWS_FLAG=0', '-DCOPYGC_FLAG=1',
    '-DDOPRA_FLAG=1', '-DRUNTIME_TRACE_FLAG=1', '-DCJ_SDK_VERSION=0.0.1', '-DDISABLE_VERSION_CHECK=1',
    '-DCMAKE_C_COMPILER=' + cc, '-DCMAKE_CXX_COMPILER=' + cc + '++', '-DCMAKE_ASM_COMPILER=' + cc,
    '-DCMAKE_C_COMPILER_LAUNCHER=sccache', '-DCMAKE_CXX_COMPILER_LAUNCHER=sccache',
    '-DCMAKE_INSTALL_PREFIX=' + install, '-DCJ_RUNTIME_COMMIT=' + ref, '-DMRT_GC_UNIT_TESTS=OFF']);
  run(arm + '-build', ['cmake', '--build', build, '--parallel', '2']);
  run(arm + '-install', ['cmake', '--install', build]);
  const libraries = files(install).filter(p => p.endsWith('/libcangjie-runtime.so'));
  assert.equal(libraries.length, 1);
  const lib = path.dirname(libraries[0]);
  const headers = [runtime + '/src', runtime + '/include', runtime + '/src/Loader/BinaryFile',
    runtime + '/src/CJThread/src/runtime/schedule/include', dependency + '/include',
    ...files(build).filter(p => p.endsWith('/log.h')).map(p => path.dirname(p))];
  const consumer = out + '/' + arm + '/consumer';
  run(arm + '-consumer-link', [cc + '++', '-std=gnu++17', '-O2', ...headers.map(p => '-I' + p),
    here + '/android-exit-consumer.cpp', '-L' + lib, '-lcangjie-runtime', '-ldl', '-o', consumer]);
  const symbols = run(arm + '-consumer-symbols', [tools + '/llvm-readelf', '-sW', consumer]);
  assert(!/MRT_LibraryOnLoadRetainedV1|CJ_MRT_LibraryOnLoadRetainedV1/.test(symbols), 'no new image ABI in common consumer');
  run(arm + '-runtime-elf', [tools + '/llvm-readelf', '-dnW', libraries[0]]);
  built[arm] = {ref, source, build, install, runtime: identity(libraries[0]), consumer: identity(consumer),
    header: identity(runtime + '/src/Cangjie.h'), lib};
}
assert.equal(built.base.header.sha256, built.candidate.header.sha256, 'common public C API header is unchanged');
fs.writeFileSync(out + '/identity.json', JSON.stringify({harness, refs, boundsRef, ndkVersion, api: 31,
  systemImage: 'system-images;android-31;google_apis;x86_64', built,
  source: identity(here + '/android-exit-consumer.cpp'), scope: 'normal x64 Android production; exactly three common-C-API/no-image children; no performance/full-image fix qualification'}, null, 2));

run('create-avd', [android + '/cmdline-tools/latest/bin/avdmanager', 'create', 'avd', '--name', 'readscope-exit',
  '--path', avdHome + '/readscope-exit.avd', '--package', 'system-images;android-31;google_apis;x86_64', '--device', 'pixel_2'], {input: 'no\n'});
assert(fs.statSync(avdHome + '/readscope-exit.ini').isFile());
run('list-avds', [android + '/cmdline-tools/latest/bin/avdmanager', 'list', 'avd']);
assert(run('native-list-avds', [android + '/emulator/emulator', '-list-avds']).split(/\r?\n/).includes('readscope-exit'));
const a = fs.openSync(out + '/emulator.stdout', 'wx'), b = fs.openSync(out + '/emulator.stderr', 'wx');
const emulator = spawn(android + '/emulator/emulator', ['-avd', 'readscope-exit', '-port', '5554',
  '-no-window', '-no-audio', '-no-boot-anim', '-no-snapshot', '-gpu', 'swiftshader_indirect', '-memory', '2048'],
{env, stdio: ['ignore', a, b]});
const emulatorExit = new Promise(resolve => {
  emulator.on('error', error => resolve({error: error.message}));
  emulator.on('close', (rc, signal) => resolve({rc, signal}));
});
const device = (...args) => [adb, '-s', 'emulator-5554', ...args];
try {
  run('wait-device', device('wait-for-device'), {timeout: 180000});
  const deadline = Date.now() + 240000; let boot = false;
  while (Date.now() < deadline) {
    const r = spawnSync(adb, ['-s', 'emulator-5554', 'shell', 'getprop', 'sys.boot_completed'], {encoding: 'utf8', timeout: 5000});
    if (r.status === 0 && r.stdout.trim() === '1') {boot = true; break;}
    await new Promise(resolve => setTimeout(resolve, 2000));
  }
  assert(boot, 'original Android emulator boot deadline');
  run('device-identity', device('shell', 'getprop'));
  // Access is optional. Never invoke a debugger or request a live thread dump.
  run('optional-root-for-existing-tombstones', device('root'), {expected: null, timeout: 10000});
  run('root-wait-device', device('wait-for-device'), {timeout: 30000});
  for (const [arm, item] of Object.entries(built)) {
    const remote = '/data/local/tmp/readscope-exit/' + arm;
    run(arm + '-remote-dir', device('shell', 'mkdir', '-p', remote));
    const deploy = [item.consumer.path, ...files(item.lib).filter(p => p.endsWith('.so')),
      toolchain + '/sysroot/usr/lib/x86_64-linux-android/libc++_shared.so'];
    const names = new Set();
    for (const [index, file] of deploy.entries()) {
      assert(!names.has(path.basename(file)), 'ambiguous dependency'); names.add(path.basename(file));
      run(arm + '-push-' + index, device('push', file, remote + '/'));
    }
    item.deployed = deploy.map(identity);
    run(arm + '-chmod', device('shell', 'chmod', '700', remote + '/consumer'));
    item.remote = remote;
  }
  fs.writeFileSync(out + '/deployed-identity.json', JSON.stringify(built, null, 2));
  // Fixed count/order. A child/fatal failure is recorded and the remaining
  // authorized independent controls still run once; preparation fails closed.
  for (const [name, arm, mode] of [['base-return', 'base', 'return'], ['candidate-return', 'candidate', 'return'], ['candidate-fini', 'candidate', 'fini']]) {
    run(name + '-clear-log', device('logcat', '-b', 'all', '-c'), {timeout: 10000});
    const before = run(name + '-tombstone-before', device('shell', 'ls', '-ln', '/data/tombstones'), {expected: null, timeout: 10000});
    const started = new Date().toISOString(), remote = built[arm].remote;
    // The paths and modes are fixed ASCII literals, never user shell input.
    const shell = `ulimit -c 0; LD_LIBRARY_PATH=${remote} ${remote}/consumer ${mode}; native_status=$?; printf '\\nEXIT_SHELL native_wait_rc=%s\\n' "$native_status"; exit "$native_status"`;
    const stdout = run(name, device('shell', shell), {expected: null, timeout: 20000});
    const command = rows.at(-1);
    // This bounded log-drain delay is not another native invocation/warmup.
    await new Promise(resolve => setTimeout(resolve, 1000));
    const logcat = run(name + '-logcat', device('logcat', '-d', '-b', 'all', '-v', 'epoch'), {expected: null, timeout: 10000});
    const logCommand = rows.at(-1);
    const result = classifyExit({stdout, stderr: fs.readFileSync(out + '/' + name + '.stderr', 'utf8'), logcat,
      adbRc: command.rc, adbSignal: command.signal, adbError: command.error, mode,
      remoteRuntime: remote + '/libcangjie-runtime.so'});
    result.logCaptureComplete = logCommand.rc === 0 && logCommand.signal === null && !logCommand.error;
    result.pass &&= result.logCaptureComplete;
    fs.writeFileSync(out + '/' + name + '.pid-logcat', result.pidLogcat);
    delete result.pidLogcat;
    // Only already generated system crash records/files; no debugger, signals,
    // stack requests or repeated execution. Keep raw files and PID attribution.
    run(name + '-crash-buffer', device('logcat', '-d', '-b', 'crash', '-v', 'epoch'), {expected: null, timeout: 10000});
    const listing = run(name + '-tombstone-list', device('shell', 'ls', '-ln', '/data/tombstones'), {expected: null, timeout: 10000});
    const directory = out + '/tombstones/' + name;
    fs.mkdirSync(directory, {recursive: true});
    for (const filename of [...new Set(listing.match(/\btombstone_\d+\b/g) ?? [])]) {
      const line = listing.split('\n').find(row => row.endsWith(' ' + filename));
      if (line && before.split('\n').includes(line)) continue;
      run(name + '-pull-' + filename, device('pull', '/data/tombstones/' + filename, directory + '/' + filename), {expected: null, timeout: 10000});
    }
    const tombstones = files(directory).map(file => ({...identity(file),
      samePidNewOrChangedFile: result.pid !== null && new RegExp(`\\bpid: ${result.pid},`).test(fs.readFileSync(file, 'utf8')),
      attribution: 'raw postmortem file; compare its Timestamp/PID with this case, not a mutex-cause proof'}));
    if (tombstones.some(row => row.samePidNewOrChangedFile)) result.pass = false;
    cases.push({name, arm, mode, started, ended: new Date().toISOString(), command, ...result, tombstones});
    fs.writeFileSync(out + '/results.json', JSON.stringify({plannedChildren: 3, warmup: 0, retry: 0,
      cases, fullImageExitRed: 'OPEN; original d51c run43760946735 is not replaced', cause: 'UNKNOWN'}, null, 2));
  }
  assert.equal(cases.length, 3);
  process.exitCode = cases.every(row => row.pass) ? 0 : 1;
} finally {
  run('stop-emulator', device('emu', 'kill'), {expected: null, timeout: 10000});
  const ended = await Promise.race([emulatorExit, new Promise(resolve => setTimeout(() => resolve(null), 10000))]);
  const explicitTerminated = ended === null && emulator.kill('SIGTERM');
  const afterTerm = ended === null ? await Promise.race([emulatorExit,
    new Promise(resolve => setTimeout(() => resolve(null), 10000))]) : null;
  fs.writeFileSync(out + '/emulator-result.json', JSON.stringify({pid: emulator.pid, ended, explicitTerminated, afterTerm}));
  if (ended === null && afterTerm === null) process.exitCode = 1;
  fs.closeSync(a); fs.closeSync(b);
}
