#!/usr/bin/env zx
// Synthetic gate inputs use the same compiler database as the native consumer.
import { copyFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
const [source, fixture, output, ohos, testable = '0'] = process.argv.slice(2);
for (const file of ['build/resolve_runtime_headers.mjs', 'tests/gc_unit/product_test_configuration.mjs']) {
  copyFileSync(join(source, 'runtime', file), join(fixture, 'runtime', file));
}
mkdirSync(join(output, 'include'), { recursive: true });
writeFileSync(join(output, 'include/schedule.h'), '// Synthetic gate fixture, not a runtime header.\n');
const args = ['clang++'];
if (ohos === '1') args.push('-DMRT_GC_UNIT_OHOS_HOST=1');
if (testable === '1') args.push('-DMRT_TESTABLE_INTERNALS=1');
writeFileSync(join(dirname(output), 'compile_commands.json'), JSON.stringify([
  { directory: dirname(output), file: '/runtime/src/Heap/z/zGeneration.cpp', arguments: args }
]) + '\n');
