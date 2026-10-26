#!/usr/bin/env zx
// Internal tests consume the selected normal CMake build's compiler inputs.
// This locates inputs; it does not certify copied libraries or search inventories.
import { readFileSync, realpathSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
import { headerRoot } from '../../build/resolve_runtime_headers.mjs';

export function buildDirectory(output) {
  return realpathSync(process.env.GCV2_RUNTIME_BUILD_DIR || resolve(output, '..'));
}
export function configuration(output) {
  const commands = JSON.parse(readFileSync(join(buildDirectory(output), 'compile_commands.json'), 'utf8'));
  const entries = commands.filter(e => e.file.replaceAll('\\', '/').endsWith('/Heap/z/zGeneration.cpp'));
  if (entries.length !== 1) throw Error('expected one zGeneration.cpp command in the selected CMake build');
  const entry = entries[0];
  const args = entry.arguments || JSON.parse(execFileSync('python3', ['-c',
    'import json,shlex,sys; print(json.dumps(shlex.split(sys.argv[1])))', entry.command], { encoding: 'utf8' }));
  return ['MRT_TESTABLE_INTERNALS', 'MRT_GC_UNIT_TESTS', 'MRT_GC_UNIT_OHOS_HOST', 'NDEBUG', 'MRT_DEBUG']
    .map(name => Number(args.includes(`-D${name}`) || args.includes(`-D${name}=1`)));
}
function elf(path) {
  const b = readFileSync(path).subarray(0, 20);
  if (b.length !== 20 || b.subarray(0, 4).toString() !== '\x7fELF' || ![1, 2].includes(b[5])) throw Error(`invalid ELF: ${path}`);
  return [b[4], b[5], b[5] === 1 ? b.readUInt16LE(18) : b.readUInt16BE(18)];
}
export function copyObject(library, compiler) {
  const args = JSON.parse(execFileSync('python3', ['-c',
    'import json,shlex,sys; print(json.dumps(shlex.split(sys.argv[1])))', compiler], { encoding: 'utf8' }));
  const target = execFileSync(args[0], [...args.slice(1), '-dumpmachine'], { encoding: 'utf8' }).trim().split('-')[0];
  const machines = { aarch64: [2, 1, 183], arm64: [2, 1, 183], x86_64: [2, 1, 62], amd64: [2, 1, 62], arm: [1, 1, 40], armv7: [1, 1, 40], armv7a: [1, 1, 40], armv7l: [1, 1, 40], i386: [1, 1, 3], i686: [1, 1, 3] };
  const identity = elf(join(library, 'libcangjie-runtime.so'));
  if (String(machines[target]) !== String(identity)) throw Error(`compiler target ${target} differs from runtime ELF ${identity}`);
  if (identity[2] !== 183) return '';
  // RuntimeOutputLayout passes the selected RuntimeCopy target to the
  // publisher, which retains it beside this exact published library pair.
  const path = realpathSync(join(library, 'gc-unit', 'Copy_aarch64.o'));
  if (String(elf(path)) !== String(identity)) throw Error('RuntimeCopy object and selected runtime ELF architectures differ');
  return path;
}
const scriptIndex = process.argv.findIndex((arg, i) => i > 0 && resolve(arg) === fileURLToPath(import.meta.url));
if (scriptIndex > 0) {
  try {
    const args = process.argv.slice(scriptIndex + 1), [, library, output] = args;
    if (args.includes('--resolve-root')) console.log(headerRoot(library));
    else if (args.includes('--copy-object')) console.log(copyObject(library, args.includes('--compiler') ? args[args.indexOf('--compiler') + 1] : 'clang++'));
    else console.log(...configuration(output).slice(0, args.includes('--with-debug') ? 5 : 4));
  } catch (error) { console.error(`GC_UNIT_PRODUCT_CONFIGURATION_FAIL: ${error.message}`); process.exitCode = 2; }
}
