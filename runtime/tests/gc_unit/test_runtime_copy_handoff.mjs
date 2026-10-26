#!/usr/bin/env zx
// Limited CMake/publication/CLI regression: real ELF objects, no target execution.
// SOURCE is the runtime directory; EVIDENCE must be a fresh isolated directory.
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import assert from 'node:assert/strict';

const [sourceArg, evidenceArg] = process.argv.slice(2);
assert.ok(sourceArg && evidenceArg, 'usage: test_runtime_copy_handoff.mjs RUNTIME EVIDENCE');
const source = path.resolve(sourceArg), evidence = path.resolve(evidenceArg);
assert.ok(!fs.existsSync(evidence), 'preserve previous evidence');
fs.mkdirSync(evidence, { recursive: true });
const records = [], checks = [];
const env = { ...process.env };
delete env.GCV2_RUNTIME_COPY_OBJECT;
function run(name, command, args, cwd, extra = {}) {
  const started = new Date().toISOString();
  const p = spawnSync(command, args, { cwd, env: { ...env, ...extra }, encoding: 'utf8', maxBuffer: 8 * 1024 ** 2 });
  fs.writeFileSync(path.join(evidence, name + '.stdout'), p.stdout ?? '');
  fs.writeFileSync(path.join(evidence, name + '.stderr'), p.stderr ?? '');
  records.push({ name, command, args, cwd, started, ended: new Date().toISOString(), rc: p.status, signal: p.signal, error: p.error?.message ?? null });
  fs.writeFileSync(path.join(evidence, 'commands.json'), JSON.stringify(records, null, 2));
  return p;
}
function check(name, pass, detail) {
  checks.push({ name, pass: !!pass, detail });
  fs.writeFileSync(path.join(evidence, 'checks.json'), JSON.stringify(checks, null, 2));
  console.log(`${name}=${pass ? 'PASS' : 'FAIL'} ${detail ?? ''}`);
  assert.ok(pass, name);
}
const layoutRelative = 'build/cmake/RuntimeOutputLayout.cmake';
const helperRelative = 'tests/gc_unit/product_test_configuration.mjs';
const publisherRelative = 'build/publish_runtime_output.py';
const originals = Object.fromEntries([layoutRelative, helperRelative, publisherRelative,
  'build/resolve_runtime_headers.mjs', 'tests/gc_unit/product_test_configuration.py',
  'src/Base/Copy_aarch64.S'].map(n => [n, fs.readFileSync(path.join(source, n))]));

function build(name, arch, cut = false) {
  const root = path.join(evidence, name), src = path.join(root, 'source'), out = path.join(root, 'build');
  fs.mkdirSync(src, { recursive: true });
  for (const [n, bytes] of Object.entries(originals)) {
    fs.mkdirSync(path.dirname(path.join(src, n)), { recursive: true });
    fs.writeFileSync(path.join(src, n), bytes);
  }
  if (cut) {
    const anchor = "        files[inputs['internal_test_objects']['copy_disjoint_words']['path']] = copy_object";
    const text = originals[publisherRelative].toString();
    assert.equal(text.split(anchor).length, 2, 'one production publication edge');
    fs.writeFileSync(path.join(src, publisherRelative), text.replace(anchor, '        pass  # controlled loss of the selected object'));
    fs.writeFileSync(path.join(evidence, 'cut.diff'), `--- a/runtime/${publisherRelative}\n+++ b/runtime/${publisherRelative}\n@@\n-${anchor}\n+        pass  # controlled loss of the selected object\n`);
  }
  // Only the libraries' bodies are fixtures. Layout, target-object argument,
  // copying and public consumer CLI are the unchanged production programs.
  fs.writeFileSync(path.join(src, 'body.c'), 'int fixture_export(void) { return 7; }\n');
  fs.writeFileSync(path.join(src, 'CMakeLists.txt'), `cmake_minimum_required(VERSION 3.19)
project(CopyHandoff C ASM)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(build/cmake/RuntimeOutputLayout.cmake)
cj_runtime_prepare_output_layout()
add_library(boundscheck SHARED body.c)
add_library(cangjie-runtime SHARED body.c)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
  add_library(RuntimeCopy OBJECT src/Base/Copy_aarch64.S)
  target_sources(cangjie-runtime PRIVATE $<TARGET_OBJECTS:RuntimeCopy>)
endif()
add_custom_command(TARGET cangjie-runtime POST_BUILD
  COMMAND "\${Python3_EXECUTABLE}" "\${CMAKE_SOURCE_DIR}/build/publish_runtime_output.py"
    "@$<TARGET_PROPERTY:cangjie-runtime,CJ_RUNTIME_PUBLISH_ARGS>" VERBATIM)
cj_runtime_finalize_output_layout()
`);
  const compiler = process.env.COPY_HANDOFF_CLANG || 'clang';
  const triple = arch === 'aarch64' ? 'aarch64-linux-gnu' : 'x86_64-linux-gnu';
  const configured = run(name + '-configure', 'cmake', ['-S', src, '-B', out, '-G', 'Ninja',
    '-DCMAKE_SYSTEM_NAME=Linux', `-DCMAKE_SYSTEM_PROCESSOR=${arch}`, '-DCMAKE_BUILD_TYPE=Release',
    `-DCMAKE_C_COMPILER=${compiler}`, `-DCMAKE_ASM_COMPILER=${compiler}`, `-DCMAKE_C_COMPILER_TARGET=${triple}`,
    `-DCMAKE_ASM_COMPILER_TARGET=${triple}`, '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
    '-DCMAKE_SHARED_LINKER_FLAGS=-nostdlib -fuse-ld=lld'], root);
  check(name + '.configure', configured.status === 0, `rc=${configured.status}`);
  const built = run(name + '-build', 'cmake', ['--build', out, '--target', 'cangjie-runtime', '-j', '2'], root);
  check(name + '.publish', built.status === 0, `rc=${built.status}`);
  const cache = fs.readFileSync(path.join(out, 'CMakeCache.txt'), 'utf8');
  const published = cache.match(/^OUTPUT_TEMP_PATH:INTERNAL=(.+)$/m)?.[1];
  assert.ok(published, 'real publisher updated selected output');
  const library = path.join(published, 'lib', arch + '_Release');
  const command = ['python3', path.join(src, 'tests/gc_unit/product_test_configuration.py'), src, library, published,
    '--copy-object', '--compiler', `${compiler} --target=${triple}`];
  const result = run(name + '-lookup', command[0], command.slice(1), root);
  return { root, src, out, published, library, command, result };
}

const arm = build('selected-arm', 'aarch64');
const selected = path.join(arm.library, 'gc-unit/Copy_aarch64.o');
check('selected-arm.lookup', arm.result.status === 0 && arm.result.stdout.trim() === fs.realpathSync(selected), `rc=${arm.result.status}`);
const args = fs.readFileSync(path.join(arm.out, 'runtime-publish-args.txt'), 'utf8').trim().split('\n');
const original = args[args.indexOf('--copy-object') + 1];
check('selected-arm.same-object', fs.readFileSync(original).equals(fs.readFileSync(selected)), 'actual CMake target object equals published consumer input');
const ignored = run('ambient-object-ignored', arm.command[0], arm.command.slice(1), arm.root, { GCV2_RUNTIME_COPY_OBJECT: '/not-the-selected-build/object.o' });
check('ambient-object-ignored', ignored.status === 0 && ignored.stdout === arm.result.stdout, `rc=${ignored.status}`);

const cut = build('publication-cut', 'aarch64', true);
const missingObject = path.join(cut.library, 'gc-unit/Copy_aarch64.o');
check('publication-cut.target-reached', cut.result.status === 2 && /GC_UNIT_PRODUCT_CONFIGURATION_FAIL/.test(cut.result.stderr)
  && /ENOENT/.test(cut.result.stderr) && !fs.existsSync(missingObject)
  && [missingObject, path.dirname(missingObject)].some(p => cut.result.stderr.includes(`'${p}'`)), `rc=${cut.result.status}`);
const control = build('x86-control', 'x86_64', true);
check('x86-control.lookup', control.result.status === 0 && control.result.stdout.trim() === '', `rc=${control.result.status}`);
const restored = build('restored-arm', 'aarch64');
check('restored-arm.lookup', restored.result.status === 0 && restored.result.stdout.trim() === path.join(restored.library, 'gc-unit/Copy_aarch64.o'), `rc=${restored.result.status}`);

const bytes = fs.readFileSync(selected);
try {
  fs.copyFileSync(path.join(control.library, 'libcangjie-runtime.so'), selected);
  const wrong = run('wrong-object-architecture', arm.command[0], arm.command.slice(1), arm.root);
  check('wrong-object-architecture', wrong.status === 2 && /architectures differ/.test(wrong.stderr), `rc=${wrong.status}`);
} finally { fs.writeFileSync(selected, bytes); }
const after = run('restored-object', arm.command[0], arm.command.slice(1), arm.root);
check('restored-object', after.status === 0, `rc=${after.status}`);
for (const [n, bytesBefore] of Object.entries(originals)) assert.ok(fs.readFileSync(path.join(source, n)).equals(bytesBefore), `source unchanged: ${n}`);
console.log('RUNTIME_COPY_HANDOFF_OK scope=limited-cmake-publisher-cli target_execution=NOT_RUN');
