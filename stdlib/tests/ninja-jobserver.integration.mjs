#!/usr/bin/env node
// Finite Linux entry qualification. Uses the unmodified build.py entry and the
// actual AddCangjieSource command producer with small real packages; not a full std build.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {setTimeout as delay} from 'node:timers/promises';

const source = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const adapter = path.join(source, 'scripts/ninja-jobserver.mjs');
const [mode, arm = 'candidate'] = process.argv.slice(2);
const buildType = process.env.JOBSERVER_TEST_BUILD_TYPE || 'release';
assert(['release', 'relwithdebinfo'].includes(buildType));
const root = path.resolve(process.env.JOBSERVER_TEST_ROOT || 'jobserver-test-results');
fs.mkdirSync(root, {recursive: true});
const records = [];
function run(name, command, options = {}) {
  const out = path.join(root, name + '.stdout'), err = path.join(root, name + '.stderr');
  const a = fs.openSync(out, 'w'), b = fs.openSync(err, 'w');
  const started = Date.now();
  const r = spawnSync(command[0], command.slice(1), {
    cwd: root, env: {...process.env, ...options.env}, stdio: ['ignore', a, b], timeout: 600000, ...options,
  });
  fs.closeSync(a); fs.closeSync(b);
  const record = {name, command, rc: r.status, signal: r.signal, error: r.error?.message, wall_ms: Date.now() - started};
  records.push(record);
  fs.writeFileSync(path.join(root, `${mode}-${arm}-commands.json`), JSON.stringify(records, null, 2));
  console.log(JSON.stringify(record));
  return {...record, stdout: fs.readFileSync(out, 'utf8'), stderr: fs.readFileSync(err, 'utf8')};
}
const readEvents = file => fs.readFileSync(file, 'utf8').trim().split('\n').filter(Boolean).map(x => JSON.parse(x));
function cleanFlags(env) {
  const result = {...env};
  for (const k of ['MAKEFLAGS', 'MFLAGS', 'CARGO_MAKEFLAGS', 'CJPM_JOBSERVER', 'CANGJIE_JOBSERVER_ROOT', 'CANGJIE_JOBSERVER_JOBS']) delete result[k];
  return result;
}
async function protocol() {
  assert.equal(run('syntax', ['node', '--check', adapter]).rc, 0);
  assert.equal(run('existing-configure-tests', ['python3', path.join(source, 'tests/test_configure_required.py')]).rc, 0);
  const consumer = path.join(root, 'protocol-consumer.mjs');
  fs.writeFileSync(consumer, `import fs from 'node:fs';
const m=process.env.MAKEFLAGS.match(/--jobserver-auth=(\\d+),(\\d+)/); if(!m)throw Error('auth missing');
const r=+m[1],w=+m[2],b=Buffer.alloc(1);let extra=0;
for(;;){try{if(fs.readSync(r,b,0,1,null)!==1)throw Error('EOF');extra++;}catch(e){if(e.code==='EAGAIN')break;throw e;}}
console.log(JSON.stringify({implicit:1,extra}));
for(let i=0;i<extra;i++)fs.writeSync(w,Buffer.from('+'));
`);
  for (const jobs of [1, 4, 16]) {
    const log = path.join(root, `protocol-${jobs}.jsonl`);
    const r = run(`protocol-${jobs}`, ['node', adapter, 'run', '--jobs', String(jobs), '--',
      'node', adapter, 'command', '--', 'node', consumer],
    {env: {...cleanFlags(process.env), CANGJIE_JOBSERVER_LOG: log}});
    assert.equal(r.rc, 0); assert.deepEqual(JSON.parse(r.stdout), {implicit: 1, extra: jobs - 1});
    const end = readEvents(log).find(x => x.event === 'pool-end');
    assert.equal(end.returned, jobs); assert.equal(end.missing, 0);
  }
  const nestedLog = path.join(root, 'nested.jsonl');
  const nested = run('nested', ['node', adapter, 'run', '--jobs', '4', '--',
    'node', adapter, 'command', '--', 'node', adapter, 'run', '--jobs', '2', '--',
    'node', adapter, 'command', '--', 'node', consumer],
  {env: {...cleanFlags(process.env), CANGJIE_JOBSERVER_LOG: nestedLog}});
  assert.equal(nested.rc, 0); assert.equal(JSON.parse(nested.stdout).extra, 1);
  assert.deepEqual(readEvents(nestedLog).filter(x => x.event === 'pool-end').map(x => x.missing), [0, 0]);

  const makefile = path.join(root, 'Makefile');
  fs.writeFileSync(makefile, `all:\n\t+node ${adapter} run --jobs 4 -- node ${adapter} command -- node ${consumer}\n`);
  const makeLog = path.join(root, 'make.jsonl');
  const make = run('make-import', ['make', '-s', '-f', makefile, '-j4'],
    {env: {...cleanFlags(process.env), CANGJIE_JOBSERVER_LOG: makeLog}});
  assert.equal(make.rc, 0); assert.equal(JSON.parse(make.stdout).extra, 3);
  assert.equal(readEvents(makeLog).find(x => x.event === 'parent-return').count, 3);

  const fifo = path.join(root, 'parent.fifo');
  assert.equal(run('mkfifo-parent', ['mkfifo', '-m', '600', fifo]).rc, 0);
  const fd = fs.openSync(fifo, fs.constants.O_RDWR | fs.constants.O_NONBLOCK);
  fs.writeSync(fd, Buffer.from('abc'));
  const fifoLog = path.join(root, 'fifo.jsonl');
  const fifoRun = run('fifo-import-local-cap', ['node', adapter, 'run', '--jobs', '2', '--',
    'node', adapter, 'command', '--', 'node', consumer],
  {env: {...cleanFlags(process.env), MAKEFLAGS: `--jobserver-auth=fifo:${fifo} -j4`, CANGJIE_JOBSERVER_LOG: fifoLog}});
  assert.equal(fifoRun.rc, 0); assert.equal(JSON.parse(fifoRun.stdout).extra, 1);
  const returned = Buffer.alloc(3); assert.equal(fs.readSync(fd, returned, 0, 3, null), 3);
  assert.deepEqual([...returned].sort(), [...Buffer.from('abc')].sort());
  fs.closeSync(fd); fs.unlinkSync(fifo);

  const bad = run('bad-auth', ['node', adapter, 'run', '--jobs', '4', '--', 'true'],
    {env: {...cleanFlags(process.env), MAKEFLAGS: '--jobserver-auth=999,998 -j4'}});
  assert.notEqual(bad.rc, 0); assert.match(bad.stderr, /JOBSERVER_ERROR/);
  const failedLog = path.join(root, 'failed.jsonl');
  const failed = run('command-failure', ['node', adapter, 'run', '--jobs', '4', '--',
    'node', adapter, 'command', '--', 'node', '-e', 'console.error("FIRST_ERROR_SENTINEL");process.exit(7)'],
  {env: {...cleanFlags(process.env), CANGJIE_JOBSERVER_LOG: failedLog}});
  assert.match(failed.stderr, /FIRST_ERROR_SENTINEL/);
  assert.equal(failed.rc, 7); assert.equal(readEvents(failedLog).find(x => x.event === 'command-end').rc, 7);

  const cancelLog = path.join(root, 'cancel.jsonl');
  const child = spawn('node', [adapter, 'run', '--jobs', '4', '--', 'node', adapter, 'command', '--',
    'node', '-e', 'setInterval(()=>{},1000)'], {cwd: root,
    env: {...cleanFlags(process.env), CANGJIE_JOBSERVER_LOG: cancelLog}, stdio: 'ignore'});
  const result = new Promise(resolve => child.once('exit', (code, signal) => resolve({code, signal})));
  for (let n = 0; n < 200; n++) {
    if (fs.existsSync(cancelLog) && readEvents(cancelLog).some(x => x.event === 'command-start')) break;
    await delay(10);
  }
  assert(readEvents(cancelLog).some(x => x.event === 'command-start'));
  child.kill('SIGTERM'); assert.equal((await result).code, 143);
  const started = readEvents(cancelLog).find(x => x.event === 'pool-start');
  // The stable guardian retains its final diagnostic record, but no live pool.
  const final = JSON.parse(fs.readFileSync(path.join(started.directory, 'status.json')));
  assert.equal(final.type, 'finished'); assert.equal(final.reaped, true);
  assert.equal(final.ledgerReturned, true);
  assert(!fs.existsSync(path.join(started.directory, 'fifo')));
  console.log('PROTOCOL_ASSERTIONS_REACHED PASS');
}
async function real({checkCache = true} = {}) {
  const sdk = process.env.JOBSERVER_TEST_SDK;
  assert(sdk, 'JOBSERVER_TEST_SDK is required');
  const fixture = path.join(root, arm, 'stdlib');
  fs.mkdirSync(fixture, {recursive: true});
  // Verbatim entry, with a finite CMake package graph. Both root and command
  // adapters are the actual product file, including during the controlled cut.
  fs.copyFileSync(path.join(source, 'build.py'), path.join(fixture, 'build.py'));
  fs.cpSync(path.join(source, 'cmake'), path.join(fixture, 'cmake'), {recursive: true});
  if (!fs.existsSync(path.join(fixture, 'scripts'))) fs.symlinkSync(path.join(source, 'scripts'), path.join(fixture, 'scripts'), 'dir');
  for (const name of ['budget_a', 'budget_b']) {
    const dir = path.join(fixture, name); if (fs.existsSync(dir)) continue; fs.mkdirSync(dir);
    for (let i = 0; i < 16; i++) fs.writeFileSync(path.join(dir, `part${i}.cj`),
      `package ${name}\npublic func value${i}(): Int64 { return ${i + 1} }\n`);
  }
  if (!fs.existsSync(path.join(fixture, 'main.cj'))) fs.writeFileSync(path.join(fixture, 'main.cj'),
    'import budget_a.*\nmain(): Int64 { let n = value15(); println(n); return if (n == 16) { 0 } else { 1 } }\n');
  fs.writeFileSync(path.join(fixture, 'CMakeLists.txt'), `cmake_minimum_required(VERSION 3.16.5)
project(jobserver_entry LANGUAGES C CXX)
include("${source}/cmake/modules/AddCangjieSource.cmake")
set(CANGJIE_CODEGEN_CJNATIVE_BACKEND ON)
set(TARGET_TRIPLE_DIRECTORY_PREFIX linux_x86_64)
set(TRIPLE x86_64-linux-gnu)
foreach(name budget_a budget_b)
  add_cangjie_library(\${name} IS_PACKAGE IS_CJNATIVE_BACKEND NO_SUB_PKG NO_INSTALL
    PACKAGE_NAME \${name} SOURCE_DIR "\${CMAKE_CURRENT_SOURCE_DIR}/\${name}")
endforeach()
add_custom_command(OUTPUT "\${CMAKE_BINARY_DIR}/probe"
  COMMAND \${CANGJIE_BUDGET_LAUNCHER} cjc "\${CMAKE_CURRENT_SOURCE_DIR}/main.cj"
    --import-path "\${CMAKE_BINARY_DIR}" -L "\${CMAKE_BINARY_DIR}" -l:budget_a.a
    -j\${CANGJIE_CPU_JOBS} -o "\${CMAKE_BINARY_DIR}/probe"
  DEPENDS budget_a budget_b budget_a_bc budget_b_bc "\${CMAKE_CURRENT_SOURCE_DIR}/main.cj"
  \${CANGJIE_PACKAGE_POOL} VERBATIM)
add_custom_target(probe_app ALL DEPENDS "\${CMAKE_BINARY_DIR}/probe")
install(PROGRAMS "\${CMAKE_BINARY_DIR}/probe" DESTINATION bin)
`);
  const log = path.join(root, `${arm}.jsonl`);
  const env = {...cleanFlags(process.env), PATH: `${sdk}/bin:${sdk}/tools/bin:${process.env.PATH}`,
    CANGJIE_HOME: sdk, CJCJ_JOBSERVER_TRACE: '1', CANGJIE_JOBSERVER_LOG: log,
    LD_LIBRARY_PATH: `${sdk}/runtime/lib/linux_x86_64_cjnative:${sdk}/lib/linux_x86_64_cjnative`,
  };
  // Keep the backend safety gate exactly at its default.
  delete env.CJCJ_BACKEND_CONCURRENCY;
  const jobs = arm === 'single' ? '1' : (arm === 'wide' || arm === 'default') ? '16' : '4';
  const jobsArgs = arm === 'default' ? [] : ['--jobs', jobs];
  const recovered = process.env.JOBSERVER_TEST_RESUME === '1';
  const observer = path.join(root, 'jobserver-available');
  if (!fs.existsSync(observer)) assert.equal(run('build-observer', ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
    path.join(source, 'tests/jobserver-available.c'), '-o', observer]).rc, 0);
  const sampleFile = path.join(root, `${arm}-available.csv`);
  const sampleFd = fs.openSync(sampleFile, 'w');
  const sampler = spawn(observer, [path.join(fixture, 'build/build')], {stdio: ['ignore', sampleFd, 'inherit']});
  const samplerResult = new Promise(resolve => sampler.once('exit', (code, signal) => resolve({code, signal})));
  const r = run(`${arm}-${recovered ? 'recovered-' : ''}build`, ['python3', path.join(fixture, 'build.py'), 'build', '-t', buildType,
    ...jobsArgs, '--package-jobs', '2'], {env});
  if (r.rc !== 0) sampler.kill('SIGTERM');
  const sampled = await Promise.race([samplerResult, delay(5000).then(async () => {sampler.kill('SIGTERM'); return await samplerResult;})]);
  fs.closeSync(sampleFd);
  fs.writeFileSync(path.join(root, `${arm}-observer-result.json`), JSON.stringify(sampled));
  if (r.rc === 0) assert.equal(sampled.code, 0);
  assert.equal(r.rc, 0, 'real compiler/entry must succeed before budget assertions');
  const prefix = path.join(root, arm, 'install');
  assert.equal(run(`${arm}-install`, ['python3', path.join(fixture, 'build.py'), 'install', '--prefix', prefix], {env}).rc, 0);
  const executable = run(`${arm}-execute`, [path.join(prefix, 'bin/probe')], {env});
  assert.equal(executable.rc, 0); assert.equal(executable.stdout.trim(), '16');
  console.log('SEMANTIC_CONTROL_REACHED PASS');
  const build = path.join(fixture, 'build/build');
  for (const name of ['budget_a.a', 'budget_b.a', 'libbudget_a.bc', 'libbudget_b.bc']) assert(fs.statSync(path.join(build, name)).size > 0);
  const events = readEvents(log);
  const end = events.filter(x => x.event === 'pool-end').at(-1);
  assert.equal(end.jobs, +jobs); assert.equal(end.returned, +jobs);
  const prior = recovered ? fs.readFileSync(path.join(root, `${arm}-build.stdout`), 'utf8') : '';
  const peaks = [...(r.stderr + r.stdout + prior).matchAll(/CJCJ_JOBSERVER acquired=(\d+) released=(\d+) active=(\d+) peak=(\d+) held_tokens=(\d+)/g)]
    .map(m => m.slice(1).map(Number));
  console.log('BUDGET_CONSUMER_ASSERTION_REACHED ' + JSON.stringify({peaks}));
  assert(peaks.length >= 5, 'ordinary, LTO and executable compiler consumers must all run');
  assert(peaks.every(([a, b, active, peak, held]) => a > 0 && a === b && active === 0 && peak >= 1 && held === 0));
  if (jobs === '1') assert(peaks.every(x => x[3] === 1));
  else assert(peaks.some(x => x[3] > 1), 'actual cjc must admit concurrent work');
  const starts = events.filter(x => x.event === 'command-start' && x.command[0] === 'cjc');
  if (buildType === 'relwithdebinfo') {
    const packages = starts.filter(x => x.command.includes('-p'));
    assert.equal(packages.length, 4, 'ordinary and LTO package compilations execute');
    assert(packages.every(x => x.command.includes('--apc=1')), 'mainline APC1 remains in actual compiler argv');
  }
  const ends = events.filter(x => x.event === 'command-end');
  const intervals = starts.map(s => ({pid: s.pid, start: BigInt(s.time_ns),
    end: BigInt(ends.find(e => e.pid === s.pid).time_ns)}));
  const overlap = intervals.some((a, i) => intervals.slice(i + 1).some(b => a.start < b.end && b.start < a.end));
  assert.equal(overlap, jobs !== '1');
  const samples = fs.readFileSync(sampleFile, 'utf8').trim().split('\n').filter(Boolean).map(line => {
    const [time, available] = line.split(','); return {time: BigInt(time), available: Number(available)};
  }).filter(s => intervals.some(i => i.start <= s.time && s.time <= i.end));
  assert(samples.length > 0, 'non-consuming FIFO observation must overlap actual compiler execution');
  assert(samples.every(s => s.available >= 0 && s.available <= +jobs));
  const peak = +jobs - Math.min(...samples.map(s => s.available));
  fs.writeFileSync(path.join(root, `${arm}-token-peak.json`), JSON.stringify({jobs: +jobs, peak, samples: samples.length}));
  console.log('GLOBAL_TOKEN_PEAK ' + JSON.stringify({jobs: +jobs, peak, samples: samples.length}));
  console.log('REAL_ENTRY_ASSERTIONS_REACHED PASS');
  if (!checkCache) return; // Independently selectable budget gate; cache gate remains required.
  checkCacheResume({fixture, env, log, jobs});
}
function checkCacheResume({fixture, env, log, jobs}) {
  const events = readEvents(log);
  const compilerStarts = rows => rows.filter(x => x.event === 'command-start' && x.command[0] === 'cjc');
  const before = compilerStarts(events).length;
  assert(before >= 5, 'cache classifier must observe the preceding real compiler invocations');
  const resumed = run(`${arm}-resume`, ['python3', path.join(fixture, 'build.py'), 'build', '-t', buildType, '--jobs', jobs], {env});
  assert.equal(resumed.rc, 0);
  const after = readEvents(log);
  fs.writeFileSync(path.join(root, `${arm}-cache-classification.json`), JSON.stringify({
    beforeCompiler: before, afterCompiler: compilerStarts(after).length,
    resumedCommands: after.slice(events.length).filter(x => x.event === 'command-start').map(x => x.command),
  }, null, 2));
  assert.equal(compilerStarts(after).length, before, 'cache hit must not rerun compiler');
  console.log('CACHE_COMPILER_CLASSIFIER_AND_RESUME_PASS');
}
async function entryErrors({checkError = true} = {}) {
  const fixture = process.env.JOBSERVER_TEST_EXISTING_FIXTURE;
  const sdk = process.env.JOBSERVER_TEST_SDK;
  assert(fixture && sdk, 'existing qualified fixture and fixed SDK required');
  const main = path.join(fixture, 'main.cj');
  const original = fs.readFileSync(main);
  const baseEnv = {...cleanFlags(process.env), PATH: `${sdk}/bin:${sdk}/tools/bin:${process.env.PATH}`,
    CANGJIE_HOME: sdk, CJCJ_JOBSERVER_TRACE: '1',
    LD_LIBRARY_PATH: `${sdk}/runtime/lib/linux_x86_64_cjnative:${sdk}/lib/linux_x86_64_cjnative`};
  delete baseEnv.CJCJ_BACKEND_CONCURRENCY;
  const build = ['python3', path.join(fixture, 'build.py'), 'build', '-t', buildType, '--jobs', '4'];
  function parent(name) {
    const fifo = path.join(root, `${name}.fifo`);
    assert.equal(run(`${name}-mkfifo`, ['mkfifo', '-m', '600', fifo]).rc, 0);
    const fd = fs.openSync(fifo, fs.constants.O_RDWR | fs.constants.O_NONBLOCK);
    fs.writeSync(fd, Buffer.from('abc'));
    const log = path.join(root, `${name}.jsonl`);
    return {fd, fifo, log, env: {...baseEnv, MAKEFLAGS: `--jobserver-auth=fifo:${fifo} -j4`, CANGJIE_JOBSERVER_LOG: log}};
  }
  function returned(p) {
    const bytes = Buffer.alloc(3);
    assert.equal(fs.readSync(p.fd, bytes, 0, 3, null), 3);
    assert.deepEqual([...bytes].sort(), [...Buffer.from('abc')].sort());
    assert.equal(readEvents(p.log).find(x => x.event === 'parent-return').count, 3);
    const pool = readEvents(p.log).find(x => x.event === 'pool-start');
    assert(!fs.existsSync(pool.directory), 'private pool must be removed after descendants stop');
  }
  if (checkError) {
  const errorParent = parent('entry-error');
  try {
    fs.writeFileSync(main, 'main(): Int64 { return JOBSERVER_EXPECTED_UNDEFINED_NAME }\n');
    const r = run('entry-error-build', build, {env: errorParent.env});
    assert.equal(r.rc, 1);
    assert.match(r.stdout + r.stderr, /JOBSERVER_EXPECTED_UNDEFINED_NAME/);
    assert(readEvents(errorParent.log).some(x => x.event === 'command-end' && x.rc === 1));
    returned(errorParent);
    console.log('ENTRY_ERROR_ASSERTIONS_REACHED PASS');
  } finally {
    fs.writeFileSync(main, original);
    fs.closeSync(errorParent.fd); fs.unlinkSync(errorParent.fifo);
  }
  }
  const cancelParent = parent('entry-cancel');
  const out = fs.openSync(path.join(root, 'entry-cancel.stdout'), 'w');
  const err = fs.openSync(path.join(root, 'entry-cancel.stderr'), 'w');
  const started = Date.now();
  const child = spawn(build[0], build.slice(1), {env: cancelParent.env, cwd: root, stdio: ['ignore', out, err]});
  const exited = new Promise(resolve => child.once('exit', (rc, signal) => resolve({rc, signal})));
  let observed, result;
  try {
    const metadata = JSON.parse(fs.readFileSync(path.join(sdk, 'STAGE-INPUTS.json'), 'utf8'));
    const expected = fs.realpathSync(path.join(sdk, metadata.compilerFile));
    for (let i = 0; i < 1500 && !observed; i++) {
      if (fs.existsSync(cancelParent.log)) {
        for (const event of readEvents(cancelParent.log).filter(x => x.event === 'command-start' && x.command[0] === 'cjc')) {
          try {
            const children = fs.readFileSync(`/proc/${event.pid}/task/${event.pid}/children`, 'utf8').trim().split(' ');
            for (const pid of children) if (pid && fs.readlinkSync(`/proc/${pid}/exe`) === expected) observed = {pid: +pid, exe: expected};
          } catch (error) { if (error.code !== 'ENOENT' && error.code !== 'ESRCH') throw error; }
        }
      }
      if (!observed) await delay(10);
    }
    assert(observed, 'actual installed cjc ELF must be running before cancellation');
    const pool = readEvents(cancelParent.log).find(x => x.event === 'pool-start');
    const group = Number(fs.readFileSync(path.join(pool.directory, 'group'), 'utf8'));
    child.kill('SIGTERM'); result = await exited;
    fs.writeFileSync(path.join(root, 'entry-cancel-result.json'), JSON.stringify({command: build, ...result, observed, group, wall_ms: Date.now() - started}));
    assert.equal(result.rc, 143); returned(cancelParent);
    const live = [];
    for (const entry of fs.readdirSync('/proc').filter(x => /^\d+$/.test(x))) {
      try {
        const text = fs.readFileSync(`/proc/${entry}/stat`, 'utf8');
        const fields = text.slice(text.lastIndexOf(')') + 2).split(' ');
        if (+fields[2] === group && fields[0] !== 'Z') live.push(+entry);
      } catch (error) { if (error.code !== 'ENOENT' && error.code !== 'ESRCH') throw error; }
    }
    assert.deepEqual(live, []);
    console.log('ENTRY_CANCEL_ASSERTIONS_REACHED PASS');
  } finally {
    if (!result) {
      child.kill('SIGTERM');
      const final = await exited;
      fs.writeFileSync(path.join(root, 'entry-cancel-cleanup.json'), JSON.stringify({command: build, ...final, observed: observed || null}));
    }
    fs.closeSync(out); fs.closeSync(err); fs.closeSync(cancelParent.fd); fs.unlinkSync(cancelParent.fifo);
    fs.writeFileSync(main, original);
  }
}
try {
  if (mode === 'protocol') await protocol();
  else if (mode === 'real') await real();
  else if (mode === 'real-only') await real({checkCache: false});
  else if (mode === 'cache-existing') {
    const fixture = process.env.JOBSERVER_TEST_EXISTING_FIXTURE;
    const previous = process.env.JOBSERVER_TEST_PREVIOUS_LOG;
    const sdk = process.env.JOBSERVER_TEST_SDK;
    assert(fixture && previous && sdk);
    const log = path.join(root, 'cache-existing.jsonl');
    fs.copyFileSync(previous, log);
    const env = {...cleanFlags(process.env), PATH: `${sdk}/bin:${sdk}/tools/bin:${process.env.PATH}`,
      CANGJIE_HOME: sdk, CANGJIE_JOBSERVER_LOG: log,
      LD_LIBRARY_PATH: `${sdk}/runtime/lib/linux_x86_64_cjnative:${sdk}/lib/linux_x86_64_cjnative`};
    checkCacheResume({fixture, env, log, jobs: '4'});
  }
  else if (mode === 'entry-errors') await entryErrors();
  else if (mode === 'entry-cancel-only') await entryErrors({checkError: false});
  else throw new Error('expected protocol or real');
} catch (error) { console.error(error.stack); process.exitCode = 1; }
