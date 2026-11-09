#!/usr/bin/env zx
// Run on kkk2 under the lane's existing wf/claim/core0 limits.
// zx regression.mjs build <sdk> <source-dir> <out> <cpus>
// zx regression.mjs run <sdk> <out> <installed-runtime-libdir> <label>
import fs from 'node:fs/promises';
import path from 'node:path';
$.verbose = false;
const [mode, sdk, input, output, extra] = argv._;
if (!['build', 'run'].includes(mode) || !sdk || !input || !output || !extra) {
  throw new Error('usage: build SDK SOURCE OUT CPUS | run SDK OUT RUNTIME_LIBDIR LABEL');
}
const out = mode === 'build' ? output : input;
await fs.mkdir(out, { recursive: true });
const envText = (await $({ env: { HOME: '/root', PATH: '/usr/bin:/bin' } })`bash -c ${'source "$1/envsetup.sh" >/dev/null 2>&1; env -0'} envsetup ${sdk}`).stdout;
const inherited = Object.fromEntries(envText.split('\0').filter(Boolean).map(line => {
  const split = line.indexOf('=');
  return [line.slice(0, split), line.slice(split + 1)];
}));
for (const name of ['_', 'PWD', 'OLDPWD', 'SHLVL']) delete inherited[name];
const records = [];
async function exec(label, file, args, env = inherited) {
  const p = await $({ cwd: out, env, nothrow: true })`${file} ${args}`;
  await fs.writeFile(path.join(out, `${label}.stdout`), p.stdout);
  await fs.writeFile(path.join(out, `${label}.stderr`), p.stderr);
  records.push({ label, file, args, rc: p.exitCode, signal: p.signal ?? null });
  await fs.writeFile(path.join(out, `${mode}-${mode === 'run' ? extra : 'producer-consumer'}.json`), JSON.stringify(records, null, 2) + '\n');
  return p.exitCode;
}
if (mode === 'build') {
  const lib = path.join(out, 'lib');
  await fs.mkdir(lib, { recursive: true });
  const cjc = path.join(sdk, 'bin/cjc');
  let rc = await exec('producer', 'taskset', ['-c', extra, cjc, '-O2', '-p', path.join(input, 'producer'), '--output-type=staticlib', '--output-dir', lib]);
  if (rc !== 0) process.exit(rc);
  rc = await exec('consumer', 'taskset', ['-c', extra, cjc, '-O2', path.join(input, 'main.cj'), '--import-path', lib, '-L', lib, '-l', 'generic_read_payload', '--output-type=exe', '-o', path.join(out, 'consumer')]);
  process.exit(rc);
} else {
  const env = { ...inherited, LD_LIBRARY_PATH: `${output}:${sdk}/tools/lib` };
  const identity = await $`sha256sum ${path.join(out, 'consumer')} ${path.join(output, 'libcangjie-runtime.so')}`;
  await fs.writeFile(path.join(out, `${extra}.identity.txt`), identity.stdout);
  const loaded = await $({ cwd: out, env: { ...env, LD_DEBUG: 'libs' }, nothrow: true })`${path.join(out, 'consumer')} references`;
  await fs.writeFile(path.join(out, `${extra}.loaded.stdout`), loaded.stdout);
  await fs.writeFile(path.join(out, `${extra}.loaded.stderr`), loaded.stderr);
  records.push({ label: `${extra}-loaded-control`, rc: loaded.exitCode, signal: loaded.signal ?? null });
  for (const arm of ['primitive', 'references']) await exec(`${extra}-${arm}`, path.join(out, 'consumer'), [arm], env);
  console.log(JSON.stringify(records));
  process.exit(records.some(r => r.rc !== 0) ? 1 : 0);
}
