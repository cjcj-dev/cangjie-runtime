#!/usr/bin/env zx
import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { pathToFileURL } from 'node:url';

const HERE = dirname(new URL(import.meta.url).pathname);
const OLD_ADDRESS = '0x7b33ef1eb08c';
const RUNTIME = 'd51c34a06cfd980d0e2694514e2a41421ac41b83';
const ORIGINAL_SO = 'aac5c4456fc9746243a2a15d22beac6a52ee27f269a94cd0e839315bc080db51';
const OBSERVER = '761b0ad956d975488b984e419ef4a803711e2635';
const NDK = '27.3.13750724';
const REMOTE = '/data/local/tmp/readscope-lifetime';
const TARGETS = ['elf-native-load', 'elf-constructor-exactly-once', 'elf-public-load',
  'elf-public-native-identity', 'elf-unknown-close-rejected', 'elf-logical-close',
  'elf-external-native-close', 'elf-no-close-fini', 'elf-reopen-same-identity',
  'elf-reopen-no-double-init', 'elf-reopen-close', 'elf-runtime-init', 'elf-published-package'];

const TOOLS = {
  hostLldb: `ndk/${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/lldb`,
  hostWrapper: `ndk/${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/lldb.sh`,
  server: `ndk/${NDK}/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/18/lib/linux/x86_64/lldb-server`,
  adb: 'platform-tools/adb',
  emulator: 'emulator/emulator',
};

function fail(message) {
  throw new Error(message);
}

function command(records, name) {
  const found = records.filter(item => item.name === name);
  if (found.length !== 1) fail(`route ${name} count ${found.length}`);
  return found[0];
}

function route(records) {
  const fixture = command(records, 'fixture-link');
  const consumer = command(records, 'consumer-link');
  const preinit = command(records, 'preinit');
  const text = JSON.stringify(preinit.args);
  if (preinit.args.at(-1) !== 'preinit') fail('preinit argv does not end in preinit');
  if (text.includes('runtime-fini') || text.includes('FiniCJRuntime')) fail('preinit route contains fini');
  if (!fixture.args.some(arg => arg.endsWith('/elf-fixture.cpp'))) fail('fixture source drifted');
  if (!consumer.args.some(arg => arg.endsWith('/elf-consumer.cpp'))) fail('consumer source drifted');
  return {
    mode: 'preinit',
    fullImage: true,
    runtime: RUNTIME,
    originalSo: ORIGINAL_SO,
    originalSoStatus: 'identity only; fixture ELF was not saved; a relink is a new SHA',
    observer: OBSERVER,
    fixtureLink: fixture.args,
    consumerLink: consumer.args,
    launch: preinit.args,
    rejected: ['runtime-fini', 'no-image', 'NoFP', OLD_ADDRESS],
  };
}

function preflight(sdk) {
  const tools = {};
  for (const [name, relative] of Object.entries(TOOLS)) {
    const path = join(sdk, relative);
    tools[name] = { path, present: existsSync(path) };
  }
  return { sdk, kvm: existsSync('/dev/kvm'), tools, executed: false, remoteAndroid: 'NOT_VERIFIED' };
}

function finish(outcome, simulated, log, extra = {}) {
  return {
    outcome,
    simulated,
    sceneSuccess: false,
    productPass: false,
    readyForNativeExecution: false,
    originalFault: 'OPEN',
    log,
    ...extra,
  };
}

function stackOk(frames) {
  return Array.isArray(frames) && frames.length >= 2 && frames.every(frame => frame && frame.pc != null && frame.module && frame.name);
}

function quietObservation() {
  return [
    `${REMOTE}/libcangjie-runtime.so`,
    'build-id: 0123456789abcdef',
    ...TARGETS.map(name => `TARGET ${name} executed=1 pass=1`),
    'ELF_FIXTURE_INIT count=1',
    'ELF_FIXTURE_FINI count=1',
    'EXIT_SHELL child_pid=2400 realtime_start=1763240464.000999999 realtime_end=1763240465.000999999 native_wait_rc=0',
  ].join('\n');
}

function startShell() {
  return `f=${REMOTE}/lldb-server.identity; rm -f "$f"; echo DEVICE_PID=$$ > "$f"; echo DEVICE_START=$(awk '{print $22}' /proc/$$/stat) >> "$f"; exec ${REMOTE}/lldb-server platform --listen '*:5039' --server`;
}

function identityShell() {
  return `f=${REMOTE}/lldb-server.identity
pid=$(sed -n 's/^DEVICE_PID=//p' "$f")
start=$(sed -n 's/^DEVICE_START=//p' "$f")
cmd=$(tr '\\0' ' ' < /proc/$pid/cmdline)
now=$(awk '{print $22}' /proc/$pid/stat)
owned=0
case "$cmd" in
  *${REMOTE}/lldb-server*)
    if [ "$now" = "$start" ]; then
      inodes=$(ls -l /proc/$pid/fd 2>/dev/null | sed -n 's/.*socket:\\[\\([0-9][0-9]*\\)\\].*/\\1/p')
      for inode in $inodes; do
        if grep -q " $inode " /proc/net/tcp /proc/net/tcp6 2>/dev/null && grep -Eqi ':13af ' /proc/net/tcp /proc/net/tcp6; then owned=1; fi
      done
    fi
    ;;
esac
echo DEVICE_PID=$pid
echo DEVICE_START=$start
echo CMDLINE=$cmd
echo PORT_OWNED=$owned`;
}

function cleanupShell(pid, start) {
  return `set -eu
pid=${pid}
start=${start}
cmd=$(tr '\\0' ' ' < /proc/$pid/cmdline)
now=$(awk '{print $22}' /proc/$pid/stat)
case "$cmd" in
  *${REMOTE}/lldb-server*) ;;
  *) echo UNCONFIRMED; exit 3 ;;
esac
[ "$now" = "$start" ] || { echo STALE; exit 3; }
kill -TERM "$pid"
i=0
while [ "$i" -lt 25 ]; do
  if [ ! -d /proc/$pid ]; then echo REAPED; exit 0; fi
  i=$((i+1))
  sleep 0.2
done
echo NOT_REAPED
exit 4`;
}

function parseIdentity(text, hostPid) {
  const grab = name => {
    const matched = String(text || '').match(new RegExp(`^${name}=(.*)$`, 'm'));
    return matched ? matched[1].trim() : '';
  };
  const pid = grab('DEVICE_PID');
  const start = grab('DEVICE_START');
  const cmdline = grab('CMDLINE');
  if (!/^\d+$/.test(pid) || !/^\d+$/.test(start)) return null;
  if (hostPid != null && pid === String(hostPid)) return null;
  if (!cmdline.includes(`${REMOTE}/lldb-server`)) return null;
  if (grab('PORT_OWNED') !== '1') return null;
  return { pid, start, cmdline };
}

function launchArgv(lldb) {
  const capture = join(HERE, 'capture.py');
  const settings = join(HERE, 'lldb-commands.txt');
  const symbols = process.env.HOST_SYMBOLS;
  return [
    lldb, '-b',
    '-o', `command source ${settings}`,
    '-o', `command script import ${capture}`,
    '-o', 'platform select remote-android',
    '-o', 'platform connect connect://127.0.0.1:5039',
    '-o', `target create ${REMOTE}/consumer`,
    '-o', `target symbols add ${symbols}`,
    '-o', 'script capture.arm()',
    '-o', 'process handle SIGABRT --stop true --pass false --notify true',
    '-o', 'target stop-hook add --one-liner "script capture.on_stop()"',
    '-o', `process launch --environment LD_LIBRARY_PATH=${REMOTE} -- ${REMOTE}/fixture.so preinit`,
    '-o', 'image list',
    '-o', 'script capture.dump()',
    '-o', 'quit',
  ];
}

function jsonLine(text, prefix) {
  const line = String(text || '').split('\n').find(row => row.startsWith(prefix));
  if (!line) return null;
  try { return JSON.parse(line.slice(prefix.length)); } catch { return null; }
}

function messageAddress(text) {
  const matched = String(text || '').match(/destroyed mutex \((0x[0-9a-fA-F]+)\)/);
  return matched ? Number(BigInt(matched[1])) : null;
}

function decode(document) {
  return new Promise(resolve => {
    const child = spawn('python3', [join(HERE, 'capture.py'), 'decode'], { stdio: ['pipe', 'pipe', 'pipe'] });
    let stdout = '';
    child.stdout.on('data', chunk => { stdout += chunk; });
    child.stdin.end(JSON.stringify(document));
    child.on('close', code => {
      if (code !== 0) resolve({ chain: 'UNKNOWN', source: 'decode' });
      else {
        try { resolve(JSON.parse(stdout)); } catch { resolve({ chain: 'UNKNOWN', source: 'decode' }); }
      }
    });
  });
}

async function loadJudge() {
  const path = process.env.SCENE_OBSERVER;
  if (!path) return null;
  const loaded = await import(pathToFileURL(path).href);
  return loaded.classifyFullImageExit;
}

async function drive(io, sdk, serial = 'emulator-5554', judge = null) {
  const log = [];
  const meta = { cleanup: 'NOT_STARTED', hostPid: null, devicePid: null, remoteAndroid: false };
  const started = { device: null, forward: false };
  const tool = name => join(sdk, TOOLS[name]);
  const run = async (step, argv, options = {}) => {
    const result = await io.run(step, argv, options);
    log.push({
      step, argv, rc: result.rc, timedOut: Boolean(result.timedOut),
      stdout: typeof result.stdout === 'string' ? result.stdout : null,
      stderr: typeof result.stderr === 'string' ? result.stderr : null,
    });
    return result;
  };
  const cleanup = async () => {
    if (started.device) {
      const cleaned = await run('cleanup-server', [tool('adb'), '-s', serial, 'shell', cleanupShell(started.device.pid, started.device.start)]);
      meta.cleanup = cleaned.rc === 0 && String(cleaned.stdout || '').includes('REAPED') ? 'REAPED' : 'UNCONFIRMED';
    }
    if (started.forward) await run('cleanup-forward', [tool('adb'), '-s', serial, 'forward', '--remove', 'tcp:5039']);
  };
  try {
    for (const name of ['hostLldb', 'server', 'adb']) {
      if (!io.stat(tool(name))) return finish('TOOL_ABSENT', io.simulated, log, { missing: name, meta });
    }
    const commands = io.readText(join(HERE, 'lldb-commands.txt'));
    if (commands.includes('/root') || commands.includes('arm()') || commands.includes('import ')) {
      return finish('COMMANDS_INVALID', io.simulated, log, { meta });
    }
    const symbols = process.env.HOST_SYMBOLS;
    if (!symbols || symbols === `${REMOTE}/consumer`) return finish('SYMBOLS_ABSENT', io.simulated, log, { meta });
    const hostVer = await run('host-version', [tool('hostLldb'), '--version'], { timeoutMs: 30000 });
    if (hostVer.timedOut) return finish('UNCAPTURED', io.simulated, log, { meta });
    if (hostVer.rc !== 0 || !hostVer.stdout) return finish('VERSION_ABSENT', io.simulated, log, { meta });
    const platforms = await run('platform-list', [tool('hostLldb'), '-b', '-o', 'platform list', '-o', 'quit'], { timeoutMs: 30000 });
    if (platforms.timedOut) return finish('UNCAPTURED', io.simulated, log, { meta });
    if (platforms.rc !== 0) return finish('PLATFORM_LIST_FAIL', io.simulated, log, { meta });
    if (!/(^|\s)remote-android(\s|$)/.test(platforms.stdout || '')) return finish('PLATFORM_ABSENT', io.simulated, log, { meta });
    meta.remoteAndroid = true;
    const devices = await run('adb-devices', [tool('adb'), 'devices'], { timeoutMs: 30000 });
    if (devices.rc !== 0 || !(devices.stdout || '').includes(serial)) return finish('CONNECT_FAIL', io.simulated, log, { meta });
    const pushed = await run('push-server', [tool('adb'), '-s', serial, 'push', tool('server'), `${REMOTE}/lldb-server`]);
    if (pushed.rc !== 0) return finish('CONNECT_FAIL', io.simulated, log, { meta });
    const serverVer = await run('server-version', [tool('adb'), '-s', serial, 'shell', `${REMOTE}/lldb-server version`]);
    if (serverVer.rc !== 0 || !serverVer.stdout) return finish('VERSION_ABSENT', io.simulated, log, { meta });
    const server = await run('start-server', [tool('adb'), '-s', serial, 'shell', startShell()], { background: true });
    meta.hostPid = typeof server.pid === 'number' ? server.pid : null;
    if (server.rc !== 0) return finish('CONNECT_FAIL', io.simulated, log, { meta });
    const identity = await run('server-identity', [tool('adb'), '-s', serial, 'shell', identityShell()]);
    started.device = parseIdentity(identity.stdout, meta.hostPid);
    meta.devicePid = started.device ? started.device.pid : null;
    if (!started.device) return finish('SERVER_UNCONFIRMED', io.simulated, log, { meta });
    const forwarded = await run('forward', [tool('adb'), '-s', serial, 'forward', 'tcp:5039', 'tcp:5039']);
    if (forwarded.rc !== 0) return finish('CONNECT_FAIL', io.simulated, log, { meta });
    started.forward = true;
    const launch = await run('launch', launchArgv(tool('hostLldb')), { timeoutMs: 60000, totalMs: 300000 });
    if (launch.timedOut) return finish('UNCAPTURED', io.simulated, log, { meta });
    const logcat = await run('logcat', [tool('adb'), '-s', serial, 'logcat', '-d', '-v', 'epoch']);
    const crash = await run('crash', [tool('adb'), '-s', serial, 'logcat', '-d', '-b', 'crash', '-v', 'epoch']);
    if (typeof launch.stdout !== 'string' || typeof launch.stderr !== 'string') {
      return finish('LOG_MISSING', io.simulated, log, { meta });
    }
    const text = `${launch.stdout}\n${launch.stderr}`;
    const fatal = /FORTIFY:|\bSIGABRT\b|Abort message:/.test(text);
    const maps = /libcangjie-runtime\.so/.test(launch.stdout) && /build-id:/i.test(launch.stdout);
    const buildId = (launch.stdout.match(/build-id:\s*(\S+)/i) || [])[1] || null;
    if (fatal) {
      const state = jsonLine(launch.stdout, 'EVENT_JSON:');
      const fatalFrames = jsonLine(launch.stdout, 'FATAL_FRAMES_JSON:') || state?.fatal?.frames || null;
      const linked = await decode({ state: state || { complete: false }, dwarf: null, messageAddress: messageAddress(text), fatalFrames });
      const outcome = linked.chain === 'DESTROY0_THEN_LOCK' && stackOk(linked.destroy?.frames) && stackOk(linked.fatalFrames)
        ? 'FATAL_ATTRIBUTED' : 'FATAL_UNKNOWN';
      return finish(outcome, io.simulated, log, { association: linked, maps, buildId, meta });
    }
    if (launch.rc !== 0) return finish('LAUNCH_FAILED', io.simulated, log, { maps, buildId, meta });
    if (!maps || !buildId) return finish('MAPS_ABSENT', io.simulated, log, { maps, buildId, meta });
    if (typeof judge !== 'function') return finish('OBSERVER_ABSENT', io.simulated, log, { maps, buildId, meta });
    const judged = judge({
      stdout: launch.stdout, stderr: launch.stderr,
      logcat: typeof logcat.stdout === 'string' ? logcat.stdout : '',
      crash: typeof crash.stdout === 'string' ? crash.stdout : '',
      adbRc: launch.rc, adbSignal: null, adbError: null, mode: 'preinit',
      remoteRuntime: `${REMOTE}/libcangjie-runtime.so`,
      captures: {
        stderr: true,
        logcat: { rc: logcat.rc, signal: null },
        crash: { rc: crash.rc, signal: null },
      },
    });
    if (judged.outcome === 'FAIL') return finish('OBSERVER_FAIL', io.simulated, log, { observer: judged.outcome, nativePid: judged.pid, buildId, meta });
    if (judged.outcome !== 'PASS') return finish('OBSERVER_UNKNOWN', io.simulated, log, { observer: judged.outcome, nativePid: judged.pid, buildId, meta });
    return finish('NOT_REPRODUCED', io.simulated, log, { observer: judged.outcome, nativePid: judged.pid, buildId, meta });
  } catch (error) {
    return finish('DRIVER_EXCEPTION', io.simulated, log, { error: String(error), meta });
  } finally {
    try { await cleanup(); } catch (error) { meta.cleanup = 'UNCONFIRMED'; meta.cleanupError = String(error); }
  }
}

function scripted(script) {
  const runs = [];
  const defaults = {
    'host-version': { rc: 0, stdout: 'lldb version 18.0.1', stderr: '' },
    'platform-list': { rc: 0, stdout: script.platform || '', stderr: '' },
    'adb-devices': { rc: 0, stdout: script.serial || '', stderr: '' },
    'push-server': { rc: 0, stdout: '', stderr: '' },
    'server-version': { rc: 0, stdout: 'lldb-server version 18.0.1', stderr: '' },
    'start-server': { rc: 0, stdout: '', stderr: '', pid: 100 },
    'server-identity': { rc: 0, stdout: `DEVICE_PID=200\nDEVICE_START=99\nCMDLINE=${REMOTE}/lldb-server platform --listen *:5039\nPORT_OWNED=1\n`, stderr: '' },
    forward: { rc: 0, stdout: '', stderr: '' },
    launch: { rc: 0, stdout: quietObservation(), stderr: '' },
    logcat: { rc: 0, stdout: '', stderr: '' },
    crash: { rc: 0, stdout: '', stderr: '' },
    'cleanup-server': { rc: 0, stdout: 'REAPED\n', stderr: '' },
    'cleanup-forward': { rc: 0, stdout: '', stderr: '' },
  };
  return {
    simulated: true,
    stat(path) { return script.absent ? false : !String(path).endsWith('/missing'); },
    readText(path) { return readFileSync(path, 'utf8'); },
    async run(step, argv) {
      runs.push(step);
      if (script.throw === step) throw new Error(`injected ${step}`);
      if (script.timeout === step) return { rc: null, timedOut: true, stdout: '', stderr: '' };
      const row = { ...(defaults[step] || { rc: 0, stdout: '', stderr: '' }), ...(script.steps[step] || {}) };
      return row;
    },
    runs,
  };
}

function realIO() {
  return {
    simulated: false,
    stat(path) { return existsSync(path); },
    readText(path) { return readFileSync(path, 'utf8'); },
    run(step, argv, options = {}) {
      return new Promise(resolve => {
        const child = spawn(argv[0], argv.slice(1), {
          stdio: ['ignore', 'pipe', 'pipe'],
          env: { ...process.env, EXIT_SCENE_OUT: join(HERE, 'run') },
        });
        let stdout = '';
        let stderr = '';
        let settled = false;
        const finishRun = result => { if (!settled) { settled = true; resolve(result); } };
        const timer = setTimeout(() => {
          child.kill('SIGTERM');
          finishRun({ rc: null, timedOut: true, stdout, stderr, pid: child.pid });
        }, options.timeoutMs ?? options.totalMs ?? 300000);
        child.stdout.on('data', chunk => { stdout += chunk; });
        child.stderr.on('data', chunk => { stderr += chunk; });
        child.on('error', error => {
          clearTimeout(timer);
          finishRun({ rc: 127, timedOut: false, stdout, stderr: `${stderr}${error.message}`, pid: child.pid });
        });
        if (options.background) {
          clearTimeout(timer);
          finishRun({ rc: 0, timedOut: false, stdout, stderr, pid: child.pid });
          return;
        }
        child.on('close', code => {
          clearTimeout(timer);
          finishRun({ rc: code, timedOut: false, stdout, stderr, pid: child.pid });
        });
      });
    },
  };
}

function plan(selected) {
  return {
    status: 'PREPARED_NOT_EXECUTED',
    readyForNativeExecution: false,
    runner: 'ubuntu-24.04',
    limits: { minutes: 45, jobs: 2, sccache: true, core: 0, bootSeconds: 240, activeSeconds: 60, totalSeconds: 300 },
    image: 'system-images;android-31;google_apis;x86_64',
    ndk: {
      sdkPackage: `ndk;${NDK}`,
      zip: 'https://dl.google.com/android/repository/android-ndk-r27d-linux.zip',
      size: 663956036,
      checksumSha1: '22105e410cf29afcf163760cc95522b9fb981121',
      revision: NDK,
      releaseName: 'r27d',
    },
    mode: selected.mode,
    emulatorOwnedByDriver: false,
    debugger: {
      platformRequired: 'remote-android',
      missingPlatform: 'PLATFORM_ABSENT',
      commandOrder: ['command source', 'command script import', 'platform select remote-android', 'platform connect', 'target create', 'target symbols add', 'script capture.arm()', 'process launch'],
      remoteExecutable: `${REMOTE}/consumer`,
      remoteLibraryPath: REMOTE,
      hostSymbols: 'HOST_SYMBOLS',
    },
    observation: {
      pairing: 'per tid stack; only rax 0 changes a generation; both ends need a full stack',
      truncated: 'complete=false and chain stays UNKNOWN',
      fatalAddress: 'dwarf variable or same-run fatal message only; historical rdi is not an argument',
      devicePid: 'identity file plus cmdline, starttime and port; host adb pid is never killed',
      sceneSuccess: false,
    },
  };
}

function writeBundle(out, selected, tools) {
  mkdirSync(out, { recursive: true });
  writeFileSync(join(out, 'route.json'), JSON.stringify(selected, null, 2) + '\n');
  writeFileSync(join(out, 'tool-preflight.json'), JSON.stringify(tools, null, 2) + '\n');
  writeFileSync(join(out, 'plan.json'), JSON.stringify(plan(selected), null, 2) + '\n');
  const commands = readFileSync(join(HERE, 'lldb-commands.txt'), 'utf8');
  if (commands.includes(OLD_ADDRESS) || commands.includes('pthread_mutex_lock') || commands.includes('/root') || commands.includes('arm()')) {
    fail('lldb command file is out of bounds');
  }
  writeFileSync(join(out, 'job-interface.json'), JSON.stringify({
    owner: 'metadata-platform',
    editWorkflow: false,
    readyForNativeExecution: false,
    packageDir: '.',
    packages: [`ndk;${NDK}`, 'platform-tools', 'emulator', 'system-images;android-31;google_apis;x86_64'],
    scripts: ['scene.mjs', 'capture.py', 'lldb-commands.txt'],
    requiredEnv: ['SCENE_COMMANDS', 'SCENE_OBSERVER', 'HOST_SYMBOLS', 'ANDROID_SDK_ROOT', 'ANDROID_SERIAL'],
    prepare: 'node scene.mjs self-test && node scene.mjs calibrate',
    execute: 'SCENE_EXECUTE=1 node scene.mjs execute',
    executeThisLane: 'not invoked',
    sameDrive: true,
    budget: { minutes: 45, jobs: 2, core: 0, activeSeconds: 60, totalSeconds: 300 },
    mode: 'preinit/full-image',
    forbid: ['runtime-fini', 'no-image', 'NoFP', 'retry', 'ndk-gdb', 'simulated sceneSuccess', 'host-pid kill', '-S before target'],
  }, null, 2) + '\n');
}

function assertOrder(result) {
  const launch = result.log.find(row => row.step === 'launch');
  if (!launch) fail('launch missing');
  const argv = launch.argv;
  if (argv.includes('-S')) fail('launch used -S');
  const at = prefix => argv.findIndex(item => item.startsWith(prefix));
  const source = at('command source ');
  const imported = at('command script import ');
  const selected = argv.indexOf('platform select remote-android');
  const connected = argv.indexOf('platform connect connect://127.0.0.1:5039');
  const created = at('target create ');
  const symbols = at('target symbols add ');
  const armed = argv.indexOf('script capture.arm()');
  const launched = at('process launch ');
  const order = [source, imported, selected, connected, created, symbols, armed, launched];
  if (order.some(index => index < 0) || order.some((index, place) => place > 0 && index < order[place - 1])) fail('command order');
  if (argv[imported] !== `command script import ${join(HERE, 'capture.py')}`) fail('import is not package-relative');
  if (!argv[launched].includes(`LD_LIBRARY_PATH=${REMOTE}`)) fail('remote library path missing');
  if (argv[symbols] === `target symbols add ${REMOTE}/consumer`) fail('symbols alias the remote executable');
  if (launch.stdout == null || launch.stderr == null) fail('launch output was dropped');
  const cleanup = result.log.find(row => row.step === 'cleanup-server');
  const joined = cleanup ? cleanup.argv.join(' ') : '';
  if (!joined.includes('pid=200') || joined.includes('pid=100')) fail('cleanup used the host adb pid');
  if (result.meta.hostPid === result.meta.devicePid) fail('device pid collapsed to host pid');
}

async function calibrate(judge) {
  const cases = [];
  const expect = async (name, script, outcome, absent) => {
    const previous = process.env.HOST_SYMBOLS;
    if (script.symbols === undefined) process.env.HOST_SYMBOLS = '/symbols/libcangjie-runtime.so';
    else if (script.symbols === null) delete process.env.HOST_SYMBOLS;
    else process.env.HOST_SYMBOLS = script.symbols;
    const io = scripted(script);
    const result = await drive(io, '/sdk', 'emulator-5554', judge);
    if (previous === undefined) delete process.env.HOST_SYMBOLS;
    else process.env.HOST_SYMBOLS = previous;
    if (result.outcome !== outcome || result.sceneSuccess || result.productPass || result.readyForNativeExecution || !result.simulated) {
      fail(`${name} -> ${result.outcome}`);
    }
    if (absent && result.log.some(row => row.step === absent)) fail(`${name} ran ${absent}`);
    if (result.outcome === 'NOT_REPRODUCED' && result.observer !== 'PASS') fail(`${name} skipped the observer`);
    cases.push({ name, outcome: result.outcome, steps: result.log.map(row => row.step), simulated: true, sceneSuccess: false, observer: result.observer || null });
    return result;
  };
  await expect('missing-tools', { absent: true, steps: {} }, 'TOOL_ABSENT', 'platform-list');
  await expect('symbols-absent', { symbols: null, platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: {} }, 'SYMBOLS_ABSENT', 'launch');
  await expect('version-absent', { platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: { 'host-version': { rc: 0, stdout: '', stderr: '' } } }, 'VERSION_ABSENT', 'launch');
  await expect('platform-missing', { platform: 'remote-linux remote-gdb-server', serial: 'emulator-5554\tdevice', steps: {} }, 'PLATFORM_ABSENT', 'launch');
  await expect('connect-fail', { platform: 'remote-android', serial: '', steps: { 'adb-devices': { rc: 1, stdout: '', stderr: '' } } }, 'CONNECT_FAIL', 'launch');
  await expect('server-unconfirmed', { platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: { 'server-identity': { rc: 0, stdout: 'PORT_OWNED=0\n', stderr: '' } } }, 'SERVER_UNCONFIRMED', 'launch');
  const unconfirmed = cases.at(-1);
  if (unconfirmed.steps.includes('cleanup-server')) fail('unconfirmed server was killed');
  await expect('timeout', { platform: 'remote-android', serial: 'emulator-5554\tdevice', timeout: 'launch', steps: {} }, 'UNCAPTURED', null);
  const natural = await expect('natural-exit', { platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: {} }, 'NOT_REPRODUCED', null);
  assertOrder(natural);
  await expect('launch-failed', { platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: { launch: { rc: 1, stdout: '', stderr: 'launch error' } } }, 'LAUNCH_FAILED', null);
  await expect('log-missing', { platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: { launch: { rc: 0, stdout: null, stderr: null } } }, 'LOG_MISSING', null);
  await expect('maps-absent', { platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: { launch: { rc: 0, stdout: 'EXIT_SHELL child_pid=2400 realtime_start=1763240464.000999999 realtime_end=1763240465.000999999 native_wait_rc=0\n', stderr: '' } } }, 'MAPS_ABSENT', null);
  const broken = quietObservation().replace('TARGET elf-published-package executed=1 pass=1', '');
  await expect('observer-unknown', { platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: { launch: { rc: 0, stdout: broken, stderr: '' } } }, 'OBSERVER_UNKNOWN', null);
  const fatalText = 'SIGABRT\nAbort message: FORTIFY: pthread_mutex_lock called on a destroyed mutex (0x1234)\n';
  await expect('fatal-unrecovered', { platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: { launch: { rc: 0, stdout: fatalText, stderr: '' } } }, 'FATAL_UNKNOWN', null);
  const frames = [{ pc: 1, module: 'libc.so', name: 'pthread_mutex_destroy' }, { pc: 2, module: 'libconsumer.so', name: 'owner' }];
  const fatalFrames = [{ pc: 3, module: 'libc.so', name: 'pthread_mutex_lock' }, { pc: 4, module: 'libconsumer.so', name: 'user' }];
  const state = { complete: true, dead: { '4660': { state: 'dead', event: { frames, tid: 1624, kind: 'destroy', return: 0 } } } };
  const attributedText = `${fatalText}EVENT_JSON:${JSON.stringify(state)}\nFATAL_FRAMES_JSON:${JSON.stringify(fatalFrames)}\n`;
  const attributed = await expect('fatal-attributed', { platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: { launch: { rc: 0, stdout: attributedText, stderr: '' } } }, 'FATAL_ATTRIBUTED', null);
  if (attributed.sceneSuccess) fail('attributed scene was marked success');
  const thrown = await expect('launch-threw', { platform: 'remote-android', serial: 'emulator-5554\tdevice', throw: 'launch', steps: {} }, 'DRIVER_EXCEPTION', null);
  if (!thrown.log.some(row => row.step === 'cleanup-server' && row.argv.join(' ').includes('pid=200'))) fail('exception path skipped device cleanup');
  return cases;
}

const argv = process.argv.slice(2);

if (argv[0] === 'self-test') {
  const source = process.env.SCENE_COMMANDS;
  if (!source) fail('SCENE_COMMANDS unset');
  const records = JSON.parse(readFileSync(source, 'utf8'));
  const selected = route(records);
  if (selected.launch.at(-1) !== 'preinit') fail('self-test launch');
  let rejected = false;
  try {
    route([{ name: 'fixture-link', args: ['/src/elf-fixture.cpp'] }, { name: 'consumer-link', args: ['/src/elf-consumer.cpp'] }, { name: 'preinit', args: ['consumer', 'fixture.so', 'runtime-fini'] }]);
  } catch { rejected = true; }
  if (!rejected) fail('fini route was accepted');
  const tools = preflight(process.env.ANDROID_SDK_ROOT || '/usr/local/lib/android/sdk');
  if (Object.values(tools.tools).some(item => item.present)) fail('local official tool unexpectedly present');
  writeBundle(HERE, selected, tools);
  console.log('SELFTEST_PASS');
  process.exit(0);
}

if (argv[0] === 'calibrate') {
  const source = process.env.SCENE_COMMANDS;
  if (!source) fail('SCENE_COMMANDS unset');
  route(JSON.parse(readFileSync(source, 'utf8')));
  const python = spawn('python3', [join(HERE, 'capture.py'), 'self-test'], { stdio: ['ignore', 'pipe', 'pipe'] });
  let pyOut = '';
  python.stdout.on('data', chunk => { pyOut += chunk; });
  python.on('close', async code => {
    try {
      if (code !== 0 || !pyOut.includes('PAIRING_PASS')) fail('pairing self-test');
      const judge = await loadJudge();
      if (typeof judge !== 'function') fail('SCENE_OBSERVER unset');
      const cases = await calibrate(judge);
      writeFileSync(join(HERE, 'calibration.json'), JSON.stringify({ simulated: true, sceneSuccess: false, readyForNativeExecution: false, cases }, null, 2) + '\n');
      console.log('CALIBRATE_PASS');
    } catch (error) {
      console.error(error.stack || error.message);
      process.exit(1);
    }
  });
} else if (argv[0] === 'execute') {
  if (process.env.SCENE_EXECUTE !== '1') {
    console.error('NOT_AUTHORIZED');
    process.exit(2);
  }
  const sdk = process.env.ANDROID_SDK_ROOT;
  if (!sdk) fail('ANDROID_SDK_ROOT unset');
  const source = process.env.SCENE_COMMANDS;
  route(JSON.parse(readFileSync(source, 'utf8')));
  const judge = await loadJudge();
  const result = await drive(realIO(), sdk, process.env.ANDROID_SERIAL || 'emulator-5554', judge);
  writeFileSync(join(HERE, 'execute-result.json'), JSON.stringify(result, null, 2) + '\n');
  console.log(result.outcome);
  process.exit(result.outcome === 'NOT_REPRODUCED' || result.outcome.startsWith('FATAL_') ? 0 : 1);
} else {
  fail('usage: scene.mjs self-test|calibrate|execute');
}
