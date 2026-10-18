#!/usr/bin/env zx
// Native/managed fixtures from f0f3542c650a1d4a6904a2e86f6ac3ff534dcffc.
// Invoke only through the admitted box + wf slot, with core soft/hard limits 0.
import { $, fs, path } from 'zx';
import { fileURLToPath } from 'node:url';
import os from 'node:os';

const [runtimeLibArg, sdkArg, outputArg] = process.argv.slice(2);
if (!runtimeLibArg || !sdkArg || !outputArg) {
  throw new Error('usage: build_signal_shutdown.mjs <runtime-lib-dir> <paired-sdk-install> <output-dir>');
}
if (process.platform !== 'linux' || os.arch() !== 'x64') {
  throw new Error('this inherited shutdown fixture build is Linux x86_64 only');
}
const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const lib = await fs.realpath(runtimeLibArg);
const sdk = await fs.realpath(sdkArg);
const output = path.resolve(outputArg);
const cjc = path.join(sdk, 'bin/cjc');
await fs.access(cjc, fs.constants.X_OK);
await fs.ensureDir(output);
const includes = [
  'runtime/src', 'runtime/src/Heap', 'runtime/include', 'runtime/src/Heap/z/os/linux',
  'runtime/src/CJThread/src/runtime/schedule/include',
  'runtime/src/CJThread/src/runtime/schedule/include/inner',
  'runtime/src/CJThread/src/runtime/schedule/include/inner/gas',
  'runtime/src/CJThread/src/runtime/schedule/include/inner/gas/x86/x86_64',
  'runtime/src/CJThread/src/base/log/include', 'runtime/src/CJThread/src/base/mid/include',
  'runtime/third_party/third_party_bounds_checking_function/include',
].map(value => `-I${path.join(repo, value)}`);
includes.push(`-I${path.resolve(lib, '../../include')}`);
const results = [];
async function record(name, command) {
  const result = await command;
  await fs.writeFile(path.join(output, `${name}.log`), result.stdout + result.stderr);
  results.push({name, exitCode: result.exitCode});
  await fs.writeJson(path.join(output, 'build-results.json'), results, {spaces: 2});
  if (result.exitCode !== 0) { process.exit(result.exitCode ?? 1); }
}
for (const [source, name] of [
  ['signal_shutdown.cpp', 'signal-shutdown'], ['shutdown_owner_identity.cpp', 'shutdown-owner-identity'],
]) {
  await record(name, $({nothrow: true})`clang++ -std=c++17 -O0 -g -pthread -fno-rtti ${includes} ${path.join(repo, 'runtime/tests', source)} -L${lib} -lcangjie-runtime -lboundscheck -ldl -Wl,--export-dynamic -o ${path.join(output, name)}`);
}
await record('managed', $({nothrow: true, env: {...process.env, CANGJIE_HOME: sdk, cjEnableGC: '1'}})`${cjc} ${path.join(repo, 'runtime/tests/signal_shutdown_managed.cj')} --output-type=dylib -o ${path.join(output, 'libshutdownprobe.so')}`);
await record('identities', $({nothrow: true})`sha256sum ${path.join(output, 'signal-shutdown')} ${path.join(output, 'shutdown-owner-identity')} ${path.join(output, 'libshutdownprobe.so')} ${path.join(lib, 'libcangjie-runtime.so')} ${path.join(lib, 'libboundscheck.so')}`);
