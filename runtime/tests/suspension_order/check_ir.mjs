#!/usr/bin/env zx
// Check the actual header consumers, not a duplicate atomic implementation.
import {readFileSync} from 'node:fs';
const path = process.argv[2];
if (!path) throw Error('usage: check_ir.mjs <ir.ll>');
const ir = readFileSync(path, 'utf8');
let ok = true;
for (const name of ['R06GetFlag', 'R06HasSync', 'R06HasAny']) {
  const body = ir.match(new RegExp(`define [^\\n]*@${name}\\([^]*?^}`, 'm'))?.[0];
  const loads = body?.split('\n').filter(line => /load atomic i32\b/.test(line)) ?? [];
  const passed = loads.length === 1 && /\bseq_cst\b/.test(loads[0]);
  console.log(`R06_IR_ASSERT name=${name} loads=${loads.length} sc=${passed}`);
  ok &&= passed;
}
process.exitCode = ok ? 0 : 6;
