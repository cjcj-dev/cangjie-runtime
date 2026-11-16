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
const ACTIVE_MS = 60000;
const TOTAL_MS = 300000;
const RUNTIME_PATH = `${REMOTE}/libcangjie-runtime.so`;

const TOOLS = {
  hostLldb: `ndk/${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/lldb`,
  hostWrapper: `ndk/${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/lldb.sh`,
  readelf: `ndk/${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf`,
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
    shellWait: 'UNKNOWN',
    businessExit: null,
    log,
    ...extra,
    shellWait: 'UNKNOWN',
    sceneSuccess: false,
    productPass: false,
    readyForNativeExecution: false,
  };
}

function stackOk(frames) {
  return Array.isArray(frames) && frames.length >= 2 && frames.every(frame => frame && frame.pc != null && frame.module && frame.name);
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
echo DEVICE_PID=$pid
echo DEVICE_START=$start
echo DEVICE_NOW=$now
echo CMDLINE=$cmd
echo '---FD---'
ls -l /proc/$pid/fd 2>/dev/null || true
echo '---TCP---'
cat /proc/net/tcp 2>/dev/null || true
echo '---TCP6---'
cat /proc/net/tcp6 2>/dev/null || true`;
}

function killShell(pid) {
  return `pid=${pid}
if [ ! -d /proc/$pid ]; then echo UNCONFIRMED; exit 3; fi
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

function section(text, name) {
  const body = String(text || '');
  const start = body.indexOf(`---${name}---`);
  if (start < 0) return '';
  const rest = body.slice(start + name.length + 6);
  const next = rest.search(/\n---[A-Z0-9]+---/);
  return next < 0 ? rest : rest.slice(0, next);
}

function listenOwned(text) {
  const inodes = new Set([...section(text, 'FD').matchAll(/socket:\[(\d+)\]/g)].map(match => match[1]));
  const tables = `${section(text, 'TCP')}\n${section(text, 'TCP6')}`;
  for (const line of tables.split('\n')) {
    const parts = line.trim().split(/\s+/);
    if (parts.length < 10 || !/^\d+:$/.test(parts[0])) continue;
    const local = parts[1].split(':');
    const port = (local[1] || '').toLowerCase();
    const state = parts[3];
    const inode = parts[9];
    if (port === '13af' && state === '0A' && inodes.has(inode)) return inode;
  }
  return null;
}

function parseIdentity(text, hostPid) {
  const grab = name => {
    const matched = String(text || '').match(new RegExp(`^${name}=(.*)$`, 'm'));
    return matched ? matched[1].trim() : '';
  };
  const pid = grab('DEVICE_PID');
  const start = grab('DEVICE_START');
  const cmdline = grab('CMDLINE');
  if (!/^\d+$/.test(pid) || !/^\d+$/.test(start) || grab('DEVICE_NOW') !== start) return null;
  if (hostPid != null && pid === String(hostPid)) return null;
  if (!cmdline.includes(`${REMOTE}/lldb-server`)) return null;
  if (!listenOwned(text)) return null;
  return { pid, start, cmdline };
}

function forwardBusy(text, serial) {
  return String(text || '').split('\n').some(line => {
    const parts = line.trim().split(/\s+/);
    return parts[0] === serial && parts[1] === 'tcp:5039';
  });
}

function imageModule(text) {
  const found = [];
  for (const line of String(text || '').split('\n')) {
    const matched = line.match(/^\[\s*\d+\]\s+([0-9A-Fa-f]+(?:-[0-9A-Fa-f]+)*)\s+(0x[0-9A-Fa-f]+)\s+(\S+)/);
    if (!matched || matched[3] !== RUNTIME_PATH) continue;
    found.push({ uuid: matched[1].toLowerCase(), base: BigInt(matched[2]), path: matched[3] });
  }
  if (found.length === 0) return null;
  const same = found.every(item => item.base === found[0].base && item.path === found[0].path);
  return same ? found[0] : { conflict: true };
}

function mapsBase(text) {
  let base = null;
  for (const line of String(text || '').split('\n')) {
    const matched = line.match(/^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+\S+\s+\S+\s+\S+\s+(\S+)$/);
    if (!matched || matched[2] !== RUNTIME_PATH) continue;
    const start = BigInt(`0x${matched[1]}`);
    if (base === null || start < base) base = start;
  }
  return base;
}

function noteStatus(text) {
  return /^\s*Build ID:/m.test(String(text || '')) ? 'PRESENT' : 'ABSENT';
}

function liveParts(text) {
  const examinable = new Set(['stopped', 'crashed', 'suspended']);
  let state = 'before';
  let pid = null;
  const image = [];
  const maps = [];
  const files = [];
  let inMaps = false;
  for (const line of String(text || '').split('\n')) {
    if (/^Process \d+ exited with status = /.test(line) || /^Process \d+ detached$/.test(line)) {
      state = 'dead';
      inMaps = false;
      continue;
    }
    const live = line.match(/^MODULE_LIVE pid=(\d+) state=(\S+)$/);
    if (live) {
      pid = live[1];
      state = examinable.has(live[2]) ? 'live' : 'dead';
      inMaps = false;
      continue;
    }
    if (line === 'MODULE_UNAVAILABLE') {
      state = 'dead';
      inMaps = false;
      continue;
    }
    if (line === 'MAPS_BEGIN') {
      inMaps = state === 'live';
      continue;
    }
    if (line === 'MAPS_END') {
      inMaps = false;
      continue;
    }
    if (state !== 'live') continue;
    if (inMaps) maps.push(line);
    else if (line.startsWith('FILE_SHA ')) files.push(line.slice('FILE_SHA '.length));
    else if (/^\[\s*\d+\]/.test(line)) image.push(line);
  }
  return { pid, image: image.join('\n'), maps: maps.join('\n'), file: files.join('\n') };
}

function fileSha(text) {
  const matched = String(text || '').match(/^([0-9a-f]{64})\s+\S+$/m);
  return matched ? matched[1] : null;
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
    '-o', `process launch --stop-at-entry true --environment LD_LIBRARY_PATH=${REMOTE} -- ${REMOTE}/fixture.so preinit`,
    '-o', 'script capture.live_module()',
    '-o', 'image list',
    '-o', 'process continue',
    '-o', 'process status',
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

function sbProcess(text) {
  const launched = [];
  const exits = [];
  const states = [];
  const stops = [];
  const restarts = [];
  let current = null;
  for (const line of String(text || '').split('\n')) {
    let matched = line.match(/^Process (\d+) launched: '([^']*)' \(([^)]*)\)$/);
    if (matched) {
      current = matched[1];
      launched.push({ pid: matched[1], path: matched[2], arch: matched[3] });
      continue;
    }
    matched = line.match(/^Process (\d+) exited with status = (-?\d+) \(0x[0-9a-fA-F]+\)\s*(.*)$/);
    if (matched) {
      current = matched[1];
      exits.push({ pid: matched[1], status: Number(matched[2]), description: matched[3].trim() });
      continue;
    }
    matched = line.match(/^Process (\d+) stopped and restarted(?:: (.*))?$/);
    if (matched) {
      current = matched[1];
      restarts.push({ pid: matched[1], reason: (matched[2] || '').trim() || null });
      continue;
    }
    matched = line.match(/^Process (\d+) (stopped|crashed|detached|suspended|exited|running|launching|stepping|connected|attaching|invalid|unloaded|unknown)$/);
    if (matched) {
      current = matched[1];
      states.push({ pid: matched[1], state: matched[2] });
      continue;
    }
    matched = line.match(/^Process (\d+) is running\.$/);
    if (matched) {
      current = matched[1];
      states.push({ pid: matched[1], state: 'running' });
      continue;
    }
    matched = line.match(/^\* thread #\d+.*\bstop reason = (.+)$/);
    if (matched) stops.push({ pid: current, reason: matched[1].trim() });
  }
  return { launched, exits, states, stops, restarts };
}

async function drive(io, sdk, serial = 'emulator-5554') {
  const log = [];
  const meta = { cleanup: 'NOT_STARTED', hostReap: 'NOT_STARTED', hostPid: null, devicePid: null, remoteAndroid: false, nativeWait: 'ABSENT' };
  const started = { device: null, forward: false };
  const tool = name => join(sdk, TOOLS[name]);
  const origin = typeof io.now === 'function' ? io.now('start') : Date.now();
  const left = step => TOTAL_MS - ((typeof io.now === 'function' ? io.now(step) : Date.now()) - origin);
  const run = async (step, argv, options = {}) => {
    const cap = options.stop ? (options.timeoutMs ?? 5000) : (options.active ? ACTIVE_MS : (options.timeoutMs ?? 30000));
    const slice = options.stop ? cap : Math.min(cap, left(step));
    if (!options.stop && slice <= 0) {
      const skipped = { rc: null, timedOut: true, stdout: '', stderr: '', skipped: true };
      log.push({ step, argv, rc: null, timedOut: true, skipped: true, timeoutMs: 0, stdout: '', stderr: '' });
      return skipped;
    }
    const result = await io.run(step, argv, { ...options, timeoutMs: slice });
    log.push({
      step, argv, rc: result.rc, timedOut: Boolean(result.timedOut), timeoutMs: slice,
      stdout: typeof result.stdout === 'string' ? result.stdout : null,
      stderr: typeof result.stderr === 'string' ? result.stderr : null,
    });
    return result;
  };
  const cleanup = async () => {
    if (meta.hostPid != null && typeof io.reap === 'function') {
      const reaped = await io.reap(meta.hostPid);
      log.push({ step: 'reap-host', argv: ['reap', String(meta.hostPid)], rc: reaped.rc, timedOut: false, stdout: reaped.stdout ?? '', stderr: reaped.stderr ?? '' });
      meta.hostReap = reaped.rc === 0 && String(reaped.stdout || '').includes('REAPED') ? 'REAPED' : 'UNCONFIRMED';
    }
    if (started.device) {
      const probe = await run('cleanup-probe', [tool('adb'), '-s', serial, 'shell', identityShell()], { timeoutMs: 5000, stop: true });
      const still = parseIdentity(probe.stdout, meta.hostPid);
      if (!still || still.pid !== started.device.pid || still.start !== started.device.start) meta.cleanup = 'UNCONFIRMED';
      else {
        const cleaned = await run('cleanup-server', [tool('adb'), '-s', serial, 'shell', killShell(still.pid)], { timeoutMs: 5000, stop: true });
        meta.cleanup = cleaned.rc === 0 && String(cleaned.stdout || '').includes('REAPED') && !String(cleaned.argv || '').includes(String(meta.hostPid)) ? 'REAPED' : 'UNCONFIRMED';
      }
    }
    if (started.forward) {
      const removed = await run('cleanup-forward', [tool('adb'), '-s', serial, 'forward', '--remove', 'tcp:5039'], { timeoutMs: 5000, stop: true });
      if (removed.rc !== 0) meta.cleanup = 'UNCONFIRMED';
    }
  };
  const attempt = async () => {
    for (const name of ['hostLldb', 'readelf', 'server', 'adb']) {
      if (!io.stat(tool(name))) { return finish('TOOL_ABSENT', io.simulated, log, { missing: name, meta }); }
    }
    const commands = io.readText(join(HERE, 'lldb-commands.txt'));
    if (commands.includes('/root') || commands.includes('arm()') || commands.includes('import ')) {
      return finish('COMMANDS_INVALID', io.simulated, log, { meta });
    }
    const symbols = process.env.HOST_SYMBOLS;
    if (!symbols || symbols === `${REMOTE}/consumer`) { return finish('SYMBOLS_ABSENT', io.simulated, log, { meta }); }
    const hostVer = await run('host-version', [tool('hostLldb'), '--version']);
    if (hostVer.timedOut) { return finish('UNCAPTURED', io.simulated, log, { meta }); }
    if (hostVer.rc !== 0 || !hostVer.stdout) { return finish('VERSION_ABSENT', io.simulated, log, { meta }); }
    const platforms = await run('platform-list', [tool('hostLldb'), '-b', '-o', 'platform list', '-o', 'quit']);
    if (platforms.timedOut) { return finish('UNCAPTURED', io.simulated, log, { meta }); }
    if (platforms.rc !== 0) { return finish('PLATFORM_LIST_FAIL', io.simulated, log, { meta }); }
    if (!/(^|\s)remote-android(\s|$)/.test(platforms.stdout || '')) { return finish('PLATFORM_ABSENT', io.simulated, log, { meta }); }
    meta.remoteAndroid = true;
    const devices = await run('adb-devices', [tool('adb'), 'devices']);
    if (devices.rc !== 0 || !(devices.stdout || '').includes(serial)) { return finish('CONNECT_FAIL', io.simulated, log, { meta }); }
    const pushed = await run('push-server', [tool('adb'), '-s', serial, 'push', tool('server'), `${REMOTE}/lldb-server`]);
    if (pushed.rc !== 0) { return finish('CONNECT_FAIL', io.simulated, log, { meta }); }
    const serverVer = await run('server-version', [tool('adb'), '-s', serial, 'shell', `${REMOTE}/lldb-server version`]);
    if (serverVer.rc !== 0 || !serverVer.stdout) { return finish('VERSION_ABSENT', io.simulated, log, { meta }); }
    const server = await run('start-server', [tool('adb'), '-s', serial, 'shell', startShell()], { background: true });
    meta.hostPid = typeof server.pid === 'number' ? server.pid : null;
    if (server.rc !== 0 || server.timedOut) { return finish(server.timedOut ? 'UNCAPTURED' : 'CONNECT_FAIL', io.simulated, log, { meta }); }
    const identity = await run('server-identity', [tool('adb'), '-s', serial, 'shell', identityShell()]);
    started.device = parseIdentity(identity.stdout, meta.hostPid);
    meta.devicePid = started.device ? started.device.pid : null;
    if (!started.device) { return finish('SERVER_UNCONFIRMED', io.simulated, log, { meta }); }
    const listed = await run('forward-list', [tool('adb'), '-s', serial, 'forward', '--list']);
    if (listed.rc !== 0 || listed.timedOut) { return finish(listed.timedOut ? 'UNCAPTURED' : 'FORWARD_UNKNOWN', io.simulated, log, { meta }); }
    if (forwardBusy(listed.stdout, serial)) { return finish('FORWARD_BUSY', io.simulated, log, { meta }); }
    const forwarded = await run('forward', [tool('adb'), '-s', serial, 'forward', 'tcp:5039', 'tcp:5039']);
    if (forwarded.rc !== 0 || forwarded.timedOut) { return finish(forwarded.timedOut ? 'UNCAPTURED' : 'CONNECT_FAIL', io.simulated, log, { meta }); }
    started.forward = true;
    const so = process.env.RUNTIME_SO;
    const note = so ? await run('runtime-note', [tool('readelf'), '-n', so]) : null;
    const hashed = so ? await run('runtime-hash', ['sha256sum', so]) : null;
    const launch = await run('launch', launchArgv(tool('hostLldb')), { active: true });
    if (launch.timedOut) { return finish('UNCAPTURED', io.simulated, log, { meta }); }
    const logcat = await run('logcat', [tool('adb'), '-s', serial, 'logcat', '-d', '-v', 'epoch']);
    const crash = await run('crash', [tool('adb'), '-s', serial, 'logcat', '-d', '-b', 'crash', '-v', 'epoch']);
    if (typeof launch.stdout !== 'string' || typeof launch.stderr !== 'string') {
      return finish('LOG_MISSING', io.simulated, log, { meta });
    }
    const text = `${launch.stdout}\n${launch.stderr}`;
    const fatal = /FORTIFY:|\bSIGABRT\b|Abort message:/.test(text);
    const live = liveParts(launch.stdout);
    const image = imageModule(live.image);
    const noted = note && note.rc === 0 ? noteStatus(note.stdout) : null;
    const digest = hashed && hashed.rc === 0 ? fileSha(hashed.stdout) : null;
    meta.buildId = null;
    meta.buildIdStatus = 'ABSENT';
    meta.lldbUuid = image && !image.conflict ? image.uuid : null;
    meta.module = 'ABSENT';
    meta.nativeWait = 'UNKNOWN';
    const proc = sbProcess(text);
    const ledger = { buildId: null, meta, lldbRc: launch.rc, businessExit: null, process: proc, captures: { logcat: logcat.rc ?? null, crash: crash.rc ?? null } };
    if (!so || !digest || noted !== 'ABSENT') {
      if (digest && digest !== ORIGINAL_SO) return finish('FILE_IDENTITY_MISMATCH', io.simulated, log, ledger);
      if (noted === 'PRESENT') return finish('NOTE_PRESENT', io.simulated, log, ledger);
      return finish('FILE_IDENTITY_ABSENT', io.simulated, log, ledger);
    }
    if (digest !== ORIGINAL_SO) return finish('FILE_IDENTITY_MISMATCH', io.simulated, log, ledger);
    if (!image) return finish('MODULE_PATH_ABSENT', io.simulated, log, ledger);
    if (image.conflict) return finish('BASE_MISMATCH', io.simulated, log, ledger);
    const deviceSha = fileSha(live.file);
    if (!deviceSha) return finish('FILE_IDENTITY_ABSENT', io.simulated, log, ledger);
    if (deviceSha !== ORIGINAL_SO) return finish('FILE_IDENTITY_MISMATCH', io.simulated, log, ledger);
    const base = mapsBase(live.maps);
    if (base === null) return finish('MAPS_ABSENT', io.simulated, log, ledger);
    if (base !== image.base) return finish('BASE_MISMATCH', io.simulated, log, ledger);
    if (proc.launched.length === 0) return finish('PROCESS_ABSENT', io.simulated, log, ledger);
    if (proc.launched.length !== 1) return finish('DUPLICATE_PID', io.simulated, log, ledger);
    const pid = proc.launched[0].pid;
    if (live.pid !== pid) return finish('MAPS_ABSENT', io.simulated, log, ledger);
    if (proc.exits.some(row => row.pid !== pid) || proc.states.some(row => row.pid !== pid) || proc.restarts.some(row => row.pid !== pid)) {
      return finish('DUPLICATE_PID', io.simulated, log, ledger);
    }
    meta.module = 'LINKED';
    meta.loadBase = `0x${image.base.toString(16)}`;
    const exitRow = proc.exits.find(row => row.pid === pid) || null;
    const stops = proc.stops.filter(row => row.pid === pid && row.reason);
    const fatalStop = stops.some(row => /\bsignal SIGABRT\b|\bsignal SIGSEGV\b/.test(row.reason));
    const detached = proc.states.some(row => row.pid === pid && row.state === 'detached');
    let linked = null;
    if (fatalStop || fatal) {
      const state = jsonLine(launch.stdout, 'EVENT_JSON:');
      const fatalFrames = jsonLine(launch.stdout, 'FATAL_FRAMES_JSON:') || state?.fatal?.frames || null;
      linked = await decode({ state: state || { complete: false }, dwarf: null, messageAddress: messageAddress(text), fatalFrames });
    }
    if (fatalStop && !exitRow) return finish('FATAL_STOP_UNEXITED', io.simulated, log, { ...ledger, association: linked });
    if (detached && !exitRow) return finish('TERMINAL_UNOBSERVED', io.simulated, log, { ...ledger, association: linked });
    if (fatalStop && exitRow) {
      const attributed = linked?.chain === 'DESTROY0_THEN_LOCK' && stackOk(linked.destroy?.frames) && stackOk(linked.fatalFrames);
      return finish(attributed ? 'FATAL_ATTRIBUTED' : 'FATAL_UNKNOWN', io.simulated, log, { ...ledger, association: linked, businessExit: exitRow.status });
    }
    if (exitRow) return finish('DIAG_EXITED', io.simulated, log, { ...ledger, businessExit: exitRow.status });
    return finish('TERMINAL_UNOBSERVED', io.simulated, log, ledger);
  };
  let result;
  try {
    result = await attempt();
  } catch (error) {
    result = finish('DRIVER_EXCEPTION', io.simulated, log, { error: String(error), meta });
  } finally {
    try { await cleanup(); } catch (error) { meta.cleanup = 'UNCONFIRMED'; meta.cleanupError = String(error); }
  }
  const quiet = result && !String(result.outcome).startsWith('FATAL_');
  if (quiet && started.device && meta.cleanup !== 'REAPED') result = { ...result, outcome: 'CLEANUP_UNCONFIRMED' };
  if (quiet && meta.hostPid != null && meta.hostReap !== 'REAPED' && result.outcome !== 'SERVER_UNCONFIRMED' && result.outcome !== 'FORWARD_BUSY') {
    result = { ...result, outcome: 'CLEANUP_UNCONFIRMED' };
  }
  return result;
}

function procDump(pid = '200', start = '99', inode = '5555', listen = '5555') {
  return `DEVICE_PID=${pid}
DEVICE_START=${start}
DEVICE_NOW=${start}
CMDLINE=${REMOTE}/lldb-server platform --listen *:5039
---FD---
lrwx------ 1 root root 64 0 socket:[${inode}]
---TCP---
  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 0100007F:C000 00000000:0000 01 00000000:00000000 00:00000000 00000000  2000        0 ${inode} 1 0000000000000000 100 0 0 10 0
   1: 00000000:13AF 00000000:0000 0A 00000000:00000000 00:00000000 00000000  2000        0 ${listen} 1 0000000000000000 100 0 0 10 0
---TCP6---
`;
}

function imageLine(uuid = 'aabbccdd-eeff-0011-2233-445566778899') {
  return `[  2] ${uuid} 0x00007f0000000000 ${RUNTIME_PATH}\n`;
}

function scripted(script) {
  const runs = [];
  const dead = new Set();
  const state = { clock: 0 };
  const identity = procDump();
  const defaults = {
    'host-version': { rc: 0, stdout: 'lldb version 18.0.1', stderr: '' },
    'platform-list': { rc: 0, stdout: script.platform || '', stderr: '' },
    'adb-devices': { rc: 0, stdout: script.serial || '', stderr: '' },
    'push-server': { rc: 0, stdout: '', stderr: '' },
    'server-version': { rc: 0, stdout: 'lldb-server version 18.0.1', stderr: '' },
    'start-server': { rc: 0, stdout: '', stderr: '', pid: 100 },
    'server-identity': { rc: 0, stdout: identity, stderr: '' },
    'forward-list': { rc: 0, stdout: '', stderr: '' },
    forward: { rc: 0, stdout: '', stderr: '' },
    'runtime-note': { rc: 0, stdout: 'Displaying notes found in: .note.android.ident\n  Android 0x00000084 NT_VERSION\n', stderr: '' },
    'runtime-hash': { rc: 0, stdout: `${ORIGINAL_SO}  ${process.env.RUNTIME_SO || '/opt/runtime.so'}\n`, stderr: '' },
    'device-file': { rc: 0, stdout: `${ORIGINAL_SO}  ${RUNTIME_PATH}\n`, stderr: '' },
    'module-maps': { rc: 0, stdout: `7f0000000000-7f0000001000 r-xp 00000000 00:00 1 ${RUNTIME_PATH}\n`, stderr: '' },
    launch: { rc: 0, stdout: imageLine(), stderr: '' },
    logcat: { rc: 0, stdout: '', stderr: '' },
    crash: { rc: 0, stdout: '', stderr: '' },
    'cleanup-probe': { rc: 0, stdout: identity, stderr: '' },
    'cleanup-server': { rc: 0, stdout: 'REAPED\n', stderr: '' },
    'cleanup-forward': { rc: 0, stdout: '', stderr: '' },
  };
  return {
    simulated: true,
    stat(path) { return script.absent ? false : !String(path).endsWith('/missing'); },
    readText(path) { return readFileSync(path, 'utf8'); },
    now(step) {
      if (script.at && script.at[step] != null) state.clock = script.at[step];
      return state.clock;
    },
    async reap(pid) {
      runs.push('reap-host');
      return { ...(script.steps.reap || { rc: 0, stdout: 'REAPED\n', stderr: '' }), pid };
    },
    async run(step, argv) {
      runs.push(step);
      const joined = Array.isArray(argv) ? argv.join(' ') : '';
      const mapsPid = joined.match(/\/proc\/(\d+)\/maps/);
      if (mapsPid && dead.has(mapsPid[1])) return { rc: 1, stdout: '', stderr: 'DEAD' };
      if (script.throw === step) throw new Error(`injected ${step}`);
      if (script.timeout === step) return { rc: null, timedOut: true, stdout: '', stderr: '' };
      const result = { ...(defaults[step] || { rc: 0, stdout: '', stderr: '' }), ...(script.steps[step] || {}) };
      if (Array.isArray(argv) && argv.includes('quit')) {
        for (const matched of String(result.stdout || '').matchAll(/Process (\d+) launched:/g)) dead.add(matched[1]);
      }
      return result;
    },
    runs,
  };
}

function realIO() {
  const children = new Map();
  return {
    simulated: false,
    stat(path) { return existsSync(path); },
    readText(path) { return readFileSync(path, 'utf8'); },
    now() { return Date.now(); },
    reap(pid) {
      const child = children.get(pid);
      if (!child) return Promise.resolve({ rc: 1, stdout: 'UNCONFIRMED', stderr: '' });
      if (child.exitCode != null || child.signalCode != null) return Promise.resolve({ rc: 0, stdout: 'REAPED', stderr: '' });
      return new Promise(resolve => {
        const timer = setTimeout(() => {
          child.kill('SIGKILL');
          resolve({ rc: 4, stdout: 'NOT_REAPED', stderr: '' });
        }, 5000);
        child.once('close', () => {
          clearTimeout(timer);
          resolve({ rc: 0, stdout: 'REAPED', stderr: '' });
        });
        child.kill('SIGTERM');
      });
    },
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
          children.set(child.pid, child);
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
      commandOrder: ['command source', 'command script import', 'platform select remote-android', 'platform connect', 'target create', 'target symbols add', 'script capture.arm()', 'process launch --stop-at-entry', 'script capture.live_module()', 'image list', 'process continue', 'process status', 'quit'],
      remoteExecutable: `${REMOTE}/consumer`,
      remoteLibraryPath: REMOTE,
      hostSymbols: 'HOST_SYMBOLS',
    },
    observation: {
      pairing: 'per tid stack; only rax 0 changes a generation; both ends need a full stack',
      truncated: 'complete=false and chain stays UNKNOWN',
      fatalAddress: 'dwarf variable or same-run fatal message only; historical rdi is not an argument',
      devicePid: 'one /proc/net/tcp LISTEN row whose inode is in that pid fd; host adb pid is reaped separately and never killed as the server',
      forward: 'adb forward --list must not already own tcp:5039; this driver does not replace it',
      imageList: 'image list uuid is not a GNU build id. Path, maps and file sha are taken at the entry stop, before continue or quit',
      shellWait: 'formal android.mjs EXIT_SHELL is not an input; this diagnostic always leaves it UNKNOWN',
      businessExit: 'only the same pid Process <id> exited with status line; lldb rc and a fatal stop are separate',
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
    requiredEnv: ['SCENE_COMMANDS', 'HOST_SYMBOLS', 'RUNTIME_SO', 'ANDROID_SDK_ROOT', 'ANDROID_SERIAL'],
    notUsed: ['SCENE_OBSERVER', 'classifyFullImageExit', 'EXIT_SHELL'],
    prepare: 'node scene.mjs self-test && node scene.mjs calibrate',
    execute: 'SCENE_EXECUTE=1 node scene.mjs execute',
    executeThisLane: 'not invoked',
    sameDrive: true,
    consumer: 'node scene.mjs execute keeps the entry-stop module snapshot from lldb stdout. It does not read /proc after quit, and it does not classify the formal shell',
    budget: { minutes: 45, jobs: 2, core: 0, activeSeconds: 60, totalSeconds: 300 },
    mode: 'preinit/full-image',
    forbid: ['runtime-fini', 'no-image', 'NoFP', 'retry', 'ndk-gdb', 'simulated sceneSuccess', 'host-pid kill', '-S before target', 'synthetic EXIT_SHELL', 'literal build-id token', 'shell classifier', 'lldb rc as business exit', 'lldb uuid as gnu build id', 'relink to add a note'],
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
  const live = argv.indexOf('script capture.live_module()');
  const image = argv.indexOf('image list');
  const continued = argv.indexOf('process continue');
  const status = argv.indexOf('process status');
  const quit = argv.indexOf('quit');
  const order = [source, imported, selected, connected, created, symbols, armed, launched, live, image, continued, status, quit];
  if (order.some(index => index < 0) || order.some((index, place) => place > 0 && index < order[place - 1])) fail('command order');
  if (argv[imported] !== `command script import ${join(HERE, 'capture.py')}`) fail('import is not package-relative');
  if (!argv[launched].includes('--stop-at-entry')) fail('module snapshot is not taken at a stop');
  if (!argv[launched].includes(`LD_LIBRARY_PATH=${REMOTE}`)) fail('remote library path missing');
  if (argv[symbols] === `target symbols add ${REMOTE}/consumer`) fail('symbols alias the remote executable');
  if (launch.stdout == null || launch.stderr == null) fail('launch output was dropped');
  if (launch.timeoutMs !== 60000) fail(`launch budget ${launch.timeoutMs}`);
  if (argv.some(item => item.includes('EXIT_SHELL') || item.includes('build-id:'))) fail('launch command synthesizes records');
  const cleanup = result.log.find(row => row.step === 'cleanup-server');
  const joined = cleanup ? cleanup.argv.join(' ') : '';
  if (!joined.includes('pid=200') || joined.includes('pid=100')) fail('cleanup used the host adb pid');
  const reap = result.log.find(row => row.step === 'reap-host');
  if (!reap || reap.argv[1] !== '100') fail('host adb child was not reaped');
  if (result.meta.hostPid === result.meta.devicePid) fail('device pid collapsed to host pid');
  if (result.log.some(row => row.step === 'module-maps' || row.step === 'device-file')) fail('module was read after the batch returned');
}

function processText(body, pid = '2400', live = {}) {
  const base = live.base || '7f0000000000';
  const sha = live.sha || ORIGINAL_SO;
  const maps = live.maps === false ? '' : `MAPS_BEGIN\n${base}-7f0000001000 r-xp 00000000 00:00 1 ${RUNTIME_PATH}\nMAPS_END\n`;
  const file = live.file === false ? '' : `FILE_SHA ${sha}  ${RUNTIME_PATH}\n`;
  const image = live.image === false ? '' : imageLine(live.uuid);
  const head = live.process === false ? '' : `Process ${pid} launched: '${REMOTE}/consumer' (x86_64)\nProcess ${pid} stopped\n`;
  return `${head}MODULE_LIVE pid=${pid} state=stopped\n${maps}${file}${image}${body}`;
}

async function calibrate() {
  const cases = [];
  const expect = async (name, script, outcome, absent) => {
    const previous = process.env.HOST_SYMBOLS;
    const previousSo = process.env.RUNTIME_SO;
    if (script.symbols === undefined) process.env.HOST_SYMBOLS = '/symbols/libcangjie-runtime.so';
    else if (script.symbols === null) delete process.env.HOST_SYMBOLS;
    else process.env.HOST_SYMBOLS = script.symbols;
    if (script.runtimeSo) process.env.RUNTIME_SO = script.runtimeSo;
    else delete process.env.RUNTIME_SO;
    const io = scripted(script);
    const result = await drive(io, '/sdk', 'emulator-5554');
    if (previous === undefined) delete process.env.HOST_SYMBOLS;
    else process.env.HOST_SYMBOLS = previous;
    if (previousSo === undefined) delete process.env.RUNTIME_SO;
    else process.env.RUNTIME_SO = previousSo;
    if (result.outcome !== outcome || result.sceneSuccess || result.productPass || result.readyForNativeExecution || !result.simulated) {
      fail(`${name} -> ${result.outcome}`);
    }
    if (result.shellWait !== 'UNKNOWN' || result.outcome === 'NOT_REPRODUCED' || result.outcome === 'PASS') fail(`${name} adopted a shell result`);
    if (absent && result.log.some(row => row.step === absent && !row.skipped)) fail(`${name} ran ${absent}`);
    cases.push({ name, outcome: result.outcome, steps: result.log.map(row => row.step), simulated: true, sceneSuccess: false, shellWait: result.shellWait, businessExit: result.businessExit });
    return result;
  };
  await expect('missing-tools', { absent: true, steps: {} }, 'TOOL_ABSENT', 'platform-list');
  await expect('symbols-absent', { symbols: null, platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: {} }, 'SYMBOLS_ABSENT', 'launch');
  await expect('version-absent', { platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: { 'host-version': { rc: 0, stdout: '', stderr: '' } } }, 'VERSION_ABSENT', 'launch');
  await expect('platform-missing', { platform: 'remote-linux remote-gdb-server', serial: 'emulator-5554\tdevice', steps: {} }, 'PLATFORM_ABSENT', 'launch');
  await expect('connect-fail', { platform: 'remote-android', serial: '', steps: { 'adb-devices': { rc: 1, stdout: '', stderr: '' } } }, 'CONNECT_FAIL', 'launch');
  await expect('flag-only', { platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: { 'server-identity': { rc: 0, stdout: 'DEVICE_PID=200\nDEVICE_START=99\nDEVICE_NOW=99\nCMDLINE=/data/local/tmp/readscope-lifetime/lldb-server\nPORT_OWNED=1\n', stderr: '' } } }, 'SERVER_UNCONFIRMED', 'launch');
  await expect('split-socket', { platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: { 'server-identity': { rc: 0, stdout: procDump('200', '99', '5555', '9999'), stderr: '' } } }, 'SERVER_UNCONFIRMED', 'launch');
  if (cases.at(-1).steps.includes('cleanup-server')) fail('unconfirmed server was killed');
  await expect('forward-busy', { platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: { 'forward-list': { rc: 0, stdout: 'emulator-5554 tcp:5039 tcp:5039\n', stderr: '' } } }, 'FORWARD_BUSY', 'forward');
  await expect('budget', { platform: 'remote-android', serial: 'emulator-5554\tdevice', at: { launch: 300001 }, steps: {} }, 'UNCAPTURED', 'launch');
  const timed = await expect('timeout', { platform: 'remote-android', serial: 'emulator-5554\tdevice', timeout: 'launch', steps: {} }, 'UNCAPTURED', null);
  if (timed.log.find(row => row.step === 'launch').timeoutMs !== 60000) fail('active budget is not 60s');
  const shaped = await expect('launch-shaped', { platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: {} }, 'FILE_IDENTITY_ABSENT', null);
  assertOrder(shaped);
  const matched = await expect('identity-without-process', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: processText('', '2400', { process: false }), stderr: '' } } }, 'PROCESS_ABSENT', null);
  if (matched.businessExit !== null) fail('missing process acquired a business exit');
  const pasted = imageLine() + 'EXIT_SHELL child_pid=2400 realtime_start=1763240464.000999999 realtime_end=1763240465.000999999 native_wait_rc=0\n' +
    ['elf-native-load', 'elf-constructor-exactly-once', 'elf-public-load', 'elf-public-native-identity', 'elf-unknown-close-rejected', 'elf-logical-close', 'elf-external-native-close', 'elf-no-close-fini', 'elf-reopen-same-identity', 'elf-reopen-no-double-init', 'elf-reopen-close', 'elf-runtime-init', 'elf-published-package'].map(name => `TARGET ${name} executed=1 pass=1`).join('\n') +
    '\nELF_FIXTURE_INIT count=1\nELF_FIXTURE_FINI count=1\n';
  const adopted = await expect('pasted-shell', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: pasted, stderr: '' } } }, 'MODULE_PATH_ABSENT', null);
  if (adopted.businessExit !== null || adopted.shellWait !== 'UNKNOWN') fail('pasted shell became a diagnostic exit');
  const ignored = await expect('shell-ignored', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 7, stdout: processText(`Process 2400 exited with status = 0 (0x00000000) \n${pasted.replace(imageLine(), '')}`), stderr: '' } } }, 'DIAG_EXITED', null);
  if (ignored.businessExit !== 0 || ignored.lldbRc !== 7) fail('lldb rc was used as the business exit');
  await expect('duplicate-pid', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: processText('', '2400') + processText('', '2401'), stderr: '' } } }, 'DUPLICATE_PID', null);
  await expect('detached', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: processText('Process 2400 detached\n'), stderr: '' } } }, 'TERMINAL_UNOBSERVED', null);
  await expect('literal-build-id', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: `${RUNTIME_PATH}\nbuild-id: aabbccddeeff00112233445566778899\n`, stderr: '' } } }, 'MODULE_PATH_ABSENT', null);
  await expect('soname-only', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: '[  2] aabbccdd-eeff-0011-2233-445566778899 0x00007f0000000000 libcangjie-runtime.so\n', stderr: '' } } }, 'MODULE_PATH_ABSENT', null);
  const uuidOnly = await expect('uuid-not-build-id', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: processText('', '2400', { process: false, uuid: '11223344-5566-7788-99aa-bbccddeeff00' }), stderr: '' } } }, 'PROCESS_ABSENT', null);
  if (uuidOnly.buildId !== null || uuidOnly.meta.buildIdStatus !== 'ABSENT' || uuidOnly.meta.lldbUuid !== '11223344-5566-7788-99aa-bbccddeeff00') fail('lldb uuid was stored as a build id');
  await expect('note-present', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { 'runtime-note': { rc: 0, stdout: '    Build ID: aabbccddeeff00112233445566778899\n', stderr: '' } } }, 'NOTE_PRESENT', null);
  await expect('file-mismatch', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: processText('', '2400', { sha: 'b'.repeat(64) }), stderr: '' } } }, 'FILE_IDENTITY_MISMATCH', null);
  await expect('maps-absent', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: processText('', '2400', { maps: false }), stderr: '' } } }, 'MAPS_ABSENT', null);
  const lateMaps = `Process 2400 launched: '${REMOTE}/consumer' (x86_64)\nProcess 2400 stopped\nMODULE_LIVE pid=2400 state=stopped\nFILE_SHA ${ORIGINAL_SO}  ${RUNTIME_PATH}\n${imageLine()}Process 2400 exited with status = 0 (0x00000000) \nMAPS_BEGIN\n7f0000000000-7f0000001000 r-xp 00000000 00:00 1 ${RUNTIME_PATH}\nMAPS_END\n`;
  await expect('maps-after-exit', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: lateMaps, stderr: '' } } }, 'MAPS_ABSENT', null);
  await expect('base-mismatch', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: processText('', '2400', { base: '7f0000001000' }), stderr: '' } } }, 'BASE_MISMATCH', null);
  await expect('rc-without-process', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 1, stdout: processText('', '2400', { process: false }), stderr: 'launch error' } } }, 'PROCESS_ABSENT', null);
  await expect('log-missing', { platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: { launch: { rc: 0, stdout: null, stderr: null } } }, 'LOG_MISSING', null);
  await expect('cleanup-failed', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { 'cleanup-server': { rc: 4, stdout: 'NOT_REAPED\n', stderr: '' } } }, 'CLEANUP_UNCONFIRMED', null);
  const stop = 'Process 2400 stopped\n* thread #1, name = \'consumer\', stop reason = signal SIGABRT\nAbort message: FORTIFY: pthread_mutex_lock called on a destroyed mutex (0x1234)\n';
  const unexited = await expect('fatal-stop-unexited', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: processText(stop), stderr: '' } } }, 'FATAL_STOP_UNEXITED', null);
  if (unexited.businessExit !== null) fail('unobserved final state became a business exit');
  const frames = [{ pc: 1, module: 'libc.so', name: 'pthread_mutex_destroy' }, { pc: 2, module: 'libconsumer.so', name: 'owner' }];
  const fatalFrames = [{ pc: 3, module: 'libc.so', name: 'pthread_mutex_lock' }, { pc: 4, module: 'libconsumer.so', name: 'user' }];
  const state = { complete: true, dead: { '4660': { state: 'dead', event: { frames, tid: 1624, kind: 'destroy', return: 0 } } } };
  const chain = `${stop}EVENT_JSON:${JSON.stringify(state)}\nFATAL_FRAMES_JSON:${JSON.stringify(fatalFrames)}\n`;
  const held = await expect('chain-without-exit', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: processText(chain), stderr: '' } } }, 'FATAL_STOP_UNEXITED', null);
  if (held.association?.chain !== 'DESTROY0_THEN_LOCK' || held.businessExit !== null || held.sceneSuccess) fail('chain without an exit was promoted');
  await expect('fatal-unknown', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: processText(`${stop}Process 2400 exited with status = 134 (0x00000086) \n`), stderr: '' } } }, 'FATAL_UNKNOWN', null);
  const attributed = await expect('fatal-attributed', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: processText(`${chain}Process 2400 exited with status = 134 (0x00000086) \n`), stderr: '' } } }, 'FATAL_ATTRIBUTED', null);
  if (attributed.sceneSuccess || attributed.businessExit !== 134 || attributed.shellWait !== 'UNKNOWN') fail('attributed exit collapsed into a shell pass');
  const thrown = await expect('launch-threw', { platform: 'remote-android', serial: 'emulator-5554\tdevice', throw: 'launch', steps: {} }, 'DRIVER_EXCEPTION', null);
  if (!thrown.log.some(row => row.step === 'cleanup-server' && row.argv.join(' ').includes('pid=200'))) fail('exception path skipped device cleanup');
  const recorder = scripted({ platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: { launch: { rc: 0, stdout: processText('Process 2400 exited with status = 0 (0x00000000) \n'), stderr: '' }, 'module-maps': { rc: 0, stdout: `7f0000000000-7f0000001000 r-xp 00000000 00:00 1 ${RUNTIME_PATH}\n`, stderr: '' } } });
  await recorder.run('launch', launchArgv('/sdk/lldb'));
  const late = await recorder.run('module-maps', ['adb', '-s', 'emulator-5554', 'shell', 'cat /proc/2400/maps']);
  if (late.rc === 0 || String(late.stdout).includes(RUNTIME_PATH)) fail('dead pid still returned maps');
  cases.push({ name: 'dead-pid-maps', outcome: 'DEAD', steps: ['launch', 'module-maps'], simulated: true, sceneSuccess: false, shellWait: 'UNKNOWN', businessExit: null });
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
      const cases = await calibrate();
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
  const result = await drive(realIO(), sdk, process.env.ANDROID_SERIAL || 'emulator-5554');
  writeFileSync(join(HERE, 'execute-result.json'), JSON.stringify(result, null, 2) + '\n');
  console.log(result.outcome);
  process.exit(String(result.outcome).startsWith('FATAL_') ? 0 : 1);
} else {
  fail('usage: scene.mjs self-test|calibrate|execute');
}
