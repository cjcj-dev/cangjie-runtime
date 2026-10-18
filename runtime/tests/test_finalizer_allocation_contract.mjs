#!/usr/bin/env zx
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Check a real, unstripped product SO: dynamic symbols alone miss local definitions.
import { spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';

const [so] = process.argv.slice(2);
if (!so || process.argv.length !== 3) {
  process.stderr.write('usage: test_finalizer_allocation_contract.mjs <unstripped-runtime-so>\n');
  process.exit(2);
}
const nm = spawnSync('nm', ['--defined-only', '--demangle', so], { encoding: 'utf8', maxBuffer: 32 * 1024 * 1024 });
if (nm.error || nm.status !== 0 || !nm.stdout.trim()) {
  process.stderr.write(`NM_EXECUTOR_FAILURE status=${nm.status} error=${nm.error ?? ''} ${nm.stderr ?? ''}\n`);
  process.exit(2);
}
const symbols = nm.stdout.split('\n').map(line => line.replace(/^\s*[0-9a-fA-F]+\s+\S\s+/, ''));
console.log(`PRODUCT_SO_SHA256=${createHash('sha256').update(readFileSync(so)).digest('hex')} path=${so}`);
let failures = 0;
const check = (name, pass, evidence) => {
  console.log(`FINALIZER_ALLOCATION_TARGET name=${name} pass=${Number(pass)} evidence=${JSON.stringify(evidence)}`);
  if (!pass) ++failures;
};
const exact = name => symbols.includes(name);
for (const name of ['MCC_NewObject', 'CJ_MCC_NewObject', 'MCC_NewPinnedObject', 'CJ_MCC_NewPinnedObject',
                    'MCC_OnFinalizerCreated', 'CJ_MCC_OnFinalizerCreated']) {
  check(`preserve_${name}`, exact(name), name);
}
check('preserve_finalizer_thread', symbols.some(s => s === 'MapleRuntime::NewFinalizerCJThread()'),
      symbols.filter(s => s.includes('NewFinalizerCJThread')));
const old = symbols.filter(s => /^(?:CJ_)?MCC_NewFinalizer(?:Fast)?$/.test(s) ||
  /^MapleRuntime::(?:MObject|ObjectManager)::NewFinalizer\(/.test(s));
check('remove_finalizer_allocation_family', old.length === 0, old);
const pinned = symbols.filter(s => /^MapleRuntime::ObjectManager::NewPinnedObject\(/.test(s));
check('remove_pinned_finalizer_flag', pinned.every(s => !/\bbool\b/.test(s)), pinned);
process.exit(failures ? 1 : 0);
