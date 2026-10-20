#!/usr/bin/env zx
import { accessSync, constants, realpathSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export function languageInputs(env = process.env) {
  const required = name => {
    if (!env[name]) throw Error(`set ${name}`);
    return realpathSync(env[name]);
  };
  const sdk = required('GC_UNIT_LANGUAGE_SDK');
  const host = required('GC_UNIT_CJC_RUNTIME_LIB_DIR');
  const target = required('GCV2_RUNTIME_LIB_DIR');
  const cjc = join(sdk, 'bin/cjc');
  accessSync(cjc, constants.X_OK);
  if (env.CJC && realpathSync(env.CJC) !== realpathSync(cjc)) throw Error('CJC conflicts with GC_UNIT_LANGUAGE_SDK/bin/cjc');
  for (const path of [join(sdk, 'lib/linux_x86_64_cjnative/libcangjie-std-core.a'),
    join(host, 'libcangjie-runtime.so'), join(target, 'libcangjie-runtime.so'), join(target, 'libboundscheck.so')]) accessSync(path, constants.R_OK);
  return { sdk, cjc, compiler_host_dir: host, runtime_lib_dir: target };
}
if (process.argv.some((arg, i) => i > 0 && resolve(arg) === fileURLToPath(import.meta.url))) {
  try { console.log(JSON.stringify(languageInputs())); }
  catch (error) { console.error(`LANGUAGE_INPUT_NOT_RUN: ${error.message}`); process.exitCode = 2; }
}
