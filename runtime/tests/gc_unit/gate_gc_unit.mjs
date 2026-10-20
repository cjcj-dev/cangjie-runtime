#!/usr/bin/env zx
// Execute consumers against explicit normal build/install inputs. Results are
// invocation facts, never a reusable admission stamp or a product inventory.
import { readFileSync, writeFileSync, mkdirSync, mkdtempSync, existsSync, openSync, closeSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { configuration } from './product_test_configuration.mjs';
import { headerRoot } from '../../build/resolve_runtime_headers.mjs';
import { languageInputs } from './language_toolchain.mjs';

const src = dirname(fileURLToPath(import.meta.url)), runtime = resolve(src, '../..');
const env = { ...process.env };
const root = env.GC_UNIT_OUT || join(src, 'build_standalone');
mkdirSync(root, { recursive: true });
const out = mkdtempSync(join(root, 'run-'));
env.GC_UNIT_OUT = out;
env.GC_UNIT_TALLY_FILE = join(out, 'gate_tally.txt');
env.GC_UNIT_OHOS_HOST_RECEIPT = join(out, 'ohos_host.results');
env.SIGNAL_TEST_OUTPUT = join(out, 'so-reentry');
const mode = env.GC_UNIT_GATE_LANGUAGE_TESTS || 'all';
const status = { GATE: 'FAIL', REASON: 'STARTED', EVIDENCE_DIR: out, LANGUAGE_TEST_MODE: mode,
  CPP_SUITE: 'NOT_RUN', LANGUAGE_TESTS: 'NOT_RUN', CPP_RUNNER_RC: 'NOT_RUN',
  OHOS_RUNNER_RC: 'NOT_RUN', FINALIZER_RUNNER_RC: 'NOT_RUN', PHASE_ENTRY_RUNNER_RC: 'NOT_RUN',
  SO_REENTRY_RUNNER_RC: 'NOT_RUN', SEGMENTED_RUNNER_RC: 'NOT_RUN' };
function fail(reason, code = 2) { throw Object.assign(Error(reason), { code }); }
function run(name, command, args = [], extra = {}) {
  const path = join(out, `${name}.log`), fd = openSync(path, 'w');
  let result;
  try { result = spawnSync(command, args, { env: { ...env, ...extra }, stdio: ['ignore', fd, fd] }); }
  finally { closeSync(fd); }
  const rc = result.status ?? (result.signal === 'SIGTERM' ? 143 : result.signal === 'SIGKILL' ? 137 : 127);
  writeFileSync(join(out, `${name}.rc`), `${rc}\n`);
  writeFileSync(join(out, `${name}.command.json`), JSON.stringify({ command, args, rc, signal: result.signal, error: result.error?.message }, null, 2) + '\n');
  return { rc, text: readFileSync(path, 'utf8') };
}
function script(name, file, args = []) {
  return run(name, 'timeout', [env.GC_UNIT_TIMEOUT || '600', 'bash', join(src, file), ...args]);
}
function requireFile(path) { if (!existsSync(path)) fail(`MISSING_INPUT ${path}`); }
try {
  if (!['all', 'defer', 'only'].includes(mode)) fail('GC_UNIT_GATE_LANGUAGE_TESTS must be all, defer, or only');
  if (env.GC_UNIT_GATE_CONTRACT_SELFTEST !== '1') {
    const contract = run('gate-contract', 'zx', [join(src, 'test_gate_testable_contract.mjs')],
      { GC_UNIT_GATE_CONTRACT_SELFTEST: '1', GC_UNIT_CONTRACT_EVIDENCE: join(out, 'contract') });
    if (contract.rc) fail('gate contract failed', 2);
  }
  if (env.GC_UNIT_GATE_SKIP === '1') {
    status.GATE = 'NOT_RUN'; status.REASON = 'EXPLICIT_SKIP';
  } else {
    if (!env.GCV2_RUNTIME_LIB_DIR) fail('set GCV2_RUNTIME_LIB_DIR to the selected build/install library directory');
    const so = join(env.GCV2_RUNTIME_LIB_DIR, 'libcangjie-runtime.so');
    requireFile(so); requireFile(join(env.GCV2_RUNTIME_LIB_DIR, 'libboundscheck.so'));
    env.GCV2_RUNTIME_OUTPUT_ROOT = headerRoot(env.GCV2_RUNTIME_LIB_DIR);
    const [testable, , ohos] = configuration(env.GCV2_RUNTIME_OUTPUT_ROOT);
    if (env.MRT_TESTABLE_INTERNALS === '1' && !testable) fail('TESTABLE_INTERNALS=1 conflicts with selected CMake build');
    for (const file of ['run_so_reentry_bounded.sh', 'so_reentry_probe.cpp', 'run_finalizer_trigger.sh',
      'finalizer_trigger.cj', 'run_phase_entry_trigger.sh', 'phase_entry_trigger.cj', 'phase_entry_major.cj']) requireFile(join(src, file));
    if (mode !== 'only') requireFile(join(src, 'test_defect_regressions.cpp'));
    status.RUNTIME_LIB_DIR = env.GCV2_RUNTIME_LIB_DIR;
    status.RUNTIME_HEADERS = env.GCV2_RUNTIME_OUTPUT_ROOT;
    if (testable) requireFile(join(src, 'segmented_array_managed.cj'));
    if (ohos) {
      const product = run('ohos-product-symbols', 'nm', ['--defined-only', so]);
      for (const symbol of ['CJ_GetUIThreadStackTop', 'CJ_PushUIThreadStackTop']) {
        if (product.rc || !new RegExp(`\\s${symbol}(?:@@?\\S+)?$`, 'm').test(product.text)) fail(`OHOS_HOST_PRODUCT_SYMBOL_MISSING ${symbol}`);
      }
      const result = script('ohos-suite', 'run_standalone.sh');
      status.OHOS_RUNNER_RC = result.rc;
      if (result.rc) fail('OHOS_HOST_SUITE_FAILURE', 1);
      if (!env.GC_UNIT_OHOS_HOST_TEST_ELF && !result.text.split('\n').includes(`GC_UNIT_OHOS_HOST_HEADER_ROOT=${env.GCV2_RUNTIME_OUTPUT_ROOT}/include`)) fail('OHOS_HOST_HEADER_ROOT_MISMATCH');
      const facts = readFileSync(env.GC_UNIT_OHOS_HOST_RECEIPT, 'utf8').split('\n');
      for (const key of ['RESULT', 'FILTER_HANDLER', 'FILTER_MAJOR', 'FILTER_POST', 'FILTER_EMPTY']) {
        if (!facts.includes(`${key}=PASS`)) fail(`OHOS_HOST_INCOMPLETE ${key}`, 1);
      }
      status.CPP_SUITE = 'PASS'; status.OHOS_FILTERS = 4;
    } else {
      // Product capability checks precede a missing compiler: a testable build
      // cannot hide missing hooks or fixtures behind unavailable language inputs.
      const hooks = run('testable-hooks', 'python3', [join(src, 'testable_product_hooks.py'), '--source', join(runtime, 'src'), '--so', so]);
      if (testable && hooks.rc) fail('TESTABLE_INTERNALS=1 but product SO lacks current test hooks');
      if (!hooks.rc) requireFile(join(src, 'run_segmented_array_managed.sh'));
      const symbols = run('product-symbols', 'nm', ['--defined-only', so]);
      const guard = !symbols.rc && ['Expand', 'Recover', 'Get'].every(s => new RegExp(`\\sCJ_CJThreadStackGuard${s}(?:@@?\\S+)?$`, 'm').test(symbols.text));
      if (mode !== 'only') {
        const names = text => [...new Set(text.match(/test_[a-z0-9_]+\.cpp/g) || [])].sort().join('\n');
        if (names(readFileSync(join(src, 'CMakeLists.txt'), 'utf8')) !== names(readFileSync(join(src, 'run_standalone.sh'), 'utf8'))) fail('CMake and standalone test lists differ', 5);
        const parallel = script('parallel-runner-contract', 'test_parallel_runner.sh');
        if (parallel.rc) fail('parallel runner contract failed', 1);
        const result = script('cpp-suite', 'run_standalone.sh');
        status.CPP_RUNNER_RC = result.rc;
        if (result.rc) fail(`CPP_SUITE_EXIT_FAILURE rc=${result.rc}`, result.rc === 124 ? 6 : 1);
        const tally = existsSync(env.GC_UNIT_TALLY_FILE) ? readFileSync(env.GC_UNIT_TALLY_FILE, 'utf8') : '';
        const match = tally.match(/^\[========\] (\d+) tests: (\d+) passed, (\d+) failed\n?$/);
        if (!match) fail('CPP_SUITE_INCOMPLETE: no valid independent tally', 3);
        const [total, passed, failed] = match.slice(1).map(Number);
        const actual = [...new Set([...result.text.matchAll(/^\[  FAIL  \] ([A-Za-z0-9_]+\.[A-Za-z0-9_]+)/gm)].map(m => m[1]))].sort();
        const allowed = [...new Set(readFileSync(join(src, 'known_failures.txt'), 'utf8').split('\n').map(s => s.trim()).filter(s => s && !s.startsWith('#')))].sort();
        if (!total || total !== passed + failed || failed !== actual.length) fail('CPP_SUITE_EVIDENCE_MISMATCH_OR_EMPTY', 3);
        if (JSON.stringify(actual) !== JSON.stringify(allowed)) fail('CPP_SUITE_FAILURE: actual failures differ from known_failures.txt', 1);
        status.CPP_SUITE = 'PASS'; status.CPP_TESTS = total;
      }
      if (mode === 'defer') status.LANGUAGE_TESTS = 'LANGUAGE_DEFERRED';
      else {
        const inputs = languageInputs(env);
        writeFileSync(join(out, 'language-inputs.json'), JSON.stringify(inputs, null, 2) + '\n');
        env.CANGJIE_HOME = inputs.sdk; env.CJC = inputs.cjc;
        const cases = [ ['FINALIZER', 'run_finalizer_trigger.sh', []], ['PHASE_ENTRY', 'run_phase_entry_trigger.sh', []] ];
        if (guard) cases.push(['SO_REENTRY', 'run_so_reentry_bounded.sh', []]);
        if (!hooks.rc) cases.push(['SEGMENTED', 'run_segmented_array_managed.sh', ['both']]);
        for (const [name, file, args] of cases) {
          const result = script(name.toLowerCase(), file, args);
          status[`${name}_RUNNER_RC`] = result.rc;
          if (result.rc) fail(`${name}_FAILURE rc=${result.rc}`, 1);
        }
        status.LANGUAGE_TESTS = 'LANGUAGE_DONE';
      }
    }
    status.GATE = 'PASS'; status.REASON = !ohos && mode === 'defer' ? 'LANGUAGE_DEFERRED' : 'PASS';
  }
} catch (error) {
  status.REASON = error.message; process.exitCode = error.code && Number.isInteger(error.code) ? error.code : 2;
  console.error(`GC_UNIT_GATE_FAIL: ${error.message}`);
} finally {
  status.GATE_RC = process.exitCode || 0;
  const text = Object.entries(status).map(([key, value]) => `${key}=${value}`).join('\n') + '\n';
  writeFileSync(join(out, 'gate.status'), text);
  if (env.GC_UNIT_GATE_STATUS) { mkdirSync(dirname(env.GC_UNIT_GATE_STATUS), { recursive: true }); writeFileSync(env.GC_UNIT_GATE_STATUS, text); }
  console.log(`GC_UNIT_GATE_${status.GATE} cpp=${status.CPP_SUITE} language=${status.LANGUAGE_TESTS} status=${join(out, 'gate.status')}`);
}
