#!/usr/bin/env zx
// Run through the admitted box + wf slot (core soft/hard 0).
import { $, fs, path } from 'zx';
import { fileURLToPath } from 'node:url';
const [sdkArg, outputArg] = process.argv.slice(2);
if (!sdkArg || !outputArg) { throw new Error('usage: build.mjs <matching-sdk-install> <output>'); }
const sdk = await fs.realpath(sdkArg);
const output = path.resolve(outputArg);
const source = path.dirname(fileURLToPath(import.meta.url));
const cjc = path.join(sdk, 'bin/cjc');
await fs.access(cjc, fs.constants.X_OK);
await fs.ensureDir(output);
const results = [];
async function record(name, command) {
  const result = await command;
  await fs.writeFile(path.join(output, `${name}.log`), result.stdout + result.stderr);
  results.push({name, exitCode: result.exitCode});
  await fs.writeJson(path.join(output, 'build-results.json'), results, {spaces: 2});
  if (result.exitCode !== 0) { process.exit(result.exitCode ?? 1); }
}
await record('observer', $({nothrow: true})`clang++ -std=c++17 -O0 -g -shared -fPIC ${path.join(source, 'observer.cpp')} -o ${path.join(output, 'libregistration-observer.so')}`);
await record('constructors', $({nothrow: true, env: {...process.env, CANGJIE_HOME: sdk, cjEnableGC: '1'}})`${cjc} ${path.join(source, 'constructors.cj')} -O0 --static-std -L${output} -lregistration-observer -o ${path.join(output, 'constructors')}`);
await record('identities', $({nothrow: true})`sha256sum ${path.join(output, 'constructors')} ${path.join(output, 'libregistration-observer.so')}`);
