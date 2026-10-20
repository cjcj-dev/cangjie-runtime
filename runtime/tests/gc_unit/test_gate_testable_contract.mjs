#!/usr/bin/env zx
// Controlled harness inputs exercise the real gate, not runtime qualification.
import { mkdirSync, mkdtempSync, copyFileSync, writeFileSync, readFileSync, existsSync, chmodSync, renameSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import assert from 'node:assert/strict';
const here = dirname(fileURLToPath(import.meta.url));
const scratch = process.env.GC_UNIT_CONTRACT_EVIDENCE || process.env.TMPDIR;
if (!scratch) throw Error('set GC_UNIT_CONTRACT_EVIDENCE or TMPDIR');
mkdirSync(scratch, { recursive: true });
const root = mkdtempSync(join(scratch, 'gate-contract-')), runtime = join(root, 'runtime'), src = join(runtime, 'tests/gc_unit');
const lib = join(root, 'lib'), build = join(root, 'build'), headers = join(build, 'runtime-staging'), bin = join(root, 'bin'), sdk = join(root, 'sdk');
for (const directory of [src, join(runtime, 'build'), lib, join(headers, 'include'), bin, join(sdk, 'bin'), join(sdk, 'lib/linux_x86_64_cjnative')]) mkdirSync(directory, { recursive: true });
for (const name of ['gate_gc_unit.sh', 'gate_gc_unit.mjs', 'product_test_configuration.mjs', 'language_toolchain.mjs', 'testable_product_hooks.py']) copyFileSync(join(here, name), join(src, name));
copyFileSync(resolve(here, '../../build/resolve_runtime_headers.mjs'), join(runtime, 'build/resolve_runtime_headers.mjs'));
for (const name of ['Heap/z/zGlobals.cpp', 'Heap/z/zMarkStack.cpp', 'Heap/z/zStat.cpp', 'Mutator/ThreadSMR.cpp']) {
  mkdirSync(dirname(join(runtime, 'src', name)), { recursive: true });
  copyFileSync(resolve(here, '../../src', name), join(runtime, 'src', name));
}
const shell = (path, body) => { writeFileSync(path, '#!/usr/bin/env bash\nset -eu\n' + body + '\n'); chmodSync(path, 0o755); };
for (const file of ['libcangjie-runtime.so', 'libboundscheck.so']) writeFileSync(join(lib, file), 'controlled harness input\n');
writeFileSync(join(headers, 'include/schedule.h'), '// controlled header\n');
writeFileSync(join(sdk, 'lib/linux_x86_64_cjnative/libcangjie-std-core.a'), 'controlled std\n');
shell(join(sdk, 'bin/cjc'), 'exit 0');
for (const file of ['finalizer_trigger.cj', 'phase_entry_trigger.cj', 'phase_entry_major.cj', 'segmented_array_managed.cj', 'so_reentry_probe.cpp', 'test_defect_regressions.cpp']) writeFileSync(join(src, file), '// fixture\n');
for (const [name, token] of [['run_finalizer_trigger.sh', 'FINALIZER'], ['run_phase_entry_trigger.sh', 'PHASE'], ['run_segmented_array_managed.sh', 'SEGMENTED'], ['run_so_reentry_bounded.sh', 'SO']]) shell(join(src, name), `echo ${token} >>"$TRACE"\nexit "\u0024{${token}_RC:-0}"`);
shell(join(src, 'test_parallel_runner.sh'), 'exit 0');
writeFileSync(join(src, 'CMakeLists.txt'), 'test_x.cpp\n'); writeFileSync(join(src, 'known_failures.txt'), '');
const defaultRunner = '# test_x.cpp\necho CPP >>"$TRACE"\nprintf "[========] %s tests: %s passed, 0 failed\\n" "${TEST_COUNT:-1}" "${TEST_COUNT:-1}" >"$GC_UNIT_TALLY_FILE"\nexit "${SUITE_RC:-0}"';
shell(join(src, 'run_standalone.sh'), defaultRunner);
function recipe(testable = 0, ohos = 0) {
  writeFileSync(join(build, 'compile_commands.json'), JSON.stringify([{ file: '/runtime/src/Heap/z/zGeneration.cpp', arguments: ['clang++', ...(testable ? ['-DMRT_TESTABLE_INTERNALS=1'] : []), ...(ohos ? ['-DMRT_GC_UNIT_OHOS_HOST=1'] : [])] }]));
}
const probe = spawnSync('python3', [join(src, 'testable_product_hooks.py'), '--source', join(runtime, 'src')], { encoding: 'utf8' });
assert.equal(probe.status, 0, probe.stderr);
const hooks = [...probe.stdout.matchAll(/^TESTABLE_PRODUCT_HOOK_TARGET=(\S+)/gm)].map(m => m[1]);
function nm(full = true, host = ['CJ_GetUIThreadStackTop', 'CJ_PushUIThreadStackTop']) {
  const text = [...(full ? hooks.map(s => `00000 T MapleRuntime::${s}${s.includes('::') ? '()' : ''}`) : []), ...host.map(s => `00000 T ${s}`)].join('\n');
  shell(join(bin, 'nm'), `cat <<'SYMBOLS'\n${text}\nSYMBOLS`);
}
const env = { ...process.env, PATH: `${bin}:${process.env.PATH}`, GCV2_RUNTIME_LIB_DIR: lib, GCV2_RUNTIME_OUTPUT_ROOT: headers,
  GCV2_RUNTIME_BUILD_DIR: build, GC_UNIT_CJC_RUNTIME_LIB_DIR: lib, GC_UNIT_LANGUAGE_SDK: sdk, CJC: join(sdk, 'bin/cjc'),
  GC_UNIT_GATE_CONTRACT_SELFTEST: '1',
  MRT_TESTABLE_INTERNALS: '0', GC_UNIT_GATE_SKIP: '0', GC_UNIT_GATE_LANGUAGE_TESTS: 'defer', GC_UNIT_OHOS_HOST_TEST_ELF: '' };
let count = 0;
function check(name, extra, expected, reason, trace) {
  const directory = join(root, name); mkdirSync(directory);
  const statusPath = join(directory, 'status'), tracePath = join(directory, 'trace');
  const result = spawnSync('bash', [join(src, 'gate_gc_unit.sh')], { env: { ...env, GC_UNIT_OUT: directory, GC_UNIT_GATE_STATUS: statusPath, TRACE: tracePath, ...extra }, encoding: 'utf8' });
  writeFileSync(join(directory, 'entry.log'), result.stdout + result.stderr); writeFileSync(join(directory, 'entry.rc'), `${result.status}\n`);
  const status = Object.fromEntries(readFileSync(statusPath, 'utf8').trim().split('\n').map(s => { const at = s.indexOf('='); return [s.slice(0, at), s.slice(at + 1)]; }));
  assert.equal(result.status, expected, `${name}: ${result.stderr}`);
  assert.match(status.REASON, reason, name);
  if (trace !== undefined) assert.equal(existsSync(tracePath) ? readFileSync(tracePath, 'utf8').trim() : '', trace, name);
  count++; console.log(`GATE_CONTRACT_TARGET ${name} rc=${result.status} reason=${status.REASON}`);
  return status;
}
recipe(); nm();
check('skip', { GC_UNIT_GATE_SKIP: '1' }, 0, /EXPLICIT_SKIP/, '');
check('invalid-before-skip', { GC_UNIT_GATE_SKIP: '1', GC_UNIT_GATE_LANGUAGE_TESTS: 'invalid' }, 2, /must be all/, '');
check('defer', {}, 0, /LANGUAGE_DEFERRED/, 'CPP');
check('only', { GC_UNIT_GATE_LANGUAGE_TESTS: 'only' }, 0, /^PASS$/, 'FINALIZER\nPHASE\nSEGMENTED');
check('all', { GC_UNIT_GATE_LANGUAGE_TESTS: 'all' }, 0, /^PASS$/, 'CPP\nFINALIZER\nPHASE\nSEGMENTED');
// Same normal inputs still execute again; there is no cached language PASS.
check('all-again', { GC_UNIT_GATE_LANGUAGE_TESTS: 'all' }, 0, /^PASS$/, 'CPP\nFINALIZER\nPHASE\nSEGMENTED');
check('empty', { TEST_COUNT: '0' }, 3, /EMPTY/, 'CPP');
check('empty-process-failure', { TEST_COUNT: '0', SUITE_RC: '1' }, 1, /EXIT_FAILURE/, 'CPP');
check('completed-process-failure', { SUITE_RC: '7' }, 1, /EXIT_FAILURE/, 'CPP');
for (const mode of ['all', 'only']) {
  check(`host-missing-${mode}`, { GC_UNIT_GATE_LANGUAGE_TESTS: mode, GC_UNIT_CJC_RUNTIME_LIB_DIR: join(root, 'missing') }, 2, /ENOENT/);
  check(`compiler-missing-${mode}`, { GC_UNIT_GATE_LANGUAGE_TESTS: mode, GC_UNIT_LANGUAGE_SDK: '' }, 2, /GC_UNIT_LANGUAGE_SDK/);
}
check('language-exit', { GC_UNIT_GATE_LANGUAGE_TESTS: 'only', PHASE_RC: '7' }, 1, /PHASE_ENTRY_FAILURE rc=7/, 'FINALIZER\nPHASE');
recipe(1); nm(false);
renameSync(join(src, 'segmented_array_managed.cj'), join(root, 'segmented.saved'));
check('missing-fixture-before-compiler', { GC_UNIT_LANGUAGE_SDK: '' }, 2, /MISSING_INPUT.*segmented/, '');
renameSync(join(root, 'segmented.saved'), join(src, 'segmented_array_managed.cj'));
check('missing-hook-before-compiler', { GC_UNIT_LANGUAGE_SDK: '' }, 2, /lacks current test hooks/, '');
const globals = join(runtime, 'src/Heap/z/zGlobals.cpp'), globalsOriginal = readFileSync(globals, 'utf8');
writeFileSync(globals, globalsOriginal.replace(/^size_t ZFailLargerCommits =.*\n/m, ''));
const shrunk = spawnSync('python3', [join(src, 'testable_product_hooks.py'), '--source', join(runtime, 'src')], { encoding: 'utf8' });
assert.equal(shrunk.status, 2); assert.match(shrunk.stderr, /target set shrank/); writeFileSync(join(root, 'shrunk-targets.log'), shrunk.stdout + shrunk.stderr);
writeFileSync(globals, globalsOriginal);
recipe(0, 1); nm(false);
shell(join(src, 'run_standalone.sh'), 'echo "GC_UNIT_OHOS_HOST_HEADER_ROOT=${HEADER_TOKEN:-$GCV2_RUNTIME_OUTPUT_ROOT/include}"\nprintf "RESULT=PASS\\nFILTER_HANDLER=%s\\nFILTER_MAJOR=PASS\\nFILTER_POST=PASS\\nFILTER_EMPTY=PASS\\n" "${HANDLER_RESULT:-PASS}" >"$GC_UNIT_OHOS_HOST_RECEIPT"');
check('ohos', {}, 0, /^PASS$/);
check('ohos-header', { HEADER_TOKEN: 'stale/include' }, 2, /HEADER_ROOT_MISMATCH/);
check('ohos-reuse', { HEADER_TOKEN: 'no-compile', GC_UNIT_OHOS_HOST_TEST_ELF: 'explicit-elf' }, 0, /^PASS$/);
check('ohos-handler', { HANDLER_RESULT: 'NOT_RUN' }, 1, /INCOMPLETE FILTER_HANDLER/);
for (const missing of ['CJ_GetUIThreadStackTop', 'CJ_PushUIThreadStackTop']) {
  nm(false, ['CJ_GetUIThreadStackTop', 'CJ_PushUIThreadStackTop'].map(s => s === missing ? s + '_unrelated' : s));
  for (const reuse of ['', 'explicit-elf']) check(`missing-${missing}-${reuse || 'fresh'}`, { GC_UNIT_OHOS_HOST_TEST_ELF: reuse }, 2, /PRODUCT_SYMBOL_MISSING/);
}
recipe(); nm(); shell(join(src, 'run_standalone.sh'), defaultRunner);
renameSync(join(src, 'so_reentry_probe.cpp'), join(root, 'so.saved'));
check('so-fixture-missing', {}, 2, /MISSING_INPUT.*so_reentry_probe/, '');
renameSync(join(root, 'so.saved'), join(src, 'so_reentry_probe.cpp'));
// Preserve the optional real-SO export cut from the old harness. The suite
// runner remains controlled; this proves hook selection, not GC behavior.
if (process.env.GC_UNIT_CONTRACT_PRODUCT_SO) {
  recipe(1);
  const original = process.env.GC_UNIT_CONTRACT_PRODUCT_SO;
  for (const arm of ['candidate', 'cut', 'restored']) {
    const directory = join(root, `real-hook-${arm}`); mkdirSync(directory);
    const so = join(directory, 'libcangjie-runtime.so'); copyFileSync(original, so);
    copyFileSync(join(lib, 'libboundscheck.so'), join(directory, 'libboundscheck.so'));
    if (arm === 'cut') {
      const symbols = spawnSync('nm', ['--defined-only', so], { encoding: 'utf8' });
      assert.equal(symbols.status, 0);
      const matches = symbols.stdout.split('\n').filter(s => s.includes('ZFailLargerCommits')).map(s => s.trim().split(/\s+/).at(-1));
      assert.equal(matches.length, 1);
      assert.equal(spawnSync('objcopy', [`--strip-symbol=${matches[0]}`, so]).status, 0);
    }
    writeFileSync(join(directory, 'runtime.sha256'), createHash('sha256').update(readFileSync(so)).digest('hex') + '\n');
    const status = check(`real-hook-entry-${arm}`, { PATH: process.env.PATH, GCV2_RUNTIME_LIB_DIR: directory },
      arm === 'cut' ? 2 : 0, arm === 'cut' ? /lacks current test hooks/ : /LANGUAGE_DEFERRED/, arm === 'cut' ? '' : 'CPP');
    const hookLog = readFileSync(join(status.EVIDENCE_DIR, 'testable-hooks.log'), 'utf8');
    assert.match(hookLog, /TESTABLE_PRODUCT_HOOK_TARGETS=5/);
    if (arm === 'cut') {
      assert.equal((hookLog.match(/TESTABLE_PRODUCT_HOOK_MISSING=/g) || []).length, 1);
      assert.match(hookLog, /TESTABLE_PRODUCT_HOOK_MISSING=ZFailLargerCommits/);
    }
  }
}
writeFileSync(join(root, 'result.json'), JSON.stringify({ gate_invocations: count, shrunk_target_checks: 1, scope: 'controlled harness; runtime and language production NOT_RUN' }, null, 2) + '\n');
console.log(`GATE_CONTRACT_OK N=${count} evidence=${root}`);
