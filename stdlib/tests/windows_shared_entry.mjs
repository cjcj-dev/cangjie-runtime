#!/usr/bin/env zx
// Direct CMake graph regression. This does not claim Windows link/run coverage.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

const [output] = process.argv.slice(2);
assert.ok(output && path.isAbsolute(output) && !fs.existsSync(output), 'provide a fresh absolute output directory');
fs.mkdirSync(output, {recursive: true});
const modules = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../libs/cmake');
const commands = [];
function run(name, command, args, cwd) {
    const started = new Date().toISOString();
    const result = spawnSync(command, args, {cwd, encoding: 'utf8', timeout: 60000});
    fs.writeFileSync(path.join(output, name + '.stdout'), result.stdout ?? '');
    fs.writeFileSync(path.join(output, name + '.stderr'), result.stderr ?? '');
    commands.push({name, command, args, cwd, started, ended: new Date().toISOString(),
        rc: result.status, signal: result.signal, error: result.error?.message ?? null});
    fs.writeFileSync(path.join(output, 'commands.json'), JSON.stringify(commands, null, 2));
    assert.equal(result.status, 0, `${name}: preparation failed: ${result.stderr}`);
    return result.stdout;
}
const results = [];
for (const [name, windows, native] of [
    ['windows-native-shared', true, true],
    ['linux-native-shared', false, true],
    ['windows-nonnative-shared', true, false],
]) {
    const source = path.join(output, name), build = path.join(source, 'build');
    fs.mkdirSync(source);
    const target = windows ? 'windows_x86_64' : 'linux_x86_64';
    const common = path.join(source, 'selected-target-install/lib', target + '_cjnative');
    // The graph is generated without substituting a fake compiler or startup object.
    fs.writeFileSync(path.join(source, 'CMakeLists.txt'), `cmake_minimum_required(VERSION 3.16)
project(StdWindowsEntryGraph NONE)
list(APPEND CMAKE_MODULE_PATH "${modules}")
include(MakeCJNATIVEStdSharedLib)
set(MINGW ${windows ? 'ON' : 'OFF'})
set(CANGJIE_CODEGEN_CJNATIVE_BACKEND ${native ? 'ON' : 'OFF'})
set(CJNATIVE_BACKEND cjnative)
set(TARGET_TRIPLE_DIRECTORY_PREFIX ${target})
set(target_folder_name ${windows ? 'windows' : 'linux'})
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_SYSTEM_NAME ${windows ? 'Windows' : 'Linux'})
set(CMAKE_SHARED_LIBRARY_SUFFIX ${windows ? '.dll' : '.so'})
set(CMAKE_C_COMPILER clang)
set(CMAKE_C_COMPILER_ID Clang)
set(TRIPLE ${windows ? 'x86_64-w64-windows-gnu' : 'x86_64-unknown-linux-gnu'})
set(LINKER_OPTION_PREFIX "-Wl,")
set(RUNTIME_COMMON_LIB_DIR "${common}")
function(cj_resolve_depends output)
  set(\${output} \${ARGN} PARENT_SCOPE)
endfunction()
add_custom_target(boundscheck)
make_cangjie_lib(std-sample IS_SHARED)
`);
    run(name + '-configure', 'cmake', ['-S', source, '-B', build, '-G', 'Ninja'], source);
    const argv = run(name + '-commands', 'ninja', ['-C', build, '-t', 'commands', 'std-sample'], source);
    const library = `lib/${target}_cjnative/libcangjie-std-sample${windows ? '.dll' : '.so'}`;
    const query = run(name + '-query', 'ninja', ['-C', build, '-t', 'query', library], source);
    const entry = common + '/cjstartdllentry.o';
    const errors = [];
    const check = (condition, message) => { if (!condition) errors.push(message); };
    if (windows && native) {
        check(argv.includes(entry), 'WINDOWS_SHARED_ENTRY_OBJECT_REQUIRED');
        check(argv.includes('-Wl,-e,__CJ_DllMainCRTStartupV1'), 'WINDOWS_SHARED_CRT_ENTRY_REQUIRED');
        check(query.split('  outputs:')[0].includes(entry), 'WINDOWS_SHARED_ENTRY_FILE_DEPENDENCY_REQUIRED');
    } else {
        check(!argv.includes('cjstartdllentry.o') && !argv.includes('__CJ_DllMainCRTStartupV1'),
            'UNRELATED_SHARED_LINK_MUST_NOT_USE_WINDOWS_ENTRY');
    }
    if (native) check(argv.includes(common + '/cjstart.o'), 'STARTUP_MUST_USE_SELECTED_TARGET');
    if (windows && native) check(argv.includes(common + '/section.o'), 'WINDOWS_SECTION_CONTROL');
    check(argv.includes('-shared'), 'SHARED_LINK_CONTROL');
    check(!argv.includes('-nostartfiles'), 'NORMAL_CRT_STARTUP_CONTROL');
    results.push({name, targetAssertionExecuted: true, errors});
}
fs.writeFileSync(path.join(output, 'results.json'), JSON.stringify(results, null, 2));
for (const result of results) console.log(JSON.stringify(result));
process.exitCode = results.some(result => result.errors.length) ? 1 : 0;
