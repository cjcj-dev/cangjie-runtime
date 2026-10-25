#!/usr/bin/env zx
// Native CMake graph fixture only: this does not build/qualify a Windows DLL.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {fileURLToPath, pathToFileURL} from 'node:url';
const here = path.dirname(fileURLToPath(import.meta.url));
const modulePath = process.env.WINDOWS_CAPTURE_MODULE || path.resolve(here, '../../build/capture_windows_runtime_definitions.mjs');
const {planRuntimeObjects, symbolBlocks} = await import(pathToFileURL(modulePath));
const out = path.resolve(process.argv[2]);
const source = path.join(out, 'runtime'), build = path.join(out, 'build');
fs.mkdirSync(source, {recursive: true});
const write = (name, text) => { const file = path.join(source, name); fs.mkdirSync(path.dirname(file), {recursive: true}); fs.writeFileSync(file, text); };
write('CMakeLists.txt', `cmake_minimum_required(VERSION 3.19)
project(RuntimeLinkGraphFixture C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
file(MAKE_DIRECTORY "\${CMAKE_BINARY_DIR}/.cmake/api/v1/query/client-runtime-exports")
file(WRITE "\${CMAKE_BINARY_DIR}/.cmake/api/v1/query/client-runtime-exports/codemodel-v2" "")
add_subdirectory(src)
`);
write('src/CMakeLists.txt', `add_library(Common STATIC common.c)
add_library(Heap STATIC reference.c)
target_link_libraries(Heap PUBLIC Common)
add_library(Unrelated STATIC unused.c)
add_library(boundscheck SHARED bounds.c)
add_library(runtime STATIC runtime.c)
add_library(cangjie-runtime SHARED runtime.c)
target_link_libraries(cangjie-runtime PRIVATE Heap boundscheck)
add_dependencies(cangjie-runtime Unrelated)
`);
for (const name of ['common', 'reference', 'unused', 'bounds', 'runtime']) write('src/' + name + '.c', `int ${name}_fixture(void) { return 0; }\n`);
const args = ['-S', source, '-B', build];
const p = spawnSync('cmake', args, {encoding: 'utf8'});
fs.writeFileSync(path.join(out, 'cmake.log'), (p.stdout || '') + (p.stderr || ''));
fs.writeFileSync(path.join(out, 'cmake.json'), JSON.stringify({cmd: 'cmake', args, status: p.status, signal: p.signal, error: p.error?.message}, null, 2));
assert.equal(p.status, 0, 'normal fixture configure');
const plan = planRuntimeObjects(build, path.join(source, 'src'));
fs.writeFileSync(path.join(out, 'plan.json'), JSON.stringify(plan, null, 2));
console.log('TARGET_ASSERT linked_archive_closure reached=1');
assert.deepEqual(plan.linkage.targets.map(t => t.name).sort(), ['Common', 'Heap', 'cangjie-runtime'].sort());
assert.deepEqual(plan.objects.map(o => o.source).sort(), ['common.c', 'reference.c', 'runtime.c']);
const db = path.join(build, 'compile_commands.json'), original = fs.readFileSync(db);
try {
    const commands = JSON.parse(original).filter(c => !c.file.endsWith('/reference.c'));
    fs.writeFileSync(db, JSON.stringify(commands));
    assert.throws(() => planRuntimeObjects(build, path.join(source, 'src')), /selected runtime source must have one object: Heap/);
} finally { fs.writeFileSync(db, original); }
assert.equal(planRuntimeObjects(build, path.join(source, 'src')).objects.length, 3);
assert.equal(symbolBlocks('Symbol {\n Name: defined\n AuxFunctionDef {\n x: 0\n }\n}\n').length, 1);
assert.throws(() => symbolBlocks('Symbol {\n Name: unfinished\n'), /truncated/);
console.log('ASSERT capture_graph PASS actual_cmake_graph=1 windows_product=0');
