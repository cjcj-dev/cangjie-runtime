#!/usr/bin/env zx
// Exercise the real add_cangjie_library CMake module, without running a compiler.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';

const [candidate, baseline, output] = process.argv.slice(2);
assert.ok(candidate && baseline && output,
    'usage: lto_file_dependency.mjs <candidate module> <baseline module> <fresh output>');
assert.ok(!fs.existsSync(output));
fs.mkdirSync(output, {recursive: true});
const archive = 'modules/linux_x86_64_cjnative/std/std.sample.a';
const bitcode = 'modules/linux_x86_64_cjnative_bc/std/libstd.sample.bc';

function run(name, cmd, argv, cwd, expected = 0) {
    const started = new Date().toISOString();
    const result = spawnSync(cmd, argv, {cwd, encoding: 'utf8', timeout: 60000});
    fs.writeFileSync(path.join(output, name + '.stdout'), result.stdout ?? '');
    fs.writeFileSync(path.join(output, name + '.stderr'), result.stderr ?? '');
    fs.writeFileSync(path.join(output, name + '.json'), JSON.stringify({cmd, argv, cwd,
        started, ended: new Date().toISOString(), rc: result.status, signal: result.signal,
        error: result.error?.message ?? null}));
    assert.equal(result.status, expected, `${name}: ${result.stderr}`);
    return result.stdout;
}

for (const [arm, module, expected] of [
    ['candidate', candidate, 0], ['baseline-cut', baseline, 1], ['restore', candidate, 0],
]) {
    const source = path.join(output, arm), build = path.join(source, 'build');
    fs.mkdirSync(path.join(source, 'sample'), {recursive: true});
    fs.writeFileSync(path.join(source, 'sample', 'sample.cj'), 'package sample\n');
    fs.writeFileSync(path.join(source, 'CMakeLists.txt'), `cmake_minimum_required(VERSION 3.16)
project(StdLtoDependency NONE)
set(CANGJIE_CODEGEN_CJNATIVE_BACKEND ON)
set(TARGET_TRIPLE_DIRECTORY_PREFIX linux_x86_64)
set(TRIPLE x86_64-unknown-linux-gnu)
include("${path.resolve(module)}")
add_cangjie_library(sample IS_PACKAGE IS_STDLIB IS_CJNATIVE_BACKEND NO_INSTALL
  MODULE_NAME std PACKAGE_NAME sample SOURCE_DIR "\${CMAKE_CURRENT_SOURCE_DIR}/sample")
`);
    run(arm + '-configure', 'cmake', ['-S', source, '-B', build, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release'], source);
    const query = run(arm + '-query', 'ninja', ['-t', 'query', bitcode], build);
    const queryFile = path.join(output, arm + '-query.stdout');
    // This child owns the target assertion/rc. An order-only edge cannot dirty BC.
    run(arm + '-target', process.execPath, ['--input-type=module', '-e', `
import fs from 'node:fs';import assert from 'node:assert/strict';
const text=fs.readFileSync(process.argv[1],'utf8');
const inputs=text.split('  input:')[1].split('  outputs:')[0]
  .split(String.fromCharCode(10)).map(s=>s.trim()).filter(s=>s && !s.startsWith('|'));
console.log('LTO_FILE_DEPENDENCY_TARGET executed=1');
assert.ok(inputs.some(s=>s===process.argv[2] || s.endsWith('/'+process.argv[2])),
  'LTO_ARCHIVE_FILE_EDGE_REQUIRED');
`, queryFile, archive], build, expected);
    const control = run(arm + '-control-query', 'ninja', ['-t', 'query', archive], build);
    const controlInputs = control.split('  input:')[1].split('  outputs:')[0]
        .split('\n').map(s => s.trim()).filter(s => s && !s.startsWith('|'));
    assert.ok(controlInputs.some(s => path.resolve(build, s) === path.join(source, 'sample', 'sample.cj')),
        'independent archive source dependency must remain');
    console.log(JSON.stringify({arm, targetRc: expected, control: 'PASS', compilerInvocations: 0}));
}
fs.writeFileSync(path.join(output, 'result.json'), JSON.stringify({scope: 'CMake/Ninja dependency graph only',
    arms: 3, compilerInvocations: 0, nativeProduct: 'NOT_RUN', pairedMacro: 'NOT_RUN'}));
