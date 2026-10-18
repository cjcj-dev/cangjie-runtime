#!/usr/bin/env node
// GNU jobserver adapter for Ninja versions without native jobserver support.
// The orchestration process donates its implicit slot while it only waits.
// Each command holds one byte for its child's implicit slot; cjc's existing
// ConcurrencyBudget borrows additional bytes from exactly the same FIFO.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {supervisedCommand} from './command-supervisor.mjs';
import {spawn, spawnSync} from 'node:child_process';
import {setTimeout as delay} from 'node:timers/promises';
import {fileURLToPath} from 'node:url';

const {O_RDONLY, O_WRONLY, O_RDWR, O_NONBLOCK} = fs.constants;
const flagNames = ['CARGO_MAKEFLAGS', 'MAKEFLAGS', 'MFLAGS'];
const ownPath = fileURLToPath(import.meta.url);
function trace(event, fields = {}) {
  if (!process.env.CANGJIE_JOBSERVER_LOG) return;
  fs.appendFileSync(process.env.CANGJIE_JOBSERVER_LOG,
    JSON.stringify({event, pid: process.pid, time_ns: process.hrtime.bigint().toString(), ...fields}) + '\n');
}
function authFromEnvironment(env = process.env) {
  for (const key of flagNames) {
    const matches = [...(env[key] || '').matchAll(/(?:^|\s)--jobserver-(?:auth|fds)=([^\s]+)/g)];
    if (matches.length) return matches.at(-1)[1];
  }
  return undefined;
}
function openAuth(auth) {
  if (auth?.startsWith('fifo:')) {
    const name = auth.slice(5);
    if (!fs.statSync(name).isFIFO()) throw new Error('JOBSERVER_NOT_FIFO');
    const fd = fs.openSync(name, O_RDWR | O_NONBLOCK);
    return {read: fd, write: fd};
  }
  if (/^\d+,\d+$/.test(auth || '')) {
    const [r, w] = auth.split(',').map(Number);
    if (!fs.fstatSync(r).isFIFO() || !fs.fstatSync(w).isFIFO()) throw new Error('JOBSERVER_NOT_PIPE');
    // Reopen on Linux to obtain separate open-file descriptions. dup/fcntl on
    // the inherited descriptors would change make's own blocking flags.
    const read = fs.openSync(`/proc/self/fd/${r}`, O_RDONLY | O_NONBLOCK);
    try { return {read, write: fs.openSync(`/proc/self/fd/${w}`, O_WRONLY | O_NONBLOCK)}; }
    catch (error) { fs.closeSync(read); throw error; }
  }
  throw new Error('JOBSERVER_INVALID_AUTH');
}
function closeAuth(fds) {
  for (const fd of new Set([fds.read, fds.write])) fs.closeSync(fd);
}
function take(fds) {
  const byte = Buffer.alloc(1);
  for (;;) {
    try {
      const n = fs.readSync(fds.read, byte, 0, 1, null);
      if (n === 0) throw new Error('JOBSERVER_EOF');
      return byte;
    } catch (error) {
      if (error.code === 'EINTR') continue;
      if (error.code === 'EAGAIN' || error.code === 'EWOULDBLOCK') return undefined;
      throw error;
    }
  }
}
function give(fds, bytes) {
  for (let offset = 0; offset < bytes.length;) {
    try { offset += fs.writeSync(fds.write, bytes, offset, bytes.length - offset); }
    catch (error) { if (error.code !== 'EINTR') throw error; }
  }
}
function flagsFor(auth, jobs, env = process.env) {
  const result = {...env, CJPM_JOBSERVER: '1'};
  for (const name of flagNames) {
    const old = (env[name] || '')
      .replace(/(?:^|\s)--jobserver-(?:auth|fds)=[^\s]+/g, '')
      .replace(/(?:^|\s)-j(?:\s*\d+)?(?=\s|$)/g, '');
    result[name] = `${old.trim()} --jobserver-auth=${auth} -j${jobs}`.trim();
  }
  return result;
}
function resultOf(child) {
  return new Promise(resolve => {
    child.once('error', error => resolve({rc: 127, error: error.message}));
    child.once('exit', (code, signal) => resolve({rc: code ?? 128 + (os.constants.signals[signal] || 0), signal}));
  });
}
function signalGroup(group, signal) {
  if (!group) return;
  try { process.kill(-group, signal); }
  catch (error) { if (error.code !== 'ESRCH') throw error; }
}
function argumentsAfterSeparator(args) {
  const index = args.indexOf('--');
  if (index < 0 || !args[index + 1]) throw new Error('expected -- command [args]');
  return {options: args.slice(0, index), command: args.slice(index + 1)};
}
async function run(args) {
  const {options,command}=argumentsAfterSeparator(args);
  if(options.length!==2||options[0]!=='--jobs'||!/^[1-9]\d*$/.test(options[1]))throw new Error('run requires --jobs N -- command');
  const result=await supervisedCommand([process.execPath,ownPath,'worker','--',...command],
    {mode:'ninja',jobs:Number(options[1])});
  if(result.error)console.error(result.error);
  return result.rc;
}
async function worker(args) {
  if(Number(process.env.CJCJ_SUPERVISOR_PARENT)!==process.ppid)throw new Error('JOBSERVER_WORKER_PARENT');
  const {options,command}=argumentsAfterSeparator(args);
  if(options.length)throw new Error('unexpected worker options');
  const child=spawn(command[0],command.slice(1),{stdio:'inherit',env:process.env});
  const result=await resultOf(child);
  return result.rc;
}
async function command(args) {
  const parsed = argumentsAfterSeparator(args);
  if (parsed.options.length) throw new Error('unexpected command options');
  const directory = process.env.CANGJIE_JOBSERVER_ROOT;
  const jobs = process.env.CANGJIE_JOBSERVER_JOBS;
  if (!directory || !/^[1-9]\d*$/.test(jobs || '')) {
    throw new Error('JOBSERVER_MISSING_BUILD_ENTRY: invoke build.py to run this Ninja graph');
  }
  const fds = openAuth(authFromEnvironment());
  let token, child, success = false;
  try {
    while (!(token = take(fds))) await delay(2);
    // Passing our nonblocking open-file descriptions avoids select/read races
    // in the existing C2 consumer; it never has to reopen the FIFO in blocking mode.
    const env = flagsFor('3,4', jobs);
    trace('command-start', {directory, command: parsed.command});
    child = spawn(parsed.command[0], parsed.command.slice(1), {
      env, stdio: ['inherit', 'pipe', 'pipe', fds.read, fds.write],
    });
    const captured = {stdout: Buffer.alloc(0), stderr: Buffer.alloc(0), truncated: false};
    for (const name of ['stdout', 'stderr']) child[name].on('data', chunk => {
      process[name].write(chunk);
      const room = 262144 - captured[name].length;
      if (chunk.length > room) captured.truncated = true;
      if (room > 0) captured[name] = Buffer.concat([captured[name], chunk.subarray(0, room)]);
    });
    const closed = new Promise(resolve => child.once('close', resolve));
    const result = await resultOf(child);
    trace('command-end', {directory, child: child.pid, ...result});
    success = result.rc === 0;
    if (!success) {
      await Promise.race([closed, delay(100)]);
      const failure = {...result, stdout: captured.stdout.toString(), stderr: captured.stderr.toString(), truncated: captured.truncated};
      try { fs.writeFileSync(path.join(directory, 'failure'), JSON.stringify(failure), {flag: 'wx'}); }
      catch (error) { if (error.code !== 'EEXIST') throw error; }
      // Abort the whole owned graph before returning any admission token.
      // The supervisor outside this group drains it, then returns upstream loans.
      const group = Number(fs.readFileSync(path.join(directory, 'group'), 'utf8'));
      signalGroup(group, 'SIGTERM');
    }
    return result.rc;
  } finally {
    if (token && success) give(fds, token);
    closeAuth(fds);
  }
}

if (process.argv[1] && fs.realpathSync(process.argv[1]) === ownPath) {
  try {
    if (process.platform !== 'linux') throw new Error('shared Ninja jobserver requires Linux');
    const [mode, ...args] = process.argv.slice(2);
    process.exitCode = await (mode === 'run' ? run(args) : mode === 'command' ? command(args) : mode === 'worker' ? worker(args) : Promise.reject(new Error('expected run or command')));
  } catch (error) {
    console.error(`JOBSERVER_ERROR: ${error.message}`);
    process.exitCode = 1;
  }
}
