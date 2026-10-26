#!/usr/bin/env zx
// Inspect the actual installed-entry / GC consumer COFF, not a surrogate TU.
// This checks compiler-to-IAT wiring only; it does not execute a Windows DLL.
import assert from 'node:assert/strict';
import fs from 'node:fs';

const [object, mode] = process.argv.slice(2);
assert.ok(['entry', 'gc', 'gc-control'].includes(mode), 'usage: object.obj entry|gc|gc-control');
const b = fs.readFileSync(object);
assert.equal(b.readUInt16LE(0), 0x8664, 'AMD64 COFF required');
assert.equal(b.readUInt16LE(16), 0, 'relocatable object required');
const symbolOffset = b.readUInt32LE(8), symbolCount = b.readUInt32LE(12);
const strings = symbolOffset + symbolCount * 18;
assert.ok(strings + 4 <= b.length, 'COFF symbol table bounds');
const stringEnd = strings + b.readUInt32LE(strings);
assert.ok(stringEnd <= b.length, 'COFF string table bounds');
const symbols = new Map();
for (let i = 0; i < symbolCount;) {
  const p = symbolOffset + i * 18;
  let name;
  if (b.readUInt32LE(p) === 0) {
    const start = strings + b.readUInt32LE(p + 4), end = b.indexOf(0, start);
    assert.ok(start >= strings + 4 && end >= start && end < stringEnd, 'COFF symbol name bounds');
    name = b.toString('utf8', start, end);
  } else {
    name = b.toString('utf8', p, p + 8).replace(/\0.*$/, '');
  }
  symbols.set(i, {name, section: b.readInt16LE(p + 12), storage: b[p + 16]});
  i += 1 + b[p + 17];
}
const relocations = [];
for (let i = 0; i < b.readUInt16LE(2); ++i) {
  const section = 20 + i * 40;
  const offset = b.readUInt32LE(section + 24), count = b.readUInt16LE(section + 32);
  assert.notEqual(count, 0xffff, 'extended relocation tables not expected for these consumers');
  assert.ok(offset + count * 10 <= b.length, 'COFF relocation bounds');
  for (let j = 0; j < count; ++j) {
    const p = offset + j * 10, symbol = symbols.get(b.readUInt32LE(p + 4));
    assert.ok(symbol, 'COFF relocation symbol');
    relocations.push({symbol: symbol.name, type: b.readUInt16LE(p + 8)});
  }
}
const target = 'windowsC2NBodyEnd';
const controls = ['windowsC2NLeaveReturn', 'windowsC2NOverflowReturn', 'windowsC2NBodyStart',
  'windowsC2NFailureEnd', 'windowsStackGrowBodyEnd', 'windowsSafepointBodyEnd',
  'windowsReturnSafepointBodyEnd', 'windowsExclusiveBodyEnd', 'windowsStackOverflowBodyEnd'];
const names = mode === 'entry' ? [target] : mode === 'gc-control' ? controls : [target, ...controls];
const checked = [];
for (const name of names) {
  const imported = [...symbols.values()].filter(s => s.name === '__imp_' + name && s.section === 0 && s.storage === 2);
  const direct = [...symbols.values()].filter(s => s.name === name || s.name.includes('.refptr.' + name));
  const refs = relocations.filter(r => r.symbol === '__imp_' + name);
  assert.ok(imported.length === 1 && direct.length === 0 && refs.length > 0 && refs.every(r => r.type === 4),
    `marker ${name} must use IAT: undefined __imp_ and AMD64 REL32, no direct/thunk reference`);
  checked.push({name, iatRelocations: refs.length});
}
console.log(JSON.stringify({object, mode, checked, scope: 'COFF import wiring only; Windows execution NOT_RUN'}));
