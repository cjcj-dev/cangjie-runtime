#!/usr/bin/env zx
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { runInNewContext } from 'node:vm';

const HERE = dirname(new URL(import.meta.url).pathname);
const OLD_ADDRESS = '0x7b33ef1eb08c';
const RUNTIME = 'd51c34a06cfd980d0e2694514e2a41421ac41b83';
const ORIGINAL_SO = 'aac5c4456fc9746243a2a15d22beac6a52ee27f269a94cd0e839315bc080db51';
const OBSERVER = '761b0ad956d975488b984e419ef4a803711e2635';
const NDK = '27.3.13750724';
const REMOTE = '/data/local/tmp/readscope-lifetime';
const ACTIVE_MS = 60000;
const TOTAL_MS = 300000;
const CLEANUP_RESERVE_MS = 20000;

function sliceMs(elapsed, requested, cleanup, total = TOTAL_MS) {
  const remain = cleanup ? total - elapsed : total - CLEANUP_RESERVE_MS - elapsed;
  if (remain <= 0) return 0;
  return Math.min(requested, remain);
}

function budgetBound(origin) {
  const raw = process.env.SCENE_DEADLINE_MS;
  if (raw == null || raw === '') return { total: TOTAL_MS, source: 'INTERNAL' };
  const absolute = Number(raw);
  if (!Number.isFinite(absolute)) return { total: null, source: 'INVALID' };
  return { total: Math.min(TOTAL_MS, absolute - origin), source: 'SCENE_DEADLINE_MS' };
}
const RUNTIME_PATH = `${REMOTE}/libcangjie-runtime.so`;
const PYTHON_SONAME = 'libpython3.11.so.1.0';
const LAUNCHER_SHA = '8d0172dae364b43da5e1e40a6707bfa07679dccb31ae9b6b321f51f114a4f3cd';
const OFFICIAL_LAUNCHER = '#!/bin/bash\nCURDIR=$(cd "$(dirname "$0")" && pwd)\nexport PYTHONHOME="$CURDIR/../python3"\nexport LD_LIBRARY_PATH="$CURDIR/../python3/lib:$LD_LIBRARY_PATH"\n"$CURDIR/lldb" "$@"\n';

const TOOLS = {
  hostLldb: `ndk/${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/lldb`,
  hostWrapper: `ndk/${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/lldb.sh`,
  pythonLib: `ndk/${NDK}/toolchains/llvm/prebuilt/linux-x86_64/python3/lib/libpython3.11.so.1.0`,
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

function agreedTarget(lines) {
  const parsed = lines.map(line => {
    const matched = line.match(/^TARGET_IDENTITY pid=(\d+) start=(\d+) path=(\S+)$/);
    return matched ? { pid: matched[1], start: matched[2], path: matched[3] } : null;
  }).filter(Boolean);
  if (parsed.length === 0) return null;
  const first = parsed[0];
  if (parsed.some(row => row.pid !== first.pid || row.start !== first.start || row.path !== first.path)) return { conflict: true };
  return first;
}

function targetProbe(target) {
  return `pid=${target.pid}
want_start=${target.start}
want_path=${target.path}
if [ ! -d /proc/$pid ]; then echo TARGET_PID=$pid; echo TARGET_START=$want_start; echo TARGET_PATH=$want_path; echo TARGET_STATE=GONE; exit 0; fi
start=$(awk '{print $22}' /proc/$pid/stat)
exe=$(readlink /proc/$pid/exe)
echo TARGET_PID=$pid
echo TARGET_START=$start
echo TARGET_PATH=$exe
if [ "$start" = "$want_start" ] && [ "$exe" = "$want_path" ]; then echo TARGET_STATE=PRESENT; else echo TARGET_STATE=MISMATCH; fi`;
}

function targetKill(target) {
  return `pid=${target.pid}
want_start=${target.start}
want_path=${target.path}
if [ ! -d /proc/$pid ]; then echo TARGET_STATE=GONE; exit 0; fi
start=$(awk '{print $22}' /proc/$pid/stat)
exe=$(readlink /proc/$pid/exe)
if [ "$start" != "$want_start" ] || [ "$exe" != "$want_path" ]; then echo TARGET_STATE=MISMATCH; exit 3; fi
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

function reapConfirmed(rc, text) {
  return rc === 0 && String(text || '').split('\n').some(line => line.trim() === 'REAPED');
}

function targetVerdict(text, target) {
  const grab = name => {
    const matched = String(text || '').match(new RegExp(`^${name}=(.*)$`, 'm'));
    return matched ? matched[1].trim() : '';
  };
  if (grab('TARGET_PID') !== target.pid || grab('TARGET_START') !== target.start || grab('TARGET_PATH') !== target.path) return 'UNKNOWN';
  const state = grab('TARGET_STATE');
  return state === 'PRESENT' || state === 'GONE' ? state : 'UNKNOWN';
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
  const targets = [];
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
    else if (line.startsWith('TARGET_IDENTITY ')) targets.push(line);
    else if (/^\[\s*\d+\]/.test(line)) image.push(line);
  }
  return { pid, image: image.join('\n'), maps: maps.join('\n'), file: files.join('\n'), target: agreedTarget(targets) };
}

function sha256(text) {
  return createHash('sha256').update(text).digest('hex');
}

function missingSonames(text) {
  const names = [];
  for (const line of String(text || '').split('\n')) {
    const match = line.match(/^\s*(\S+)\s+=>\s+not found\s*$/);
    if (match) names.push(match[1]);
  }
  return names;
}

function closureDecision(lddText, pythonPackaged) {
  const missing = missingSonames(lddText);
  const closedByLauncher = pythonPackaged ? missing.filter(name => name === PYTHON_SONAME) : [];
  const open = missing.filter(name => !closedByLauncher.includes(name));
  return { lddRcIsNotClosure: true, missing, closedByLauncher, open, ok: open.length === 0 };
}

function hostArgv(sdk, args) {
  return [join(sdk, TOOLS.hostWrapper), ...args];
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
    '-o', `process launch --stop-at-entry --environment LD_LIBRARY_PATH=${REMOTE} -- ${REMOTE}/fixture.so preinit`,
    '-o', 'script capture.live_module()',
    '-o', 'image list',
    '-o', 'process continue',
    '-o', 'process status',
    '-o', 'script capture.seal()',
    '-k', 'process status',
    '-k', 'script capture.seal()',
    '-k', 'script capture.mark_detach()',
    '-k', 'process detach --keep-stopped true',
    '-k', 'quit',
  ];
}

function launchContract(command) {
  const matched = String(command).match(/--stop-at-entry(?:\s+(\S+))?/);
  const next = matched ? matched[1] : null;
  const extra = Boolean(next && !next.startsWith('--'));
  const inferior = String(command).split(' -- ').pop().trim().split(/\s+/);
  return {
    extra,
    argc: inferior.length + 1,
    ok: Boolean(matched) && !extra && inferior.length === 2 && inferior[0].endsWith('/fixture.so') && inferior[1] === 'preinit',
  };
}

function capturedState(io, stdout) {
  if (typeof io.savedCapture === 'function') {
    const saved = io.savedCapture();
    if (saved && typeof saved === 'object') return saved;
  }
  return jsonLine(stdout, 'EVENT_JSON:');
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

function detachFact(proc) {
  return {
    intent: Boolean(proc.intent),
    completed: proc.detachState === 'detached',
    status: proc.detachState === 'detached' ? 'DETACHED' : 'UNKNOWN',
    error: proc.detachError ?? null,
  };
}

function sbProcess(text) {
  const launched = [];
  const exits = [];
  const states = [];
  const stops = [];
  const restarts = [];
  let current = null;
  let controlled = false;
  let intent = false;
  let detachState = 'UNKNOWN';
  let detachError = null;
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
      exits.push({ pid: matched[1], status: Number(matched[2]), description: matched[3].trim(), controlled });
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
      if (matched[2] === 'detached') {
        controlled = true;
        detachState = 'detached';
      }
      states.push({ pid: matched[1], state: matched[2] });
      continue;
    }
    matched = line.match(/^Process (\d+) is running\.$/);
    if (matched) {
      current = matched[1];
      states.push({ pid: matched[1], state: 'running' });
      continue;
    }
    if (line.trim() === 'DETACH_INTENT' || line.trim() === 'CONTROLLED_DETACH') {
      intent = true;
      continue;
    }
    if (line.startsWith('Detach failed:') || line.includes('invalid boolean option:')) {
      detachError = line.trim();
      continue;
    }
    matched = line.match(/^\* thread #\d+.*\bstop reason = (.+)$/);
    if (matched) stops.push({ pid: current, reason: matched[1].trim() });
  }
  return { launched, exits, states, stops, restarts, controlled, intent, detachState, detachError };
}

function hasRemoteAndroid(text) {
  return /(?:^|\s)remote-android(?=[:\s]|$)/.test(text || '');
}

async function probeHost(io, sdk, run) {
  const bare = join(sdk, TOOLS.hostLldb);
  const launcher = join(sdk, TOOLS.hostWrapper);
  const pythonLib = join(sdk, TOOLS.pythonLib);
  for (const name of ['hostLldb', 'hostWrapper', 'pythonLib']) {
    if (!io.stat(join(sdk, TOOLS[name]))) return { outcome: 'TOOL_ABSENT', extra: { missing: name } };
  }
  const text = io.readText(launcher);
  if (sha256(text) !== LAUNCHER_SHA) return { outcome: 'LAUNCHER_MISMATCH', extra: { meta: { launcher } } };
  const ldd = await run('host-ldd', ['ldd', bare]);
  if (ldd.timedOut) return { outcome: 'UNCAPTURED' };
  const closure = closureDecision(ldd.stdout, true);
  if ((ldd.rc !== 0 && closure.missing.length === 0) || !closure.ok) return { outcome: 'HOST_CLOSURE_ABSENT', closure };
  const hostVer = await run('host-version', hostArgv(sdk, ['--version']));
  if (hostVer.timedOut) return { outcome: 'UNCAPTURED', closure };
  if (hostVer.rc !== 0 || !hostVer.stdout) return { outcome: 'VERSION_ABSENT', closure };
  const platforms = await run('platform-list', hostArgv(sdk, ['-b', '-o', 'platform list', '-o', 'quit']));
  if (platforms.timedOut) return { outcome: 'UNCAPTURED', closure };
  if (platforms.rc !== 0) return { outcome: 'PLATFORM_LIST_FAIL', closure };
  if (!hasRemoteAndroid(platforms.stdout)) return { outcome: 'PLATFORM_ABSENT', closure, extra: { meta: { remoteAndroid: false } } };
  return { outcome: null, closure, launcher };
}

async function drive(io, sdk, serial = 'emulator-5554', options = {}) {
  const log = [];
  const meta = { cleanup: 'NOT_STARTED', hostReap: 'NOT_STARTED', hostPid: null, devicePid: null, target: null, targetCleanup: 'UNKNOWN', detach: { intent: false, completed: false, status: 'UNKNOWN', error: null }, remoteAndroid: false, nativeWait: 'ABSENT' };
  const started = { device: null, forward: false };
  const tool = name => join(sdk, TOOLS[name]);
  const origin = typeof io.now === 'function' ? io.now('start') : Date.now();
  const bound = budgetBound(origin);
  meta.deadline = { source: bound.source, totalMs: bound.total };
  if (options.requireDeadline && bound.source !== 'SCENE_DEADLINE_MS') {
    return finish('DEADLINE_ABSENT', io.simulated, log, { meta });
  }
  if (bound.total == null || bound.total <= 0) {
    return finish(bound.source === 'INVALID' ? 'DEADLINE_ABSENT' : 'DEADLINE_EXHAUSTED', io.simulated, log, { meta });
  }
  const elapsed = step => (typeof io.now === 'function' ? io.now(step) : Date.now()) - origin;
  const run = async (step, argv, options = {}) => {
    const cap = options.active ? ACTIVE_MS : (options.timeoutMs ?? 30000);
    const slice = sliceMs(elapsed(step), cap, Boolean(options.cleanup), bound.total);
    if (slice <= 0) {
      const skipped = { rc: null, timedOut: true, stdout: '', stderr: '', skipped: true };
      log.push({ step, argv, rc: null, timedOut: true, skipped: true, timeoutMs: 0, stdout: '', stderr: '' });
      return skipped;
    }
    const result = await io.run(step, argv, { ...options, timeoutMs: slice });
    log.push({
      step, argv, rc: result.rc, timedOut: Boolean(result.timedOut), timeoutMs: slice,
      stdout: typeof result.stdout === 'string' ? result.stdout : null,
      stderr: typeof result.stderr === 'string' ? result.stderr : null,
      owned: result.owned ?? null,
      ownership: result.ownership ?? null,
    });
    return result;
  };
  const cleanup = async () => {
    if (meta.hostPid != null && typeof io.reap === 'function') {
      const reaped = await io.reap(meta.hostPid, sliceMs(elapsed('reap-host'), 5000, true, bound.total));
      log.push({ step: 'reap-host', argv: ['reap', String(meta.hostPid)], rc: reaped.rc, timedOut: false, stdout: reaped.stdout ?? '', stderr: reaped.stderr ?? '', closed: reaped.closed === true, childRc: reaped.childRc ?? null, childSignal: reaped.childSignal ?? null });
      meta.hostReap = reaped.closed === true && reapConfirmed(reaped.rc, reaped.stdout) ? 'REAPED' : 'UNCONFIRMED';
    }
    if (started.device) {
      const probe = await run('cleanup-probe', [tool('adb'), '-s', serial, 'shell', identityShell()], { timeoutMs: 5000, cleanup: true });
      const still = parseIdentity(probe.stdout, meta.hostPid);
      if (!still || still.pid !== started.device.pid || still.start !== started.device.start) meta.cleanup = 'UNCONFIRMED';
      else {
        const cleaned = await run('cleanup-server', [tool('adb'), '-s', serial, 'shell', killShell(still.pid)], { timeoutMs: 5000, cleanup: true });
        meta.cleanup = reapConfirmed(cleaned.rc, cleaned.stdout) && !String(cleaned.argv || '').includes(String(meta.hostPid)) ? 'REAPED' : 'UNCONFIRMED';
      }
    }
    if (started.forward) {
      const removed = await run('cleanup-forward', [tool('adb'), '-s', serial, 'forward', '--remove', 'tcp:5039'], { timeoutMs: 5000, cleanup: true });
      if (removed.rc !== 0) meta.cleanup = 'UNCONFIRMED';
    }
    if (!meta.target) meta.targetCleanup = 'UNKNOWN';
    else {
      const probe = await run('cleanup-target-probe', [tool('adb'), '-s', serial, 'shell', targetProbe(meta.target)], { timeoutMs: 5000, cleanup: true });
      if (probe.skipped || probe.timedOut) meta.targetCleanup = 'UNCONFIRMED';
      else {
        const verdict = targetVerdict(probe.stdout, meta.target);
        if (verdict === 'PRESENT') {
          const cleaned = await run('cleanup-target', [tool('adb'), '-s', serial, 'shell', targetKill(meta.target)], { timeoutMs: 5000, cleanup: true });
          if (cleaned.skipped || cleaned.timedOut) meta.targetCleanup = 'UNCONFIRMED';
          else if (cleaned.rc === 0 && String(cleaned.stdout || '').split('\n').some(line => line.trim() === 'REAPED')) meta.targetCleanup = 'REAPED';
          else if (cleaned.rc === 0 && String(cleaned.stdout || '').split('\n').some(line => line.trim() === 'TARGET_STATE=GONE')) meta.targetCleanup = 'ABSENT';
          else meta.targetCleanup = 'UNCONFIRMED';
        } else if (verdict === 'GONE') meta.targetCleanup = 'ABSENT';
        else meta.targetCleanup = 'UNKNOWN';
      }
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
    const probed = await probeHost(io, sdk, run);
    if (probed.closure) meta.closure = probed.closure;
    if (probed.outcome) return finish(probed.outcome, io.simulated, log, { meta });
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
    const launch = await run('launch', launchArgv(join(sdk, TOOLS.hostWrapper)), { active: true });
    meta.foreground = {
      pid: launch.pid ?? null,
      signal: launch.signal ?? null,
      timer: Boolean(launch.timer),
      closed: launch.closed === true,
      owned: launch.owned ?? null,
      rc: launch.rc ?? null,
      timedOut: Boolean(launch.timedOut),
    };
    if (launch.timedOut) {
      const saved = capturedState(io, launch.stdout);
      return finish('UNCAPTURED', io.simulated, log, { meta, businessExit: null, saved });
    }
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
    meta.detach = detachFact(proc);
    if (proc.launched.length === 1 && live.target && !live.target.conflict && live.target.pid === proc.launched[0].pid && live.target.path === `${REMOTE}/consumer`) {
      meta.target = live.target;
    }
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
    const exitRow = proc.exits.find(row => row.pid === pid && !row.controlled) || null;
    const stops = proc.stops.filter(row => row.pid === pid && row.reason);
    const fatalStop = stops.some(row => /\bsignal SIGABRT\b|\bsignal SIGSEGV\b/.test(row.reason));
    const detached = proc.controlled || proc.states.some(row => row.pid === pid && row.state === 'detached');
    let linked = null;
    if (fatalStop || fatal) {
      const state = capturedState(io, launch.stdout);
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
    'host-ldd': { rc: 0, stdout: `\t${PYTHON_SONAME} => not found\n`, stderr: '' },
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
    readText(path) { return String(path).endsWith('/lldb.sh') ? OFFICIAL_LAUNCHER : readFileSync(path, 'utf8'); },
    now(step) {
      if (script.at && script.at[step] != null) state.clock = script.at[step];
      return state.clock;
    },
    async reap(pid) {
      runs.push('reap-host');
      const result = { ...(script.steps.reap || { rc: 0, stdout: 'REAPED\n', stderr: '' }), pid };
      if (result.closed == null) result.closed = reapConfirmed(result.rc, result.stdout);
      return result;
    },
    async run(step, argv) {
      runs.push(step);
      const joined = Array.isArray(argv) ? argv.join(' ') : '';
      const mapsPid = joined.match(/\/proc\/(\d+)\/maps/);
      if (mapsPid && dead.has(mapsPid[1])) return { rc: 1, stdout: '', stderr: 'DEAD' };
      if (script.throw === step) throw new Error(`injected ${step}`);
      if (script.timeout === step) {
        return { rc: null, timedOut: true, timer: true, signal: 'SIGTERM', closed: true, pid: 4242, stdout: script.timeoutStdout || '', stderr: '' };
      }
      const result = { ...(defaults[step] || { rc: 0, stdout: '', stderr: '' }), ...(script.steps[step] || {}) };
      if (Array.isArray(argv) && argv.includes('quit')) {
        for (const matched of String(result.stdout || '').matchAll(/Process (\d+) launched:/g)) dead.add(matched[1]);
      }
      return result;
    },
    savedCapture() { return script.saved || null; },
    runs,
  };
}

function readProc(pid) {
  try {
    const text = readFileSync(`/proc/${pid}/stat`, 'utf8');
    const end = text.lastIndexOf(')');
    if (end < 0) return null;
    const fields = text.slice(end + 2).trim().split(/\s+/);
    return { pgrp: fields[2], start: fields[19] };
  } catch {
    return null;
  }
}

function groupIdentity(pid) {
  const now = readProc(pid);
  if (!now || now.pgrp !== String(pid)) return null;
  return { pid: Number(pid), start: now.start, pgrp: now.pgrp };
}

function groupMembers(pgid, read = readProc) {
  let names = [];
  try { names = readdirSync('/proc'); } catch { return []; }
  const members = [];
  for (const name of names) {
    if (!/^\d+$/.test(name)) continue;
    const info = read(name);
    if (!info || info.pgrp !== String(pgid)) continue;
    members.push({ pid: Number(name), start: info.start, pgrp: info.pgrp });
  }
  return members;
}

function signalOwned(child, identity, signal, read = readProc) {
  if (!identity) {
    try { child.kill(signal); } catch { /* fixture or already closed */ }
    return 'DIRECT';
  }
  const now = read(identity.pid);
  if (now && (now.start !== identity.start || now.pgrp !== identity.pgrp)) return 'REUSED';
  if (now) {
    identity.members = groupMembers(identity.pid, read);
    try { process.kill(-identity.pid, signal); } catch { /* group already gone */ }
    return 'GROUP';
  }
  const members = Array.isArray(identity.members) ? identity.members : [];
  let sent = 0;
  for (const member of members) {
    if (member.pid === identity.pid) continue;
    const memberNow = read(member.pid);
    if (!memberNow || memberNow.start !== member.start || memberNow.pgrp !== identity.pgrp) continue;
    try { process.kill(member.pid, signal); sent += 1; } catch { /* member already gone */ }
  }
  return sent > 0 ? 'MEMBER' : 'UNCONFIRMED';
}

function watchChild(child, runMs, reapMs) {
  return new Promise(resolve => {
    let stdout = '';
    let stderr = '';
    let settled = false;
    let timingOut = false;
    let killTimer = null;
    let giveUp = null;
    let ownership = 'DIRECT';
    let escalation = null;
    const pid = child.pid;
    let identity = null;
    if (child.ownedGroup) {
      for (let attempt = 0; attempt < 5 && !identity; attempt += 1) identity = groupIdentity(pid);
    }
    ownership = identity ? 'GROUP' : (child.ownedGroup ? 'NO_IDENTITY' : 'DIRECT');
    const finishRun = result => { if (!settled) { settled = true; resolve({ ...result, escalation }); } };
    if (child.stdout) child.stdout.on('data', chunk => { stdout += chunk; });
    if (child.stderr) child.stderr.on('data', chunk => { stderr += chunk; });
    const arm = signal => {
      const how = signalOwned(child, identity, signal);
      if (how === 'REUSED') ownership = 'REUSED';
      if (how === 'MEMBER' || how === 'UNCONFIRMED') escalation = how;
      return how;
    };
    const timer = setTimeout(() => {
      timingOut = true;
      arm('SIGTERM');
      if (settled) return;
      killTimer = setTimeout(() => { arm('SIGKILL'); }, Math.max(1, Math.floor(reapMs / 2)));
      giveUp = setTimeout(() => {
        finishRun({ rc: null, signal: null, timer: true, timedOut: true, closed: false, owned: ownership === 'REUSED' ? 'REUSED' : 'UNCONFIRMED', ownership, pid, stdout, stderr });
      }, reapMs);
    }, runMs);
    child.on('error', error => {
      clearTimeout(timer);
      clearTimeout(killTimer);
      clearTimeout(giveUp);
      finishRun({ rc: 127, signal: null, timer: timingOut, timedOut: timingOut, closed: false, owned: 'UNCONFIRMED', ownership, pid, stdout, stderr: `${stderr}${error.message}` });
    });
    child.on('close', (code, signal) => {
      clearTimeout(timer);
      clearTimeout(killTimer);
      clearTimeout(giveUp);
      finishRun({ rc: code, signal: signal ?? null, timer: timingOut, timedOut: timingOut, closed: true, owned: 'CLOSED', ownership, pid, stdout, stderr });
    });
  });
}

function realIO() {
  const children = new Map();
  return {
    simulated: false,
    stat(path) { return existsSync(path); },
    readText(path) { return readFileSync(path, 'utf8'); },
    savedCapture() {
      const path = join(HERE, 'run', 'mutex-events.json');
      if (!existsSync(path)) return null;
      try { return JSON.parse(readFileSync(path, 'utf8')); } catch { return null; }
    },
    now() { return Date.now(); },
    reap(pid, waitMs = 0) {
      const record = children.get(Number(pid));
      if (!record) return Promise.resolve({ rc: 1, stdout: 'UNCONFIRMED', stderr: '', closed: false, signal: null, timer: false, pid, childRc: null, childSignal: null });
      const snapshot = () => {
        if (!record.close) {
          return { rc: 1, stdout: 'UNCONFIRMED\n', stderr: record.error || '', closed: false, signal: null, timer: false, pid, childRc: record.exit ? record.exit.code : null, childSignal: record.exit ? record.exit.signal : null };
        }
        return { rc: 0, stdout: 'REAPED\n', stderr: '', closed: true, signal: record.close.signal ?? null, timer: false, pid, childRc: record.close.code, childSignal: record.close.signal ?? null };
      };
      if (record.close) return Promise.resolve(snapshot());
      const exited = Boolean(record.exit) || record.child.exitCode != null || record.child.signalCode != null;
      const budget = Math.max(0, waitMs);
      if (exited) {
        if (budget === 0) return Promise.resolve(snapshot());
        return new Promise(resolve => {
          const timer = setTimeout(() => resolve(snapshot()), budget);
          record.child.once('close', () => { clearTimeout(timer); resolve(snapshot()); });
        });
      }
      if (budget === 0) return Promise.resolve(snapshot());
      return new Promise(resolve => {
        const timer = setTimeout(() => resolve({ rc: 4, stdout: 'NOT_REAPED\n', stderr: '', closed: false, signal: null, timer: true, pid, childRc: record.exit ? record.exit.code : null, childSignal: record.exit ? record.exit.signal : null }), budget);
        record.child.once('close', () => { clearTimeout(timer); resolve(snapshot()); });
        try { record.child.kill('SIGTERM'); } catch { /* already closed */ }
      });
    },
    run(step, argv, options = {}) {
      const child = spawn(argv[0], argv.slice(1), {
        stdio: ['ignore', 'pipe', 'pipe'],
        detached: !options.background,
        env: { ...process.env, EXIT_SCENE_OUT: join(HERE, 'run') },
      });
      if (!options.background) child.ownedGroup = true;
      if (options.background) {
        const record = { child, exit: null, error: null, close: null };
        child.on('exit', (code, signal) => { record.exit = { code, signal: signal ?? null }; });
        child.on('error', error => { record.error = String(error && error.message || error); });
        child.on('close', (code, signal) => { record.close = { code, signal: signal ?? null }; });
        children.set(child.pid, record);
        return Promise.resolve({ rc: 0, timedOut: false, closed: false, stdout: '', stderr: '', pid: child.pid, signal: null, timer: false });
      }
      const timeoutMs = options.timeoutMs ?? options.totalMs ?? TOTAL_MS;
      const reapMs = Math.min(2000, Math.floor(timeoutMs / 4));
      return watchChild(child, Math.max(0, timeoutMs - reapMs), reapMs);
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
      commandOrder: ['command source', 'command script import', 'platform select remote-android', 'platform connect', 'target create', 'target symbols add', 'script capture.arm()', 'process launch --stop-at-entry', 'script capture.live_module()', 'image list', 'process continue', 'process status', 'script capture.seal()', '-k process status', '-k script capture.seal()', '-k script capture.mark_detach()', '-k process detach --keep-stopped true', '-k quit'],
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
      imageList: 'image list uuid is not a GNU build id. Path, maps and file sha are taken at the entry stop, before continue. A crash does not depend on later ordinary -o commands',
      shellWait: 'formal android.mjs EXIT_SHELL is not an input; this diagnostic always leaves it UNKNOWN',
      businessExit: 'only a same-pid exited-with-status line that is not after CONTROLLED_DETACH. lldb rc, quit, detach, target reap and a timeout signal are not a business exit',
      deadline: 'SCENE_DEADLINE_MS is an absolute millisecond bound. The driver uses min(own 300s, deadline-origin) for work and cleanup. It does not add time and does not replace the server pid with the target pid',
      target: 'pid, starttime and /proc/pid/exe saved while the entry stop is examinable. Cleanup kills only that triple. A reused pid or a missing line stays UNKNOWN and is not killed',
      crashCommands: '-k status, seal, detach intent, process detach --keep-stopped true, then quit. Completion is only the formal Process <id> detached line. Intent is not completion',
      foreground: 'the launch pid is waited to close inside its own slice. TERM then SIGKILL. Cleanup uses the reserved part of the 300s and does not add another 5s',
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
    requiredEnv: ['SCENE_COMMANDS', 'HOST_SYMBOLS', 'RUNTIME_SO', 'ANDROID_SDK_ROOT', 'ANDROID_SERIAL', 'SCENE_DEADLINE_MS'],
    notUsed: ['SCENE_OBSERVER', 'classifyFullImageExit', 'EXIT_SHELL'],
    prepare: 'node scene.mjs self-test && node scene.mjs calibrate',
    execute: 'SCENE_EXECUTE=1 node scene.mjs execute',
    executeThisLane: 'not invoked',
    sameDrive: true,
    hostCheck: 'node scene.mjs host-check',
    hostLauncher: TOOLS.hostWrapper,
    hostClosure: 'ldd rc 0 is not a complete dependency result. libpython3.11.so.1.0 is closed only by lldb.sh prepending the packaged python3/lib. Foreground lldb.sh is one process group. Timeout signals that group while the leader starttime matches, then signals only recorded members that are still in the group. Background adb is not in that group.',
    consumer: 'node scene.mjs host-check before any AVD. node scene.mjs execute uses the same lldb.sh argv for version, platform list and launch, stops at SCENE_DEADLINE_MS, and never past its own 300s. Detach completion is only Process <id> detached. Host reap requires an observed close',
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
  const sealed = argv.indexOf('script capture.seal()');
  const order = [source, imported, selected, connected, created, symbols, armed, launched, live, image, continued, status, sealed];
  if (order.some(index => index < 0) || order.some((index, place) => place > 0 && index < order[place - 1])) fail('command order');
  const crash = ['process status', 'script capture.seal()', 'script capture.mark_detach()', 'process detach --keep-stopped true', 'quit'];
  let cursor = sealed;
  for (const command of crash) {
    const index = argv.indexOf(command, command === 'process status' || command === 'script capture.seal()' ? cursor + 1 : cursor);
    if (index < 0 || argv[index - 1] !== '-k' || index < cursor) fail(`crash command ${command}`);
    cursor = index;
  }
  if (argv.some(item => item.includes('--stop-at-entry true'))) fail('stop-at-entry was given an argument');
  if (!launchContract(argv[launched]).ok) fail('inferior arguments changed');
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
  const identity = live.identity ? `TARGET_IDENTITY pid=${pid} start=${live.identity.start} path=${live.identity.path}\n` : '';
  const image = live.image === false ? '' : imageLine(live.uuid);
  const head = live.process === false ? '' : `Process ${pid} launched: '${REMOTE}/consumer' (x86_64)\nProcess ${pid} stopped\n`;
  return `${head}MODULE_LIVE pid=${pid} state=stopped\n${maps}${file}${identity}${image}${body}`;
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
  const exhausted = await expect('budget', { platform: 'remote-android', serial: 'emulator-5554\tdevice', at: { launch: 300001 }, steps: {} }, 'CLEANUP_UNCONFIRMED', 'launch');
  const cleanupProbe = exhausted.log.find(row => row.step === 'cleanup-probe');
  if (!cleanupProbe || cleanupProbe.timeoutMs !== 0 || cleanupProbe.skipped !== true) fail('cleanup added time after the total');
  const timed = await expect('timeout', { platform: 'remote-android', serial: 'emulator-5554\tdevice', timeout: 'launch', steps: {} }, 'UNCAPTURED', null);
  if (timed.log.find(row => row.step === 'launch').timeoutMs !== 60000) fail('active budget is not 60s');
  if (timed.meta.foreground?.pid !== 4242 || timed.meta.foreground.signal !== 'SIGTERM' || timed.meta.foreground.closed !== true || timed.businessExit !== null) {
    fail('timeout did not record this foreground close');
  }
  const timedExit = await expect('timeout-not-exit', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', timeout: 'launch', timeoutStdout: 'Process 2400 exited with status = 0 (0x00000000) \n', steps: {} }, 'UNCAPTURED', null);
  if (timedExit.businessExit !== null || timedExit.meta.foreground?.signal !== 'SIGTERM') fail('a killed debugger became a business exit');
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
  const covered = { complete: true, dead: { '4660': { state: 'dead', event: { frames, tid: 1624, kind: 'destroy', return: 0 } } }, fatal: { frames: fatalFrames } };
  const uncovered = { ...covered, missing: ['pthread_mutex_init'] };
  const missed = await expect('missing-init-saved', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', saved: uncovered, steps: { launch: { rc: 0, stdout: processText(`${chain}Process 2400 exited with status = 134 (0x00000086) \n`), stderr: '' } } }, 'FATAL_UNKNOWN', null);
  if (missed.businessExit !== 134 || missed.association?.chain === 'DESTROY0_THEN_LOCK') fail('saved missing init still attributed one generation');
  const fromFile = await expect('saved-without-stdout', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', saved: covered, steps: { launch: { rc: 0, stdout: processText(`${stop}Process 2400 exited with status = 134 (0x00000086) \n`), stderr: '' } } }, 'FATAL_ATTRIBUTED', null);
  if (fromFile.association?.chain !== 'DESTROY0_THEN_LOCK') fail('saved capture was ignored');
  const intentOnly = await expect('detach-intent-not-completion', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: processText(`${stop}DETACH_INTENT\nProcess 2400 exited with status = 0 (0x00000000) \n`), stderr: '' } } }, 'FATAL_UNKNOWN', null);
  if (intentOnly.businessExit !== 0 || intentOnly.meta.detach.completed || intentOnly.meta.detach.status !== 'UNKNOWN') fail('detach intent erased a real exit or counted as completion');
  const detachedKill = await expect('controlled-detach-exit', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: processText(`${stop}DETACH_INTENT\nProcess 2400 detached\nProcess 2400 exited with status = 0 (0x00000000) \n`), stderr: '' } } }, 'FATAL_STOP_UNEXITED', null);
  if (detachedKill.businessExit !== null || detachedKill.meta.detach.completed !== true || detachedKill.meta.detach.status !== 'DETACHED') fail('formal detach state was read as a natural exit');
  const previousDeadline = process.env.SCENE_DEADLINE_MS;
  process.env.SCENE_DEADLINE_MS = '270000';
  const outer = await expect('outer-deadline', { platform: 'remote-android', serial: 'emulator-5554\tdevice', at: { launch: 260000 }, steps: {} }, 'UNCAPTURED', 'launch');
  const outerLaunch = outer.log.find(row => row.step === 'launch');
  const outerCleanup = outer.log.find(row => row.step === 'cleanup-probe');
  if (!outerLaunch || outerLaunch.timeoutMs !== 0 || !outerCleanup || outerCleanup.timeoutMs !== 5000) fail('shared deadline did not keep cleanup inside 270s');
  Object.assign(cases.at(-1), { launchTimeoutMs: outerLaunch.timeoutMs, cleanupTimeoutMs: outerCleanup.timeoutMs, deadlineTotalMs: outer.meta.deadline.totalMs });
  process.env.SCENE_DEADLINE_MS = '400000';
  const extended = await expect('deadline-no-extend', { platform: 'remote-android', serial: 'emulator-5554\tdevice', at: { launch: 290000 }, steps: {} }, 'UNCAPTURED', 'launch');
  if (extended.log.find(row => row.step === 'launch').timeoutMs !== 0 || extended.meta.deadline.totalMs !== 300000) fail('deadline extended the 300s budget');
  Object.assign(cases.at(-1), { launchTimeoutMs: 0, deadlineTotalMs: extended.meta.deadline.totalMs });
  if (previousDeadline === undefined) delete process.env.SCENE_DEADLINE_MS;
  else process.env.SCENE_DEADLINE_MS = previousDeadline;
  const denied = await drive(scripted({ platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: {} }), '/sdk', 'emulator-5554', { requireDeadline: true });
  if (denied.outcome !== 'DEADLINE_ABSENT' || denied.log.some(row => row.step === 'launch')) fail('execute without a deadline ran');
  cases.push({ name: 'deadline-required', outcome: denied.outcome, steps: denied.log.map(row => row.step), simulated: true, sceneSuccess: false, shellWait: 'UNKNOWN', businessExit: null });
  const target = { start: '55', path: `${REMOTE}/consumer` };
  const reaped = await expect('target-reaped', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: processText('Process 2400 exited with status = 0 (0x00000000) \n', '2400', { identity: target }), stderr: '' }, 'cleanup-target-probe': { rc: 0, stdout: `TARGET_PID=2400\nTARGET_START=55\nTARGET_PATH=${REMOTE}/consumer\nTARGET_STATE=PRESENT\n`, stderr: '' }, 'cleanup-target': { rc: 0, stdout: 'REAPED\n', stderr: '' } } }, 'DIAG_EXITED', null);
  if (reaped.businessExit !== 0 || reaped.meta.targetCleanup !== 'REAPED' || reaped.meta.devicePid === '2400' || !reaped.log.some(row => row.step === 'cleanup-target' && row.argv.join(' ').includes('want_start=55'))) fail('target reap changed the business exit');
  Object.assign(cases.at(-1), { targetCleanup: reaped.meta.targetCleanup, devicePid: reaped.meta.devicePid });
  const notReaped = await expect('target-not-reaped', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: processText('Process 2400 exited with status = 0 (0x00000000) \n', '2400', { identity: target }), stderr: '' }, 'cleanup-target-probe': { rc: 0, stdout: `TARGET_PID=2400\nTARGET_START=55\nTARGET_PATH=${REMOTE}/consumer\nTARGET_STATE=PRESENT\n`, stderr: '' }, 'cleanup-target': { rc: 4, stdout: 'NOT_REAPED\n', stderr: '' } } }, 'DIAG_EXITED', null);
  if (notReaped.businessExit !== 0 || notReaped.meta.targetCleanup !== 'UNCONFIRMED') fail('NOT_REAPED was accepted as REAPED');
  Object.assign(cases.at(-1), { targetCleanup: notReaped.meta.targetCleanup });
  const notReapedZero = await expect('target-not-reaped-rc0', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: processText('Process 2400 exited with status = 0 (0x00000000) \n', '2400', { identity: target }), stderr: '' }, 'cleanup-target-probe': { rc: 0, stdout: `TARGET_PID=2400\nTARGET_START=55\nTARGET_PATH=${REMOTE}/consumer\nTARGET_STATE=PRESENT\n`, stderr: '' }, 'cleanup-target': { rc: 0, stdout: 'NOT_REAPED\n', stderr: '' } } }, 'DIAG_EXITED', null);
  if (notReapedZero.businessExit !== 0 || notReapedZero.meta.targetCleanup !== 'UNCONFIRMED') fail('rc 0 NOT_REAPED was accepted as REAPED');
  Object.assign(cases.at(-1), { targetCleanup: notReapedZero.meta.targetCleanup });
  await expect('server-not-reaped-rc0', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { 'cleanup-server': { rc: 0, stdout: 'NOT_REAPED\n', stderr: '' } } }, 'CLEANUP_UNCONFIRMED', null);
  await expect('host-not-reaped-rc0', { platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: { reap: { rc: 0, stdout: 'NOT_REAPED\n', stderr: '' } } }, 'CLEANUP_UNCONFIRMED', null);
  const reused = await expect('target-reused-pid', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: processText('Process 2400 exited with status = 0 (0x00000000) \n', '2400', { identity: target }), stderr: '' }, 'cleanup-target-probe': { rc: 0, stdout: `TARGET_PID=2400\nTARGET_START=99\nTARGET_PATH=${REMOTE}/consumer\nTARGET_STATE=MISMATCH\n`, stderr: '' }, 'cleanup-target': { rc: 0, stdout: 'REAPED\n', stderr: '' } } }, 'DIAG_EXITED', null);
  if (reused.meta.targetCleanup !== 'UNKNOWN' || reused.businessExit !== 0 || reused.log.some(row => row.step === 'cleanup-target')) fail('a reused pid was killed');
  Object.assign(cases.at(-1), { targetCleanup: reused.meta.targetCleanup, killed: false });
  const lateIdentity = `Process 2400 launched: '${REMOTE}/consumer' (x86_64)\nProcess 2400 stopped\nMODULE_LIVE pid=2400 state=stopped\nFILE_SHA ${ORIGINAL_SO}  ${RUNTIME_PATH}\n${imageLine()}Process 2400 exited with status = 0 (0x00000000) \nTARGET_IDENTITY pid=2400 start=55 path=${REMOTE}/consumer\nMAPS_BEGIN\n7f0000000000-7f0000001000 r-xp 00000000 00:00 1 ${RUNTIME_PATH}\nMAPS_END\n`;
  const lateTarget = await expect('identity-after-exit', { platform: 'remote-android', serial: 'emulator-5554\tdevice', runtimeSo: '/opt/libcangjie-runtime.so', steps: { launch: { rc: 0, stdout: lateIdentity, stderr: '' } } }, 'MAPS_ABSENT', null);
  if (lateTarget.meta.target || lateTarget.meta.targetCleanup !== 'UNKNOWN' || lateTarget.log.some(row => row.step === 'cleanup-target')) fail('identity after exit armed a kill');
  Object.assign(cases.at(-1), { targetCleanup: lateTarget.meta.targetCleanup, killed: false });
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

if (argv[0] === 'reap-check') {
  const child = spawn('sleep', ['30'], { stdio: ['ignore', 'pipe', 'pipe'] });
  const started = Date.now();
  const result = await watchChild(child, 150, 800);
  const elapsed = Date.now() - started;
  if (!result.closed || result.pid !== child.pid || !['SIGTERM', 'SIGKILL'].includes(result.signal) || elapsed >= 2000 || result.timedOut !== true) {
    console.error(JSON.stringify({ result, elapsed }));
    process.exit(1);
  }
  console.log(`REAP_PASS pid=${result.pid} signal=${result.signal} elapsed=${elapsed}`);
  process.exit(0);
}

if (argv[0] === 'self-test') {
  const source = process.env.SCENE_COMMANDS;
  if (!source) fail('SCENE_COMMANDS unset');
  const records = JSON.parse(readFileSync(source, 'utf8'));
  const selected = route(records);
  if (selected.launch.at(-1) !== 'preinit') fail('self-test launch');
  const command = launchArgv('/sdk/lldb').find(item => item.startsWith('process launch '));
  if (!launchContract(command).ok || launchContract(command).argc !== 3) fail('launch contract');
  if (launchContract(command.replace('--stop-at-entry', '--stop-at-entry true')).ok) fail('old true argument was accepted');
  if (sliceMs(300000, 5000, false) !== 0) fail('a stop still adds time past the total');
  if (sliceMs(290000, 5000, true) !== 5000) fail('cleanup reserve was removed');
  if (sliceMs(250000, 60000, false) !== 30000) fail('active work ate the cleanup reserve');
  if (sliceMs(260000, 60000, false, 300000) !== 20000 || sliceMs(260000, 60000, false, 270000) !== 0) fail('outer deadline was not applied');
  if (reapConfirmed(4, 'NOT_REAPED\n') || reapConfirmed(0, 'NOT_REAPED\n') || !reapConfirmed(0, 'REAPED\n') || !reapConfirmed(0, 'REAPED')) fail('reap token is not a whole line');
  const sceneText = readFileSync(join(HERE, 'scene.mjs'), 'utf8');
  const branchAt = sceneText.indexOf('          if (cleaned.skipped || cleaned.timedOut) meta.targetCleanup');
  const branchEnd = sceneText.indexOf('\n        } else if (verdict', branchAt);
  if (branchAt < 0 || branchEnd <= branchAt) fail('target reap branch is not extractable');
  const exact = sceneText.slice(branchAt, branchEnd);
  const red = { cleaned: { rc: 4, stdout: 'NOT_REAPED\n' }, meta: {} };
  const control = { cleaned: { rc: 0, stdout: 'REAPED\n' }, meta: {} };
  runInNewContext(exact, red);
  runInNewContext(exact, control);
  if (red.meta.targetCleanup !== 'UNCONFIRMED' || control.meta.targetCleanup !== 'REAPED') fail('extracted reap branch still accepts NOT_REAPED');
  writeFileSync(join(HERE, 'reap-predicate.json'), JSON.stringify({ exact, red: red.meta.targetCleanup, control: control.meta.targetCleanup, nativeRuns: 0 }, null, 2) + '\n');
  if (budgetBound(0).total !== 300000) fail('missing deadline changed the internal budget');
  process.env.SCENE_DEADLINE_MS = '400000';
  if (budgetBound(0).total !== 300000) fail('absolute deadline raised the 300s cap');
  delete process.env.SCENE_DEADLINE_MS;
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
} else if (argv[0] === 'interface-check') {
  const { EventEmitter } = await import('node:events');
  const options = readFileSync('/root/cj_build/llvm_src/cjcj-llvm/lldb/source/Commands/Options.td', 'utf8');
  const definition = options.match(/def process_detach_keep_stopped\s*:[\s\S]*?;/)?.[0];
  if (!definition || !definition.includes('Arg<"Boolean">')) fail('keep-stopped is not a Boolean option');
  const stopAtEntry = options.match(/def process_launch_stop_at_entry\s*:[\s\S]*?;/)?.[0];
  if (!stopAtEntry || stopAtEntry.includes('Arg<')) fail('stop-at-entry gained an argument');
  const booleans = new Set(['true', 'false', 'on', 'off', 'yes', 'no', '1', '0']);
  const crash = launchArgv('/sdk/lldb').filter((item, index, all) => all[index - 1] === '-k');
  const expected = ['process status', 'script capture.seal()', 'script capture.mark_detach()', 'process detach --keep-stopped true', 'quit'];
  if (crash.join('\n') !== expected.join('\n')) fail(`crash commands ${crash.join(' | ')}`);
  const detach = crash[3];
  const detachArg = detach.split(/\s+/)[detach.split(/\s+/).indexOf('--keep-stopped') + 1];
  if (!booleans.has(detachArg)) fail('detach boolean missing');
  const rejected = 'process detach --keep-stopped';
  const rejectedArg = rejected.split(/\s+/)[rejected.split(/\s+/).indexOf('--keep-stopped') + 1];
  if (booleans.has(rejectedArg)) fail('missing boolean was accepted');
  const handle = launchArgv('/sdk/lldb').find(item => item.startsWith('process handle '));
  for (const name of ['--stop', '--pass', '--notify']) {
    const tokens = handle.split(/\s+/);
    const value = tokens[tokens.indexOf(name) + 1];
    if (!booleans.has(value)) fail(`${name} is not a Boolean value`);
  }
  const intent = detachFact(sbProcess("Process 2400 launched: '/data/local/tmp/readscope-lifetime/consumer' (x86_64)\nDETACH_INTENT\nProcess 2400 exited with status = 0 (0x00000000) \n"));
  const intentProc = sbProcess("Process 2400 launched: '/data/local/tmp/readscope-lifetime/consumer' (x86_64)\nDETACH_INTENT\nProcess 2400 exited with status = 0 (0x00000000) \n");
  if (intent.completed || intent.status !== 'UNKNOWN' || intentProc.exits[0].controlled) fail('intent counted as detach completion');
  const formal = sbProcess("Process 2400 launched: '/data/local/tmp/readscope-lifetime/consumer' (x86_64)\nDETACH_INTENT\nProcess 2400 detached\nProcess 2400 exited with status = 0 (0x00000000) \n");
  const formalFact = detachFact(formal);
  if (!formalFact.completed || formalFact.status !== 'DETACHED' || !formal.exits[0].controlled || formal.exits[0].status !== 0) fail('formal detach state was ignored');
  const failed = detachFact(sbProcess('DETACH_INTENT\nDetach failed: lost connection\n'));
  if (failed.completed || failed.status !== 'UNKNOWN' || !failed.error) fail('detach failure became completion');
  const statusDef = options.match(/def process_status_verbose\s*:[\s\S]*?;/)?.[0];
  if (!statusDef || statusDef.includes('Arg<')) fail('process status verbose is not a flag');
  const scriptDef = options.match(/def script_language\s*:[\s\S]*?;/)?.[0];
  if (!scriptDef || !scriptDef.includes('EnumArg<"ScriptLang">')) fail('script language option changed');
  if (crash.some(item => item.startsWith('--') || item.includes('--language') || item.includes('--verbose'))) fail('control segment invented an optional flag');
  const quitSrc = readFileSync('/root/cj_build/llvm_src/cjcj-llvm/lldb/source/Commands/CommandObjectQuit.cpp', 'utf8');
  if (!quitSrc.includes('"quit [exit-code]"') || !quitSrc.includes('eArgTypeUnsignedInteger')) fail('quit is not an optional unsigned exit code');
  if (crash[4] !== 'quit') fail('quit was given a flag');
  const sceneText = readFileSync(join(HERE, 'scene.mjs'), 'utf8');
  const fragment = sceneText.slice(sceneText.indexOf('function readProc('), sceneText.indexOf('\nfunction plan('));
  let queued = null;
  const loaded = runInNewContext(`${fragment}\n({ watchChild, realIO })`, {
    spawn() { return queued; },
    setTimeout, clearTimeout, process, join, HERE, TOTAL_MS, existsSync, readFileSync,
  });
  const hostChild = (pid, fields = {}) => {
    const child = new EventEmitter();
    child.pid = pid;
    child.exitCode = Object.hasOwn(fields, 'exitCode') ? fields.exitCode : null;
    child.signalCode = Object.hasOwn(fields, 'signalCode') ? fields.signalCode : null;
    child.stdout = new EventEmitter();
    child.stderr = new EventEmitter();
    child.kills = [];
    child.kill = signal => { child.kills.push(signal); return true; };
    return child;
  };
  const hostFact = reaped => reaped.closed === true && reapConfirmed(reaped.rc, reaped.stdout);
  queued = hostChild(424242, { exitCode: 0, signalCode: null });
  const openIo = loaded.realIO();
  await openIo.run('start-server', ['fake-only'], { background: true });
  const open = await openIo.reap(424242, 20);
  if (open.closed !== false || hostFact(open) || queued.kills.length !== 0) fail('exitCode became a close');
  queued = hostChild(424243);
  const closeIo = loaded.realIO();
  await closeIo.run('start-server', ['fake-only'], { background: true });
  queued.emit('exit', 0, null);
  queued.emit('close', 0, null);
  const closed = await closeIo.reap(424243, 20);
  if (!hostFact(closed) || closed.childRc !== 0 || closed.childSignal !== null || queued.kills.length !== 0) fail('observed close was not reaped');
  queued = hostChild(424244);
  const exitIo = loaded.realIO();
  await exitIo.run('start-server', ['fake-only'], { background: true });
  queued.emit('exit', 0, null);
  const exitOnly = await exitIo.reap(424244, 15);
  if (exitOnly.closed !== false || exitOnly.childRc !== 0 || hostFact(exitOnly) || queued.kills.length !== 0) fail('exit without close was reaped or signaled');
  queued = hostChild(424245);
  const liveIo = loaded.realIO();
  await liveIo.run('start-server', ['fake-only'], { background: true });
  const live = await liveIo.reap(424245, 15);
  if (live.closed !== false || hostFact(live) || queued.kills.join(',') !== 'SIGTERM') fail('live reap dropped the close requirement');
  const external = new EventEmitter();
  external.pid = 424242;
  external.stdout = new EventEmitter();
  external.stderr = new EventEmitter();
  external.kill = () => true;
  const externalRun = watchChild(external, 100, 20);
  queueMicrotask(() => external.emit('close', null, 'SIGKILL'));
  const externalResult = await externalRun;
  if (externalResult.signal !== 'SIGKILL' || externalResult.timer !== false || externalResult.closed !== true) fail('external signal was cleared');
  const normal = new EventEmitter();
  normal.pid = 7;
  normal.stdout = new EventEmitter();
  normal.stderr = new EventEmitter();
  normal.kill = () => true;
  const normalRun = watchChild(normal, 100, 20);
  queueMicrotask(() => normal.emit('close', 0, null));
  const normalResult = await normalRun;
  if (normalResult.rc !== 0 || normalResult.signal !== null || normalResult.timer !== false || normalResult.closed !== true) fail('normal close was rewritten');
  const timed = new EventEmitter();
  timed.pid = 8;
  timed.stdout = new EventEmitter();
  timed.stderr = new EventEmitter();
  timed.kill = () => true;
  const timedRun = watchChild(timed, 15, 40);
  setTimeout(() => timed.emit('close', null, 'SIGTERM'), 25);
  const timedResult = await timedRun;
  if (timedResult.signal !== 'SIGTERM' || timedResult.timer !== true || timedResult.timedOut !== true || timedResult.closed !== true) fail('timeout rewrote the close signal');
  const report = {
    detach: { command: detach, argument: detachArg, rejected: rejectedArg ?? null, intent, formal: formalFact, failed },
    hostExitWithoutClose: { closed: open.closed, reaped: hostFact(open), kills: 0 },
    hostClose: { closed: closed.closed, reaped: hostFact(closed), childRc: closed.childRc, kills: 0 },
    hostExitOnly: { closed: exitOnly.closed, childRc: exitOnly.childRc, reaped: hostFact(exitOnly), kills: 0 },
    hostLive: { closed: live.closed, reaped: hostFact(live), signalled: 'SIGTERM' },
    foregroundExternal: { signal: externalResult.signal, timer: externalResult.timer, closed: externalResult.closed },
    foregroundNormal: { rc: normalResult.rc, signal: normalResult.signal, timer: normalResult.timer },
    foregroundTimeout: { signal: timedResult.signal, timer: timedResult.timer, timedOut: timedResult.timedOut },
    nativeRuns: 0,
  };
  writeFileSync(join(HERE, 'interface-check.json'), JSON.stringify(report, null, 2) + '\n');
  console.log('INTERFACE_PASS');
  process.exit(0);
} else if (argv[0] === 'host-check') {
  const sdk = process.env.ANDROID_SDK_ROOT;
  if (!sdk) fail('ANDROID_SDK_ROOT unset');
  const io = realIO();
  const log = [];
  const run = async (step, args) => {
    const result = await io.run(step, args, { timeoutMs: 30000 });
    log.push({ step, argv: args, rc: result.rc ?? null, timedOut: Boolean(result.timedOut), stdout: result.stdout ?? '', stderr: result.stderr ?? '' });
    return result;
  };
  const probed = await probeHost(io, sdk, run);
  const outcome = probed.outcome || 'HOST_READY';
  const result = finish(outcome, false, log, { meta: { closure: probed.closure ?? null, launcher: probed.launcher ?? null, nativeWait: 'ABSENT', devicePid: null, target: null, businessExit: null, remoteAndroid: outcome === 'HOST_READY' } });
  writeFileSync(join(HERE, 'host-check.json'), JSON.stringify(result, null, 2) + '\n');
  console.log(outcome);
  process.exit(outcome === 'HOST_READY' ? 0 : 1);
} else if (argv[0] === 'host-closure-check') {
  if (sha256(OFFICIAL_LAUNCHER) !== LAUNCHER_SHA) fail('embedded launcher does not match the r27d zip');
  const lddPath = '/root/cj_build/agent_scratch/rev_ten_perf_std_rebuild_sol_1113_retry/readscope-retained/android-scene-bounds-preparation/external-after-exact-authorization/artifacts/14196974102/_temp/readscope-android-scene/host-lldb-dependencies.stdout';
  const executedPath = '/root/cj_build/agent_scratch/rev_ten_perf_std_rebuild_sol_1113_retry/readscope-retained/android-scene-bounds-preparation/external-after-exact-authorization/artifacts/14196974102/cangjie-runtime/cangjie-runtime/runtime/tests/readscope_native_retention/android-scene/execute-result.json';
  const lddText = readFileSync(lddPath, 'utf8');
  const closed = closureDecision(lddText, true);
  const open = closureDecision(lddText, false);
  if (!closed.ok || closed.closedByLauncher.join(',') !== PYTHON_SONAME || closed.open.length !== 0) fail('packaged python did not close the recorded gap');
  if (open.ok || open.open.join(',') !== PYTHON_SONAME) fail('missing packaged python was treated as closed');
  const executed = JSON.parse(readFileSync(executedPath, 'utf8'));
  const bare = executed.log[0];
  if (bare.argv[0].endsWith('/lldb.sh') || bare.rc !== 127 || !bare.stderr.includes(PYTHON_SONAME)) fail('recorded bare launch was rewritten');
  const positive = scripted({ platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: {} });
  process.env.HOST_SYMBOLS = '/symbols/libcangjie-runtime.so';
  const ready = await probeHost(positive, '/sdk', (step, args) => positive.run(step, args));
  if (ready.outcome || !ready.launcher.endsWith('/lldb.sh')) fail('launcher probe did not reach platform list');
  if (positive.runs.some(step => ['adb-devices', 'start-server', 'launch'].includes(step))) fail('host probe started a device or target');
  if (!positive.runs.includes('host-version') || positive.runs.at(positive.runs.indexOf('host-version') - 1) !== 'host-ldd') fail('version ran before the dependency text');
  const versionArgv = hostArgv('/sdk', ['--version']);
  if (versionArgv[0] === bare.argv[0] || !versionArgv[0].endsWith('/lldb.sh') || versionArgv[1] !== '--version') fail('version argv fell back to the bare binary');
  const blocked = scripted({ steps: { 'host-ldd': { rc: 0, stdout: `\t${PYTHON_SONAME} => not found\n\tlibncurses.so.6 => not found\n`, stderr: '' } } });
  const blockedResult = await probeHost(blocked, '/sdk', (step, args) => blocked.run(step, args));
  if (blockedResult.outcome !== 'HOST_CLOSURE_ABSENT' || blocked.runs.includes('host-version')) fail('an unclosed soname still launched lldb');
  const mismatched = scripted({});
  mismatched.readText = () => '#!/bin/bash\nexec "$0" "$@"\n';
  const mismatch = await probeHost(mismatched, '/sdk', (step, args) => mismatched.run(step, args));
  if (mismatch.outcome !== 'LAUNCHER_MISMATCH' || mismatched.runs.includes('host-ldd')) fail('a different launcher was accepted');
  const absentVersion = scripted({ platform: 'remote-android', steps: { 'host-version': { rc: 127, stdout: '', stderr: `${PYTHON_SONAME}\n` } } });
  const absent = await probeHost(absentVersion, '/sdk', (step, args) => absentVersion.run(step, args));
  if (absent.outcome !== 'VERSION_ABSENT' || absentVersion.runs.includes('platform-list')) fail('version failure continued into platform discovery');
  const officialHost = JSON.parse(readFileSync('/root/cj_build/agent_scratch/rev_ten_perf_std_rebuild_sol_1113_retry/readscope-retained/android-scene-e4f9-official-once/artifacts/14201471674/cangjie-runtime/cangjie-runtime/runtime/tests/readscope_native_retention/android-scene/host-check.json', 'utf8'));
  const officialList = officialHost.log.find(row => row.step === 'platform-list');
  if (!officialList || officialList.rc !== 0 || !hasRemoteAndroid(officialList.stdout)) fail('official platform list was rejected');
  const officialProbe = scripted({ platform: officialList.stdout, serial: 'emulator-5554\tdevice', steps: {} });
  const officialReady = await probeHost(officialProbe, '/sdk', (step, args) => officialProbe.run(step, args));
  if (officialReady.outcome || officialProbe.runs.includes('adb-devices')) fail('official platform text did not pass the host probe');
  const colonAbsent = scripted({ platform: 'remote-linux: Remote Linux user platform plug-in.\n', serial: 'emulator-5554\tdevice', steps: {} });
  const colonResult = await probeHost(colonAbsent, '/sdk', (step, args) => colonAbsent.run(step, args));
  if (colonResult.outcome !== 'PLATFORM_ABSENT' || colonResult.extra?.meta?.remoteAndroid !== false) fail('a different colon plugin became remote-android');
  for (const text of ['', 'remote-linux remote-gdb-server', 'remote-androidian: plug-in', 'notremote-android: plug-in', 'remote-android-extra: plug-in']) {
    if (hasRemoteAndroid(text)) fail(`platform text counted ${text}`);
  }
  const report = {
    launcherSha: LAUNCHER_SHA,
    recordedBare: { argv: bare.argv, rc: bare.rc },
    lddRcWouldNotClose: true,
    packaged: closed,
    unpackaged: open,
    versionArgv,
    probeSteps: positive.runs,
    officialPlatform: { rc: officialList.rc, accepted: officialReady.outcome === null, remoteAndroid: true },
    rejectedPlatformText: ['', 'remote-linux remote-gdb-server', 'remote-androidian: plug-in', 'notremote-android: plug-in', 'remote-android-extra: plug-in', 'remote-linux: Remote Linux user platform plug-in.'],
    nativeRuns: 0,
  };
  writeFileSync(join(HERE, 'host-closure.json'), JSON.stringify(report, null, 2) + '\n');
  console.log('HOST_CLOSURE_PASS');
  process.exit(0);
} else if (argv[0] === 'ownership-check') {
  const { EventEmitter } = await import('node:events');
  mkdirSync(join(HERE, 'run'), { recursive: true });
  const hold = join(HERE, 'run', 'ownership-hold.mjs');
  const outsider = join(HERE, 'run', 'ownership-outsider.mjs');
  const outsiderFile = join(HERE, 'run', 'ownership-outsider.pid');
  const launcher = join(HERE, 'run', 'ownership-launcher.sh');
  writeFileSync(outsider, "process.on('SIGHUP', () => {});\nprocess.on('SIGTERM', () => {});\nsetInterval(() => {}, 1000);\n");
  writeFileSync(hold, `import { spawn } from 'node:child_process';\nimport { writeFileSync } from 'node:fs';\nconst child = spawn(${JSON.stringify(process.execPath)}, [${JSON.stringify(outsider)}], { detached: true, stdio: 'ignore' });\nwriteFileSync(${JSON.stringify(outsiderFile)}, String(child.pid));\nchild.unref();\nprocess.on('SIGHUP', () => {});\nprocess.on('SIGTERM', () => {});\nsetInterval(() => {}, 1000);\n`);
  writeFileSync(launcher, `#!/bin/bash\n${JSON.stringify(process.execPath)} "$1" &\necho "holder=$!"\nwait\n`);
  const sceneText = readFileSync(join(HERE, 'scene.mjs'), 'utf8');
  const begin = sceneText.indexOf('function readProc(');
  const end = sceneText.indexOf('\nfunction plan(', begin);
  if (begin < 0 || end <= begin) fail('ownership fragment missing');
  const fragment = sceneText.slice(begin, end);
  const eventFixture = kind => {
    const signals = [];
    const spawnCalls = [];
    let descendantAlive = kind === 'wrapped-timeout';
    const child = new EventEmitter();
    child.pid = 101;
    child.stdout = new EventEmitter();
    child.stderr = new EventEmitter();
    child.kill = signal => {
      signals.push(signal);
      if (kind === 'wrapped-timeout') child.emit('exit', null, signal);
      else child.emit('close', null, signal);
      return true;
    };
    const io = runInNewContext(`${fragment}\nrealIO()`, {
      Map, Promise, Boolean, Number, String, Math, Date, setTimeout, clearTimeout,
      process: { env: {} }, HERE: '/sealed', TOTAL_MS: 40, join,
      spawn(program, args, options) {
        spawnCalls.push({ program, detached: options.detached, background: false });
        if (kind === 'normal') setTimeout(() => child.emit('close', 0, null), 0);
        return child;
      },
      existsSync: () => false,
      readFileSync() { throw new Error('event fixture has no process identity'); },
    });
    return { io, signals, spawnCalls, descendantAlive: () => descendantAlive };
  };
  const direct = eventFixture('direct-timeout');
  const directResult = await direct.io.run('launch', ['/sealed/direct-event-child'], { timeoutMs: 40 });
  if (!directResult.timedOut || directResult.closed !== true || directResult.signal !== 'SIGTERM' || direct.signals.join(',') !== 'SIGTERM' || directResult.ownership !== 'NO_IDENTITY') fail('direct control lost');
  const normalEvent = eventFixture('normal');
  const normalEventResult = await normalEvent.io.run('launch', ['/sealed/bin/lldb.sh'], { timeoutMs: 40 });
  if (normalEventResult.rc !== 0 || normalEventResult.closed !== true || normalEventResult.timedOut || normalEvent.signals.length !== 0) fail('normal control lost');
  const wrapped = eventFixture('wrapped-timeout');
  const wrappedResult = await wrapped.io.run('launch', ['/sealed/bin/lldb.sh', '--batch'], { timeoutMs: 40 });
  if (!wrapped.descendantAlive() || wrappedResult.closed !== false || wrappedResult.signal !== null || wrappedResult.owned !== 'UNCONFIRMED' || wrappedResult.ownership !== 'NO_IDENTITY') fail('event fixture hid the descendant');
  const seen = [];
  const split = runInNewContext(`${fragment}\nrealIO()`, {
    Map, Promise, Boolean, Number, String, Math, Date, setTimeout, clearTimeout,
    process: { env: {}, kill() { throw new Error('split must not signal'); } },
    HERE, TOTAL_MS: 40, join, existsSync: () => false,
    readFileSync() { throw new Error('no proc'); },
    spawn(program, args, options) {
      seen.push(options.detached === true);
      const child = new EventEmitter();
      child.pid = 77;
      child.stdout = new EventEmitter();
      child.stderr = new EventEmitter();
      child.kill = () => true;
      if (options.detached) setTimeout(() => child.emit('close', 0, null), 0);
      return child;
    },
  });
  await split.run('start-server', ['adb'], { background: true });
  await split.run('host-version', ['lldb.sh'], { timeoutMs: 40 });
  if (seen[0] !== false || seen[1] !== true) fail(`detached split ${seen.join(',')}`);
  const originalKill = process.kill;
  let groupKilled = false;
  process.kill = () => { groupKilled = true; throw new Error('reused'); };
  let reused = 'UNSET';
  try {
    reused = signalOwned({ pid: 1, kill() { groupKilled = true; } }, { pid: 1, start: '10', pgrp: '1' }, 'SIGTERM', () => ({ start: '99', pgrp: '1' }));
  } finally {
    process.kill = originalKill;
  }
  if (reused !== 'REUSED' || groupKilled) fail('reused pid was signaled');
  const memberCalls = [];
  process.kill = (pid, signal) => { memberCalls.push([pid, signal]); };
  let memberHow = 'UNSET';
  let reusedMember = 'UNSET';
  try {
    memberHow = signalOwned({ pid: 50, kill() { memberCalls.push(['child', 'SIGKILL']); } }, {
      pid: 50, start: '10', pgrp: '50',
      members: [{ pid: 50, start: '10', pgrp: '50' }, { pid: 60, start: '11', pgrp: '50' }],
    }, 'SIGKILL', pid => Number(pid) === 50 ? null : { start: '11', pgrp: '50' });
    reusedMember = signalOwned({ pid: 50, kill() { memberCalls.push(['child', 'SIGTERM']); } }, {
      pid: 50, start: '10', pgrp: '50',
      members: [{ pid: 61, start: '12', pgrp: '50' }],
    }, 'SIGKILL', () => ({ start: '99', pgrp: '50' }));
  } finally {
    process.kill = originalKill;
  }
  if (memberHow !== 'MEMBER' || memberCalls.some(([pid]) => pid < 0) || memberCalls.join('|') !== '60,SIGKILL') fail(`leader exit did not stay on the recorded member ${memberCalls}`);
  if (reusedMember !== 'REUSED' || memberCalls.length !== 1) fail('reused leader still reached a member');
  const live = pid => {
    try { return !/^State:\s+Z/m.test(readFileSync(`/proc/${pid}/status`, 'utf8')); } catch { return false; }
  };
  const killSame = (pid, start) => {
    const now = readProc(pid);
    if (!pid || !start || !now || now.start !== start) return false;
    try { process.kill(pid, 'SIGKILL'); return true; } catch { return false; }
  };
  const outsiderIdentity = async () => {
    const started = Date.now();
    while (Date.now() - started < 1000) {
      if (existsSync(outsiderFile)) {
        const pid = Number(readFileSync(outsiderFile, 'utf8'));
        const start = readProc(pid)?.start;
        if (pid && start) return { pid, start };
      }
      await new Promise(resolve => setTimeout(resolve, 20));
    }
    return null;
  };
  const directChild = spawn('bash', [launcher, hold], { detached: true, stdio: ['ignore', 'pipe', 'pipe'] });
  let directText = '';
  let holderPid = 0;
  try {
    holderPid = await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('holder line missing')), 2000);
      directChild.stdout.on('data', chunk => {
        directText += chunk;
        const match = directText.match(/holder=(\d+)/);
        if (match) { clearTimeout(timer); resolve(Number(match[1])); }
      });
      directChild.once('close', () => { clearTimeout(timer); reject(new Error('launcher closed before holder')); });
    });
    const leader = groupIdentity(directChild.pid);
    const holderStart = readProc(holderPid)?.start;
    const outside = await outsiderIdentity();
    if (!leader || leader.pgrp !== String(directChild.pid)) fail('launcher is not its own group leader');
    process.kill(directChild.pid, 'SIGTERM');
    await new Promise(resolve => setTimeout(resolve, 400));
    if (!live(holderPid)) fail('direct kill already removed the descendant');
    if (outside && !live(outside.pid)) fail('direct kill reached the off-group process');
  } finally {
    const holderStart = readProc(holderPid)?.start;
    const outside = existsSync(outsiderFile) ? { pid: Number(readFileSync(outsiderFile, 'utf8')), start: null } : null;
    if (outside) outside.start = readProc(outside.pid)?.start;
    killSame(holderPid, holderStart);
    if (outside) killSame(outside.pid, outside.start);
    const leaderNow = readProc(directChild.pid);
    if (leaderNow) killSame(directChild.pid, leaderNow.start);
  }
  await new Promise(resolve => { if (directChild.exitCode != null || directChild.signalCode != null) resolve(); else directChild.once('close', resolve); });
  const redChild = spawn('bash', [launcher, hold], { detached: true, stdio: ['ignore', 'pipe', 'pipe'] });
  let redText = '';
  let redHolder = 0;
  let redOutside = null;
  let oldUpgrade = 'UNSET';
  try {
    redHolder = await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('resistant holder missing')), 2000);
      redChild.stdout.on('data', chunk => {
        redText += chunk;
        const match = redText.match(/holder=(\d+)/);
        if (match) { clearTimeout(timer); resolve(Number(match[1])); }
      });
    });
    const redLeader = groupIdentity(redChild.pid);
    redOutside = await outsiderIdentity();
    if (!redLeader || !redOutside) fail('resistant fixture did not publish its group');
    const leaderExited = new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('leader was not reaped')), 2000);
      redChild.once('exit', () => { clearTimeout(timer); resolve(); });
    });
    process.kill(-redChild.pid, 'SIGTERM');
    await leaderExited;
    const after = readProc(redChild.pid);
    oldUpgrade = !after || after.start !== redLeader.start ? 'REUSED' : 'GROUP';
    if (oldUpgrade !== 'REUSED' || !live(redHolder)) fail('old upgrade was not red after the leader exited');
    if (!live(redOutside.pid)) fail('group TERM reached the off-group process');
  } finally {
    const holderStart = readProc(redHolder)?.start;
    killSame(redHolder, holderStart);
    if (redOutside) killSame(redOutside.pid, redOutside.start);
    const leaderNow = readProc(redChild.pid);
    if (leaderNow) killSame(redChild.pid, leaderNow.start);
  }
  await new Promise(resolve => { if (redChild.exitCode != null || redChild.signalCode != null) resolve(); else redChild.once('close', resolve); });
  const produced = await realIO().run('host-version', ['bash', launcher, hold], { timeoutMs: 1200 });
  const producedHolder = Number((String(produced.stdout || '').match(/holder=(\d+)/) || [])[1] || 0);
  const producedGone = producedHolder !== 0 && !existsSync(`/proc/${producedHolder}`);
  const producedOutside = existsSync(outsiderFile) ? Number(readFileSync(outsiderFile, 'utf8')) : 0;
  const producedOutsideStart = readProc(producedOutside)?.start;
  const offGroupAlive = producedOutside !== 0 && live(producedOutside);
  if (!produced.timedOut || produced.closed !== true || produced.owned !== 'CLOSED' || produced.ownership !== 'GROUP' || produced.escalation !== 'MEMBER' || !producedGone || !offGroupAlive) {
    killSame(producedHolder, readProc(producedHolder)?.start);
    killSame(producedOutside, producedOutsideStart);
    fail(`group upgrade did not close only the in-group descendant ${JSON.stringify({ closed: produced.closed, owned: produced.owned, ownership: produced.ownership, escalation: produced.escalation, gone: producedGone, offGroupAlive, signal: produced.signal })}`);
  }
  killSame(producedOutside, producedOutsideStart);
  const quick = await realIO().run('host-version', ['bash', '-c', 'exit 0'], { timeoutMs: 2000 });
  if (quick.rc !== 0 || quick.closed !== true || quick.timedOut || quick.owned !== 'CLOSED' || quick.ownership !== 'GROUP') fail('normal launcher completion changed');
  const previousDeadline = process.env.SCENE_DEADLINE_MS;
  const previousSymbols = process.env.HOST_SYMBOLS;
  process.env.SCENE_DEADLINE_MS = '1';
  process.env.HOST_SYMBOLS = '/symbols/libcangjie-runtime.so';
  let budget;
  try {
    budget = await drive(scripted({ platform: 'remote-android', serial: 'emulator-5554\tdevice', steps: {} }), '/sdk', 'emulator-5554');
  } finally {
    if (previousDeadline === undefined) delete process.env.SCENE_DEADLINE_MS;
    else process.env.SCENE_DEADLINE_MS = previousDeadline;
    if (previousSymbols === undefined) delete process.env.HOST_SYMBOLS;
    else process.env.HOST_SYMBOLS = previousSymbols;
  }
  const skipped = budget.log.find(row => row.step === 'host-ldd');
  if (budget.outcome !== 'UNCAPTURED' || skipped?.skipped !== true || skipped?.owned === 'CLOSED' || budget.sceneSuccess !== false || budget.businessExit !== null || budget.meta?.foreground) fail('exhausted budget looked confirmed');
  const report = {
    eventRed: { descendantAlive: wrapped.descendantAlive(), closed: wrappedResult.closed, signal: wrappedResult.signal, owned: wrappedResult.owned },
    directControl: { closed: directResult.closed, signal: directResult.signal, timedOut: directResult.timedOut, ownership: directResult.ownership },
    normalControl: { rc: normalEventResult.rc, closed: normalEventResult.closed, timedOut: normalEventResult.timedOut },
    detachedSplit: seen,
    reused: { status: reused, signaled: groupKilled },
    leaderGone: { status: memberHow, signals: memberCalls },
    reusedLeaderStopsMembers: reusedMember,
    directKillLeavesDescendant: true,
    oldUpgradeWhileDescendantAlive: oldUpgrade,
    groupTimeout: { closed: produced.closed, owned: produced.owned, ownership: produced.ownership, escalation: produced.escalation, signal: produced.signal, descendantGone: producedGone, offGroupAlive },
    normalCompletion: { rc: quick.rc, closed: quick.closed, owned: quick.owned, ownership: quick.ownership, timedOut: quick.timedOut },
    budgetExhausted: { outcome: budget.outcome, skipped: skipped?.skipped === true, sceneSuccess: budget.sceneSuccess, businessExit: budget.businessExit },
    nativeRuns: 0,
  };
  writeFileSync(join(HERE, 'ownership.json'), JSON.stringify(report, null, 2) + '\n');
  console.log('OWNERSHIP_PASS');
  process.exit(0);
} else if (argv[0] === 'execute') {
  if (process.env.SCENE_EXECUTE !== '1') {
    console.error('NOT_AUTHORIZED');
    process.exit(2);
  }
  const sdk = process.env.ANDROID_SDK_ROOT;
  if (!sdk) fail('ANDROID_SDK_ROOT unset');
  const source = process.env.SCENE_COMMANDS;
  route(JSON.parse(readFileSync(source, 'utf8')));
  const result = await drive(realIO(), sdk, process.env.ANDROID_SERIAL || 'emulator-5554', { requireDeadline: true });
  writeFileSync(join(HERE, 'execute-result.json'), JSON.stringify(result, null, 2) + '\n');
  console.log(result.outcome);
  const detachDone = result.meta?.detach?.completed === true;
  const unconfirmedStop = result.outcome === 'FATAL_STOP_UNEXITED' && !detachDone;
  process.exit(String(result.outcome).startsWith('FATAL_') && !unconfirmedStop ? 0 : 1);
} else {
  fail('usage: scene.mjs self-test|interface-check|host-closure-check|ownership-check|host-check|calibrate|execute');
}
