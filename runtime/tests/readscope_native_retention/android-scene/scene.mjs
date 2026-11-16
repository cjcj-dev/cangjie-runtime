#!/usr/bin/env zx
import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';

const HERE = dirname(new URL(import.meta.url).pathname);
const OLD_ADDRESS = '0x7b33ef1eb08c';
const RUNTIME = 'd51c34a06cfd980d0e2694514e2a41421ac41b83';
const ORIGINAL_SO = 'aac5c4456fc9746243a2a15d22beac6a52ee27f269a94cd0e839315bc080db51';
const OBSERVER = '761b0ad956d975488b984e419ef4a803711e2635';
const NDK = '27.3.13750724';
const REMOTE = '/data/local/tmp/readscope-lifetime';

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
  return { sdk, kvm: existsSync('/dev/kvm'), tools, executed: false };
}

function finish(outcome, simulated, log, extra = {}) {
  return {
    outcome,
    simulated,
    sceneSuccess: false,
    productPass: false,
    originalFault: 'OPEN',
    log,
    ...extra,
  };
}

async function drive(io, sdk, serial = 'emulator-5554') {
  const log = [];
  const started = { server: null, forward: false };
  const tool = name => join(sdk, TOOLS[name]);
  const run = async (step, argv, options = {}) => {
    const result = await io.run(step, argv, options);
    log.push({ step, argv, rc: result.rc, timedOut: Boolean(result.timedOut) });
    return result;
  };
  const cleanup = async () => {
    if (started.server !== null) {
      await run('cleanup-server', [tool('adb'), '-s', serial, 'shell', 'kill', '-TERM', String(started.server)]);
    }
    if (started.forward) {
      await run('cleanup-forward', [tool('adb'), '-s', serial, 'forward', '--remove', 'tcp:5039']);
    }
  };
  for (const name of ['hostLldb', 'server', 'adb']) {
    if (!io.stat(tool(name))) return finish('TOOL_ABSENT', io.simulated, log, { missing: name });
  }
  const platforms = await run('platform-list', [tool('hostLldb'), '-b', '-o', 'platform list', '-o', 'quit'], { timeoutMs: 30000 });
  if (platforms.timedOut) return finish('UNCAPTURED', io.simulated, log);
  if (platforms.rc !== 0) return finish('PLATFORM_LIST_FAIL', io.simulated, log);
  if (!/(^|\s)remote-android(\s|$)/.test(platforms.stdout || '')) return finish('PLATFORM_ABSENT', io.simulated, log);
  const devices = await run('adb-devices', [tool('adb'), 'devices'], { timeoutMs: 30000 });
  if (devices.rc !== 0 || !(devices.stdout || '').includes(serial)) {
    return finish('CONNECT_FAIL', io.simulated, log);
  }
  const pushed = await run('push-server', [tool('adb'), '-s', serial, 'push', tool('server'), `${REMOTE}/lldb-server`]);
  if (pushed.rc !== 0) return finish('CONNECT_FAIL', io.simulated, log);
  const server = await run('start-server', [tool('adb'), '-s', serial, 'shell', `${REMOTE}/lldb-server`, 'platform', '--listen', '*:5039', '--server'], { background: true });
  if (server.rc !== 0 || typeof server.pid !== 'number') return finish('CONNECT_FAIL', io.simulated, log);
  started.server = server.pid;
  const forwarded = await run('forward', [tool('adb'), '-s', serial, 'forward', 'tcp:5039', 'tcp:5039']);
  if (forwarded.rc !== 0) {
    await cleanup();
    return finish('CONNECT_FAIL', io.simulated, log);
  }
  started.forward = true;
  const launch = await run('launch', [
    tool('hostLldb'), '-b', '-S', join(HERE, 'lldb-commands.txt'),
    '-o', 'platform select remote-android',
    '-o', 'platform connect connect://127.0.0.1:5039',
    '-o', `target create ${REMOTE}/consumer`,
    '-o', `process launch -- ${REMOTE}/fixture.so preinit`,
  ], { timeoutMs: 60000, totalMs: 300000 });
  if (launch.timedOut) {
    await cleanup();
    return finish('UNCAPTURED', io.simulated, log);
  }
  await cleanup();
  if (launch.fatal) {
    const chain = launch.association?.chain === 'DESTROY0_THEN_LOCK' && launch.association?.complete === true
      ? 'FATAL_ATTRIBUTED' : 'FATAL_UNKNOWN';
    return finish(chain, io.simulated, log, { association: launch.association || { chain: 'UNKNOWN' } });
  }
  return finish('NOT_REPRODUCED', io.simulated, log);
}

function scripted(script) {
  const runs = [];
  return {
    simulated: true,
    stat(path) { return script.absent ? false : !String(path).endsWith('/missing'); },
    async run(step, argv, options = {}) {
      runs.push(step);
      const row = script.steps[step] || { rc: 0, stdout: '', pid: options.background ? 4242 : undefined };
      if (step === 'platform-list' && script.platform) row.stdout = script.platform;
      if (step === 'adb-devices' && script.serial) row.stdout = script.serial;
      if (script.timeout === step) return { rc: null, timedOut: true, stdout: '' };
      return row;
    },
    runs,
  };
}

function realIO() {
  return {
    simulated: false,
    stat(path) { return existsSync(path); },
    run(step, argv, options = {}) {
      return new Promise(resolve => {
        const child = spawn(argv[0], argv.slice(1), { stdio: ['ignore', 'pipe', 'pipe'] });
        let stdout = '';
        let stderr = '';
        const limit = options.timeoutMs ?? options.totalMs ?? 300000;
        const timer = setTimeout(() => {
          child.kill('SIGTERM');
          resolve({ rc: null, timedOut: true, stdout, stderr, pid: child.pid });
        }, limit);
        child.stdout.on('data', chunk => { stdout += chunk; });
        child.stderr.on('data', chunk => { stderr += chunk; });
        if (options.background) {
          clearTimeout(timer);
          resolve({ rc: 0, timedOut: false, stdout, stderr, pid: child.pid });
          return;
        }
        child.on('close', code => {
          clearTimeout(timer);
          resolve({ rc: code, timedOut: false, stdout, stderr, pid: child.pid });
        });
      });
    },
  };
}

function plan(selected) {
  return {
    status: 'PREPARED_NOT_EXECUTED',
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
      commands: join(HERE, 'lldb-commands.txt'),
      capture: join(HERE, 'capture.py'),
    },
    observation: {
      pairing: 'per tid stack; only rax 0 changes a generation',
      truncated: 'complete=false and chain stays UNKNOWN',
      fatalAddress: 'dwarf variable or same-run fatal message only; historical rdi is not an argument',
      lockBreakpoint: 'absent',
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
  if (commands.includes(OLD_ADDRESS) || commands.includes('pthread_mutex_lock')) fail('lldb command file is out of bounds');
  writeFileSync(join(out, 'job-interface.json'), JSON.stringify({
    owner: 'metadata-platform',
    editWorkflow: false,
    packages: [`ndk;${NDK}`, 'platform-tools', 'emulator', 'system-images;android-31;google_apis;x86_64'],
    scripts: ['scene.mjs', 'capture.py', 'lldb-commands.txt'],
    prepare: 'node scene.mjs self-test && node scene.mjs calibrate',
    execute: 'SCENE_EXECUTE=1 node scene.mjs execute',
    executeThisLane: 'not invoked',
    budget: { minutes: 45, jobs: 2, core: 0, activeSeconds: 60, totalSeconds: 300 },
    mode: 'preinit/full-image',
    forbid: ['runtime-fini', 'no-image', 'NoFP', 'retry', 'ndk-gdb', 'simulated sceneSuccess'],
  }, null, 2) + '\n');
}

async function calibrate(selected) {
  if (selected.mode !== 'preinit') fail('calibrate route');
  const cases = [];
  const expect = async (name, script, outcome, firstAbsent) => {
    const io = scripted(script);
    const result = await drive(io, '/sdk', 'emulator-5554');
    if (result.outcome !== outcome || result.sceneSuccess || result.productPass || !result.simulated) {
      fail(`${name} -> ${result.outcome}`);
    }
    if (firstAbsent && result.log.some(row => row.step === firstAbsent)) fail(`${name} ran ${firstAbsent}`);
    if (result.outcome === 'PASS' || result.outcome === 'SCENE_SUCCESS') fail(`${name} used a scene-success name`);
    cases.push({ name, outcome: result.outcome, steps: result.log.map(row => row.step), simulated: true, sceneSuccess: false });
  };
  await expect('missing-tools', { absent: true, steps: {} }, 'TOOL_ABSENT', 'platform-list');
  await expect('platform-missing', { platform: 'remote-linux remote-gdb-server', serial: 'emulator-5554\tdevice', steps: {} }, 'PLATFORM_ABSENT', 'launch');
  await expect('connect-fail', { platform: 'remote-android', serial: '', steps: { 'adb-devices': { rc: 1, stdout: '' } } }, 'CONNECT_FAIL', 'launch');
  await expect('timeout', { platform: 'remote-android', serial: 'emulator-5554\tdevice', timeout: 'launch', steps: {} }, 'UNCAPTURED', null);
  await expect('natural-exit', { platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: {} }, 'NOT_REPRODUCED', null);
  const fatal = scripted({
    platform: 'remote-android', serial: 'emulator-5554\tdevice',
    steps: { launch: { rc: 0, fatal: true, association: { chain: 'UNKNOWN', source: 'argument-not-recovered' } } },
  });
  const fatalResult = await drive(fatal, '/sdk');
  if (fatalResult.outcome !== 'FATAL_UNKNOWN' || fatalResult.sceneSuccess) fail('fatal rdi case');
  cases.push({ name: 'fatal-unrecovered', outcome: fatalResult.outcome, simulated: true, sceneSuccess: false });
  const attributed = scripted({
    platform: 'remote-android', serial: 'emulator-5554\tdevice',
    steps: { launch: { rc: 0, fatal: true, association: { chain: 'DESTROY0_THEN_LOCK', complete: true, source: 'fatal-message' } } },
  });
  const attributedResult = await drive(attributed, '/sdk');
  if (attributedResult.outcome !== 'FATAL_ATTRIBUTED' || attributedResult.sceneSuccess) fail('attributed case labeled success');
  cases.push({ name: 'fatal-message-simulated', outcome: attributedResult.outcome, simulated: true, sceneSuccess: false });
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
  } catch {
    rejected = true;
  }
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
  const selected = route(JSON.parse(readFileSync(source, 'utf8')));
  const python = spawn('python3', [join(HERE, 'capture.py'), 'self-test'], { stdio: ['ignore', 'pipe', 'pipe'] });
  let pyOut = '';
  python.stdout.on('data', chunk => { pyOut += chunk; });
  python.on('close', async code => {
    try {
      if (code !== 0 || !pyOut.includes('PAIRING_PASS')) fail('pairing self-test');
      const cases = await calibrate(selected);
      writeFileSync(join(HERE, 'calibration.json'), JSON.stringify({ simulated: true, sceneSuccess: false, cases }, null, 2) + '\n');
      console.log('CALIBRATE_PASS');
    } catch (error) {
      console.error(error.message);
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
  const selected = route(JSON.parse(readFileSync(source, 'utf8')));
  const result = await drive(realIO(), sdk);
  writeFileSync(join(HERE, 'execute-result.json'), JSON.stringify(result, null, 2) + '\n');
  console.log(result.outcome);
  process.exit(result.outcome === 'NOT_REPRODUCED' || result.outcome.startsWith('FATAL_') ? 0 : 1);
} else {
  fail('usage: scene.mjs self-test|calibrate|execute');
}
