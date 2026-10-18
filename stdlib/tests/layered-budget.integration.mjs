#!/usr/bin/env zx
// Finite production build.py -> CMake -> Ninja budget wiring. The cjc stand-in
// observes invocation/heap/concurrency only; it grants no language/ABI result.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

const source = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const root = process.argv[2];
assert(root && path.isAbsolute(root) && !fs.existsSync(root), 'fresh absolute evidence directory');
fs.mkdirSync(root, {recursive: true});
const fixture = path.join(root, 'stdlib'), bin = path.join(root, 'bin');
fs.mkdirSync(fixture); fs.mkdirSync(bin);
fs.copyFileSync(path.join(source, 'build.py'), path.join(fixture, 'build.py'));
for (const name of ['cmake', 'scripts']) fs.cpSync(path.join(source, name), path.join(fixture, name), {recursive: true});
const moduleFile = path.join(fixture, 'cmake/modules/AddCangjieSource.cmake');
const original = fs.readFileSync(moduleFile);
const eventsFile = path.join(root, 'compiler-events.jsonl');
const records = [];
const env = {...process.env, PATH: bin + ':' + process.env.PATH, CANGJIE_HOME: root + '/host',
  cjHeapSize: '7GB', BUDGET_EVENTS: eventsFile, EXPECTED_HEAP: '2GB',
  CANGJIE_JOBSERVER_LOG: root + '/jobserver.jsonl', CJCJ_SUPERVISOR_WORK: root + '/supervision'};
for (const name of ['MAKEFLAGS', 'MFLAGS', 'CARGO_MAKEFLAGS', 'CANGJIE_JOBSERVER_ROOT',
  'CANGJIE_JOBSERVER_JOBS', 'CMAKE_TOOLCHAIN_FILE', 'CC', 'CXX', 'LD_PRELOAD', 'LD_AUDIT']) delete env[name];
const events = () => fs.existsSync(eventsFile)
  ? fs.readFileSync(eventsFile, 'utf8').trim().split('\n').filter(Boolean).map(JSON.parse) : [];
function run(name, argv, extra = {}) {
  const stdout = fs.openSync(root + '/' + name + '.stdout', 'w');
  const stderr = fs.openSync(root + '/' + name + '.stderr', 'w');
  const r = spawnSync(argv[0], argv.slice(1), {cwd: fixture, env: {...env, ...extra},
    stdio: ['ignore', stdout, stderr], timeout: 180000});
  fs.closeSync(stdout); fs.closeSync(stderr);
  const row = {name, argv, rc: r.status, signal: r.signal, error: r.error?.message ?? null};
  records.push(row); fs.writeFileSync(root + '/commands.json', JSON.stringify(records, null, 2));
  console.log(JSON.stringify(row));
  return {...row, stdout: fs.readFileSync(root + '/' + name + '.stdout', 'utf8'),
    stderr: fs.readFileSync(root + '/' + name + '.stderr', 'utf8')};
}
const target = name => path.join(root, name);
for (const name of ['target-a', 'target-b']) {
  const dir = path.join(target(name), 'common/linux_relwithdebinfo_x86_64/lib/linux_x86_64_cjnative');
  fs.mkdirSync(dir, {recursive: true});
  // Selection fixture: no aio sentinel. Nothing is linked against this marker.
  fs.writeFileSync(path.join(dir, 'libcangjie-runtime.a'), name);
}
fs.writeFileSync(bin + '/cjc', `#!/usr/bin/env node
import fs from 'node:fs';import path from 'node:path';import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
const args=process.argv.slice(2), out=args[args.indexOf('--output')+1];
const event=(kind)=>fs.appendFileSync(process.env.BUDGET_EVENTS,JSON.stringify({kind,pid:process.pid,time:process.hrtime.bigint().toString(),heap:process.env.cjHeapSize,args})+'\\n');
console.log('COMPILER_HEAP_ASSERTION_REACHED '+process.env.cjHeapSize);
assert.equal(process.env.cjHeapSize,process.env.EXPECTED_HEAP,'SELECTED_HEAP_MUST_REACH_COMPILER');
assert(args.includes('--apc=1'),'APC1_MUST_REACH_STATIC_AND_BC');
event('start');await new Promise(r=>setTimeout(r,250));
fs.mkdirSync(path.dirname(out),{recursive:true});
if(out.endsWith('.a')){
 const c=out+'.c',o=out+'.o';fs.writeFileSync(c,'int budget_fixture(void){return 42;}');
 for(const argv of [['cc','-c',c,'-o',o],['ar','rcs',out,o]]){const r=spawnSync(argv[0],argv.slice(1),{stdio:'inherit'});if(r.status!==0)process.exit(r.status??1);}
}else fs.writeFileSync(out,'finite BC command observation only');
event('end');
`);
fs.chmodSync(bin + '/cjc', 0o755);
fs.writeFileSync(fixture + '/native.c', '#include <stdio.h>\nint main(void){puts("NATIVE_CONTROL_42");return 0;}\n');
for (let i = 0; i < 4; ++i) {
  fs.mkdirSync(fixture + '/p' + i);
  fs.writeFileSync(fixture + '/p' + i + '/input.cj', 'package p' + i + '\n');
}
fs.writeFileSync(fixture + '/CMakeLists.txt', `cmake_minimum_required(VERSION 3.16.5)
project(layered_budget LANGUAGES C CXX)
list(APPEND CMAKE_MODULE_PATH "\${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
set(dir1 linux_relwithdebinfo_x86_64)
set(dir2 linux_x86_64_cjnative)
include(SelectRuntime)
file(WRITE "\${CMAKE_BINARY_DIR}/selected-runtime.txt" "\${RUNTIME_COMMON_LIB_DIR}")
include(AddCangjieSource)
set(CANGJIE_CODEGEN_CJNATIVE_BACKEND ON)
set(TARGET_TRIPLE_DIRECTORY_PREFIX linux_x86_64)
set(TRIPLE x86_64-linux-gnu)
foreach(i RANGE 0 3)
  add_cangjie_library(p\${i} IS_PACKAGE IS_CJNATIVE_BACKEND NO_SUB_PKG NO_INSTALL
    PACKAGE_NAME p\${i} SOURCE_DIR "\${CMAKE_CURRENT_SOURCE_DIR}/p\${i}")
endforeach()
add_executable(native-control native.c)
install(TARGETS native-control DESTINATION bin)
`);
const build = path.join(fixture, 'build/build');
const command = ({packages = 2, heap = 2, total = 8, reserve = 4, lib = 'target-a'} = {}) =>
  ['python3', '-I', fixture + '/build.py', 'build', '-t', 'relwithdebinfo', '--jobs', '8',
    '--package-jobs', String(packages), '--memory-budget-gib', String(total),
    '--compiler-heap-gib', String(heap), '--native-reserve-gib', String(reserve), '--target-lib=' + target(lib)];
function checkIntervals(rows, packages, heap) {
  const timeline = rows.map(r => ({kind: r.kind, time: BigInt(r.time)})).sort((a,b) => a.time < b.time ? -1 : 1);
  let active = 0, peak = 0;
  for (const row of timeline) { active += row.kind === 'start' ? 1 : -1; peak = Math.max(peak, active); assert(active >= 0); }
  assert.equal(active, 0); assert.equal(peak, packages, 'PACKAGE_POOL_REACHED_REQUESTED_BOUND');
  assert.equal(rows.filter(r => r.kind === 'start').length, 8);
  assert(rows.every(r => r.heap === heap && r.args.includes('-j8')));
  console.log('LAYERED_BUDGET_ASSERTION_REACHED ' + JSON.stringify({peak, heap, starts: 8}));
  return {peak, heap, starts: 8};
}
const results = {};
try {
  const normal = run('candidate', command()); assert.equal(normal.rc, 0, normal.stderr);
  results.candidate = checkIntervals(events(), 2, '2GB');
  assert(fs.readFileSync(build + '/selected-runtime.txt', 'utf8').startsWith(target('target-a')));
  const graph = fs.readFileSync(build + '/build.ninja', 'utf8');
  assert.equal((graph.match(/pool = cangjie_packages/g) || []).length, 8);
  assert.match(graph, /cjHeapSize=2GB/);
  const control = run('native-control', [build + '/native-control']);
  assert.equal(control.rc, 0); assert.match(control.stdout, /NATIVE_CONTROL_42/);
  const count = events().length;
  const cached = run('same-input-reentry', command()); assert.equal(cached.rc, 0);
  assert.equal(events().length, count, 'unchanged input has no compiler execution');
  const invalid = run('overcommitted', command({packages: 3, heap: 3}));
  assert.notEqual(invalid.rc, 0); assert.match(invalid.stdout + invalid.stderr, /heaps plus native reserve exceed/);
  assert.equal(events().length, count);
  const changed = run('changed-inputs', command({packages: 1, heap: 3, lib: 'target-b'}), {EXPECTED_HEAP: '3GB'});
  assert.equal(changed.rc, 0, changed.stderr);
  results.changed = checkIntervals(events().slice(count), 1, '3GB');
  assert(fs.readFileSync(build + '/selected-runtime.txt', 'utf8').startsWith(target('target-b')));
  const beforeRestore = events().length;
  const reset = run('back-to-candidate', command()); assert.equal(reset.rc, 0);
  checkIntervals(events().slice(beforeRestore), 2, '2GB');
  const text = original.toString();
  const needle = 'set(CANGJIE_COMPILER_ENV "cjHeapSize=${CANGJIE_COMPILER_HEAP_GIB}GB")';
  assert.equal(text.split(needle).length, 2);
  fs.writeFileSync(root + '/cut.diff', '-    ' + needle + '\n+    set(CANGJIE_COMPILER_ENV)\n');
  fs.writeFileSync(moduleFile, text.replace(needle, 'set(CANGJIE_COMPILER_ENV)'));
  const cut = run('cut', command()); assert.notEqual(cut.rc, 0);
  assert.match(cut.stdout + cut.stderr, /COMPILER_HEAP_ASSERTION_REACHED 7GB/);
  assert.match(cut.stdout + cut.stderr, /SELECTED_HEAP_MUST_REACH_COMPILER/);
  const independent = run('cut-native-control', [build + '/native-control']); assert.equal(independent.rc, 0);
  fs.writeFileSync(moduleFile, original);
  const n = events().length;
  const restored = run('restored', command()); assert.equal(restored.rc, 0, restored.stderr);
  results.restored = checkIntervals(events().slice(n), 2, '2GB');
  assert(fs.readFileSync(moduleFile).equals(original));
  const install = run('install', ['python3', '-I', fixture + '/build.py', 'install', '--prefix', root + '/install']);
  assert.equal(install.rc, 0, install.stderr);
  const installed = run('installed-native-control', [root + '/install/bin/native-control']); assert.equal(installed.rc, 0);
  const budgetEvents = fs.readFileSync(root + '/jobserver.jsonl', 'utf8').trim().split('\n').map(JSON.parse);
  assert(budgetEvents.some(e => e.event === 'command-start' && e.command.some(a => a.endsWith('/native.c'))));
  assert(budgetEvents.filter(e => e.event === 'pool-end' && e.rc === 0).every(e => e.missing === 0));
  results.rc = {candidate: normal.rc, cut: cut.rc, independent: independent.rc, restored: restored.rc, install: install.rc};
  fs.writeFileSync(root + '/results.json', JSON.stringify(results, null, 2));
  console.log('FINITE_LAYERED_BUDGET_PASS; LANGUAGE_ABI_NOT_RUN');
} finally {
  fs.writeFileSync(moduleFile, original);
  fs.writeFileSync(root + '/restoration.json', JSON.stringify({restored: fs.readFileSync(moduleFile).equals(original)}));
}
