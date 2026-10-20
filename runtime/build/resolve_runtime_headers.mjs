#!/usr/bin/env zx
import { existsSync, realpathSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';

export function headerRoot(library, explicit = process.env.GCV2_RUNTIME_OUTPUT_ROOT) {
  const root = realpathSync(explicit || resolve(library, '../..'));
  if (!existsSync(join(root, 'include', 'schedule.h'))) {
    throw Error(`missing generated CJThread headers at ${root}/include; set GCV2_RUNTIME_OUTPUT_ROOT to the matching build/runtime-staging`);
  }
  return root;
}
const scriptIndex = process.argv.findIndex((arg, i) => i > 0 && resolve(arg) === fileURLToPath(import.meta.url));
if (scriptIndex > 0) {
  try { console.log(headerRoot(process.argv[scriptIndex + 2])); }
  catch (error) { console.error(`RUNTIME_HEADERS_RESOLVE_FAIL: ${error.message}`); process.exitCode = 2; }
}
