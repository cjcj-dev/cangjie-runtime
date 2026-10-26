#!/usr/bin/env zx
// Consume the production COFF object and llvm-readobj/objdump output. This is a
// bounded unwind-record test, not a Windows RtlVirtualUnwind or language test.
import fs from 'node:fs';
import assert from 'node:assert/strict';
const [object, unwindFile, disassemblyFile, mode = 'n2c'] = process.argv.slice(2);
const data = fs.readFileSync(object);
assert.equal(data.readUInt16LE(0), 0x8664, 'production object must remain AMD64 COFF');
const symoff = data.readUInt32LE(8), count = data.readUInt32LE(12);
const strings = symoff + count * 18, symbols = new Map(), symbolNames = new Map();
for (let i = 0; i < count;) {
  const at = symoff + i * 18;
  const name = data.readUInt32LE(at) === 0
    ? data.subarray(strings + data.readUInt32LE(at + 4)).toString().split('\0')[0]
    : data.subarray(at, at + 8).toString().split('\0')[0];
  if (data.readInt16LE(at + 12) > 0) symbols.set(name, data.readUInt32LE(at + 8));
  symbolNames.set(i, name);
  i += 1 + data[at + 17];
}
const callTargets = new Map();
for (let i = 0; i < data.readUInt16LE(2); ++i) {
  const section = 20 + data.readUInt16LE(16) + i * 40;
  const name = data.subarray(section, section + 8).toString().split('\0')[0];
  if (name !== '.text') continue;
  const relocs = data.readUInt32LE(section + 24);
  for (let j = 0; j < data.readUInt16LE(section + 32); ++j) {
    const at = relocs + j * 10;
    if (data.readUInt16LE(at + 8) === 4) {
      callTargets.set(data.readUInt32LE(at) - 1, symbolNames.get(data.readUInt32LE(at + 4)));
    }
  }
}
const instructions = new Map();
for (const line of fs.readFileSync(disassemblyFile, 'utf8').split('\n')) {
  const m = line.match(/^\s*([0-9a-f]+):\s+([a-z][a-z0-9]*)\s*(.*?)\s*$/);
  if (m) instructions.set(parseInt(m[1], 16), [m[2], m[3].split('#')[0].trim()]);
}
const blocks = fs.readFileSync(unwindFile, 'utf8').split('  RuntimeFunction {').slice(1);
const general = ['rbx', 'rbp', 'rsi', 'rdi', 'r12', 'r13', 'r14', 'r15'];
const nonvolatile = [...general, ...Array.from({length: 10}, (_, i) => 'xmm' + (i + 6))];
const stack = 0x100008n, returnPc = 0x23456789n;
const initial = Object.fromEntries([...general, 'rax', 'rcx', 'rdx', 'r8', 'r9', 'r10', 'r11', ...Array.from({length: 16}, (_, i) => 'xmm' + i)]
  .map((r, i) => [r, r.startsWith('xmm') ? (BigInt(i + 1) << 96n) + BigInt(i + 101) : BigInt(i + 1) * 0x10101n]));
initial.rsp = stack;
function write(mem, a, v, n) { for (let i = 0; i < n; i++) mem.set(a + BigInt(i), Number((v >> BigInt(i * 8)) & 255n)); }
function read(mem, a, n) { let v = 0n; for (let i = 0; i < n; i++) { assert.ok(mem.has(a + BigInt(i)), 'unwind reads unsaved memory at ' + a.toString(16)); v |= BigInt(mem.get(a + BigInt(i))) << BigInt(i * 8); } return v; }
function address(s, op) { const m = op.match(/^(-?(?:0x[0-9a-f]+|\d+))?\(%(\w+)\)$/); assert.ok(m, 'unsupported memory operand ' + op); return s.r[m[2]] + BigInt(m[1] || 0); }
function execute(s, [op, args]) {
  const a = args.split(/,\s*/);
  if (op === 'pushq') { s.r.rsp -= 8n; write(s.mem, s.r.rsp, s.r[a[0].slice(1)], 8); }
  else if (op === 'popq') { s.r[a[0].slice(1)] = read(s.mem, s.r.rsp, 8); s.r.rsp += 8n; }
  else if (op === 'retq') { s.r.rip = read(s.mem, s.r.rsp, 8); s.r.rsp += 8n + BigInt(a[0] ? a[0].slice(1) : 0); }
  else if (op === 'subq') s.r[a[1].slice(1)] -= BigInt(a[0].slice(1));
  else if (op === 'addq') s.r[a[1].slice(1)] += BigInt(a[0].slice(1));
  else if (op === 'leaq') s.r[a[1].slice(1)] = address(s, a[0]);
  else if (op === 'xorl' && a[0] === '%edx' && a[1] === '%edx') s.r.rdx = 0n;
  else if (op === 'movl' && a[1] === '%edx' && a[0].startsWith('$')) s.r.rdx = BigInt(a[0].slice(1));
  else if (['movq', 'movapd'].includes(op)) {
    const n = op === 'movapd' ? 16 : 8;
    const v = a[0].startsWith('%') ? s.r[a[0].slice(1)] : a[0].startsWith('$') ? BigInt(a[0].slice(1)) : read(s.mem, address(s, a[0]), n);
    if (a[1].startsWith('%')) s.r[a[1].slice(1)] = v; else write(s.mem, address(s, a[1]), v, n);
  } else throw Error('unsupported production prologue instruction: ' + op + ' ' + args);
}
let checks = 0;
for (const block of blocks) {
  const name = block.match(/StartAddress: (\w+)/)?.[1];
  const selected = ['closure', 'callee', 'exclusive'].includes(mode) || (mode === 'eh-transfer' ? name === 'RestoreContextForEH' : mode === 'c2n' ? name === 'CJ_MCC_C2NStub' : mode === 'control' ? name?.startsWith('__CJ_') : ['CJ_MCC_N2CStub', 'ExecuteCangjieStub', 'ApplyCangjieMethodStub'].includes(name));
  if (!selected) continue;
  const start = symbols.get(name), prolog = Number(block.match(/PrologSize: (\d+)/)[1]);
  assert.ok(start !== undefined, name + ' real production symbol missing');
  if (name === 'RestoreContextForEH' && mode === 'eh-transfer') {
    // ExceptionHandling::ChangeEHStackInfoLR replaces the native return PC.
    // Model that RET, not a CALL to this entry: the managed SP has no fresh RA.
    const managedSp = stack + 8n;
    const state = {r: {...initial, rsp: managedSp - 8n}, mem: new Map()};
    write(state.mem, state.r.rsp, BigInt(start), 8);
    execute(state, ['retq', '']);
    assert.equal(state.r.rip, BigInt(start));
    assert.equal(state.r.rsp, managedSp);
    assert.ok(!state.mem.has(managedSp), 'transfer must not fabricate a native return slot');
    let reachedHelper = false, reachedTransfer = false;
    const landingPad = 0x34567890n;
    const restored = Object.fromEntries(nonvolatile.map((reg, i) => [reg,
      reg.startsWith('xmm') ? (BigInt(i + 51) << 96n) + 0xABCDEFn : BigInt(i + 51) * 0x20202n]));
    restored.rbp = 0x200000n; restored.rsp = 0x1FFF00n;
    for (const [, insn] of [...instructions].filter(([pc]) => pc >= start).sort((a, b) => a[0] - b[0])) {
      if (reachedHelper && ['xorq', 'xorps'].includes(insn[0])) {
        const operands = insn[1].split(/,\s*/);
        assert.equal(operands[0], operands[1], 'EH return clearing must be a self XOR');
        state.r[operands[1].slice(1)] = 0n;
        continue;
      }
      if (reachedHelper && insn[0] === 'jmpq') {
        assert.equal(insn[1], '*%r11', 'actual EH landing-pad branch changed');
        assert.equal(state.r.r11, landingPad);
        for (const reg of [...nonvolatile, 'rsp']) assert.equal(state.r[reg], restored[reg], 'EH updated context restore: ' + reg);
        for (const reg of ['rax', 'rdx', 'xmm0', 'xmm1']) assert.equal(state.r[reg], 0n, 'EH native return register: ' + reg);
        reachedTransfer = true;
        break;
      }
      if (insn[0] !== 'callq') { execute(state, insn); continue; }
      assert.ok(!reachedHelper, 'unexpected additional EH helper call');
      reachedHelper = true;
      assert.equal(state.r.rsp % 16n, 0n, 'EH helper call stack alignment');
      const context = state.r.rcx;
      // Windows CalleeSavedRegisterContext: seven GPRs, padding, ten XMMs,
      // RBP, RSP. Test the actual stores before exercising native home space.
      const fields = [['rbx', 0, 8], ['rsi', 8, 8], ['rdi', 16, 8], ['r12', 24, 8],
        ['r13', 32, 8], ['r14', 40, 8], ['r15', 48, 8],
        ...Array.from({length: 10}, (_, i) => ['xmm' + (i + 6), 64 + i * 16, 16]),
        ['rbp', 224, 8], ['rsp', 232, 8]];
      for (const [reg, offset, width] of fields) {
        assert.equal(read(state.mem, context + BigInt(offset), width), reg === 'rsp' ? managedSp : initial[reg], 'EH context producer: ' + reg);
      }
      // A real Windows native callee may use all four home slots. No helper
      // implementation or OS unwind behavior is inferred from this model.
      for (let i = 0; i < 32; i++) state.mem.set(state.r.rsp + BigInt(i), 0xA5);
      for (const [reg, offset, width] of fields) {
        assert.equal(read(state.mem, context + BigInt(offset), width), reg === 'rsp' ? managedSp : initial[reg], 'EH helper home overwrote saved ' + reg);
      }
      // Exercise the assembly consumer of an updated context. This supplies
      // the helper's output contract; it does not execute managed stackmaps.
      for (const [reg, offset, width] of fields) write(state.mem, context + BigInt(offset), restored[reg], width);
      for (const reg of ['rcx', 'rdx', 'r8', 'r9', 'r10', 'r11', 'xmm0', 'xmm1']) state.r[reg] = 0xBADn;
      state.r.rax = landingPad;
      checks++;
    }
    assert.ok(reachedHelper, 'actual EH helper call must be reached');
    assert.ok(reachedTransfer, 'actual EH context restore and landing-pad branch must be reached');
    console.log('RestoreContextForEH: redirected RET/context/home/register transfer checked; managed stackmaps and SEH/OS NOT_RUN');
    continue;
  }
  if (name === 'RestoreContextForEH') {
    // Entered by a redirected managed return, without a native call/return slot.
    // Do not feed this transfer trampoline to the normal-call stack model.
    assert.equal(prolog, 0, 'context-transfer encoding changed; explicit consumer required');
    console.log(name + ': selected Exception TU; native-call unwind model NOT_APPLICABLE; transfer/OS semantics NOT_RUN');
    continue;
  }
  const frame = block.match(/FrameRegister: (\w+)/)?.[1]?.toLowerCase();
  const bias = Number.parseInt(block.match(/FrameOffset: (0x[0-9A-F]+)/)?.[1] || '0') * 16;
  const codes = [...block.matchAll(/0x([0-9A-F]+): (\w+)([^\n]*)/g)].map(m => ({pc: parseInt(m[1], 16), op: m[2], detail: m[3]}));
  let previous = 256;
  for (const c of codes) { assert.ok(c.pc <= prolog && c.pc <= previous, name + ': unwind offset outside prologue or not descending'); previous = c.pc; }
  const setFrame = codes.find(c => c.op === 'SET_FPREG');
  // These are encoding requirements, not inferred from whether the simulated
  // unwinder happens to recover the register values for one implementation.
  let allocated = false;
  for (const c of [...codes].reverse()) {
    if (c.op === 'PUSH_NONVOL') assert.ok(!allocated, name + ': PUSH_NONVOL must precede other unwind operations');
    else allocated = true;
    if (setFrame && c.op.startsWith('SAVE_')) {
      assert.ok(c.pc > setFrame.pc, name + ': ' + c.op + ' before SET_FPREG');
    }
  }
  function unwind(s, pc) {
    const r = {...s.r}, base = setFrame && pc >= setFrame.pc ? r[frame] - BigInt(bias) : r.rsp;
    for (const c of codes.filter(c => c.pc <= pc)) {
      const reg = c.detail.match(/reg=(\w+)/)?.[1]?.toLowerCase();
      if (c.op === 'PUSH_NONVOL') { r[reg] = read(s.mem, r.rsp, 8); r.rsp += 8n; }
      else if (c.op.startsWith('ALLOC_')) r.rsp += BigInt(c.detail.match(/size=(\d+)/)[1]);
      else if (c.op === 'SET_FPREG') r.rsp = r[frame] - BigInt(bias);
      else if (c.op.startsWith('SAVE_')) {
        const offset = BigInt(c.detail.match(/offset=(0x[0-9A-F]+)/)[1]);
        r[reg] = read(s.mem, base + offset, c.op.startsWith('SAVE_XMM') ? 16 : 8);
      } else throw Error('unsupported unwind opcode ' + c.op);
    }
    for (const reg of nonvolatile) assert.equal(r[reg], initial[reg], name + ': lost ' + reg + ' at PC ' + pc);
    assert.equal(read(s.mem, r.rsp, 8), returnPc, name + ': wrong caller PC');
    assert.equal(r.rsp + 8n, stack + 8n, name + ': wrong caller stack');
    checks++;
  }
  const s = {r: {...initial}, mem: new Map()}; write(s.mem, stack, returnPc, 8);
  unwind(s, 0);
  const ins = [...instructions].filter(([pc]) => pc >= start && pc < start + prolog).sort((a,b) => a[0]-b[0]);
  for (let i = 0; i < ins.length; i++) { execute(s, ins[i][1]); unwind(s, (ins[i+1]?.[0] ?? start + prolog) - start); }
  if (mode === 'n2c' && name !== 'ExecuteCangjieStub') {
    // MRT_SaveTopManagedContextToN2CStub overwrites these three reserved
    // words. Redundant RBP push records must still restore the actual caller
    // RBP from the original anchor, not any of these context values.
    for (let i = 1; i <= 3; i++) write(s.mem, s.r.rbp - BigInt(8 * i), 0xBAD000n + BigInt(i), 8);
    assert.equal(read(s.mem, s.r.rbp - 112n, 8), initial.rsi, 'managed RSI slot changed');
  }
  if (setFrame) assert.equal(s.r[frame] - BigInt(bias), s.r.rsp, name + ': frame register does not describe the completed fixed allocation');
  const nextStart = Math.min(...blocks.map(b => symbols.get(b.match(/StartAddress: (\w+)/)?.[1])).filter(x => x > start), Infinity);
  const functionBody = [...instructions].filter(([pc]) => pc >= start + prolog && pc < nextStart).sort((a,b)=>a[0]-b[0]);
  const hasCall = functionBody.some(([,i])=>i[0] === 'callq');
  const firstCall = functionBody.find(([pc,i])=>i[0] === 'callq')?.[0] ?? nextStart;
  const beforeCall = [...instructions].filter(([pc])=>pc>=start && pc<firstCall);
  const recorded = new Set(codes.map(c=>c.detail.match(/reg=(\w+)/)?.[1]?.toLowerCase()).filter(Boolean));
  for(const [,i] of beforeCall){
    const save=i[1].match(/^%(\w+), .*\(%\w+\)$/);
    if(save && nonvolatile.includes(save[1]))assert.ok(recorded.has(save[1]),name+': nonvolatile save has no unwind record: '+save[1]);
  }

  const stored = new Set(ins.flatMap(([,i])=>{const m=i[1].match(/^%(\w+), .*\(%\w+\)$/);return m && nonvolatile.includes(m[1]) ? [m[1]] : i[0] === 'pushq' ? [i[1].slice(1)] : [];}));
  for (const delta of mode === 'control' || !hasCall ? [0] : [0, 32, 80, 4096]) {
    const body = {r: {...s.r}, mem: new Map(s.mem)};
    if (!setFrame && delta) continue;
    body.r.rsp -= BigInt(delta);
    // Simulate a native callee using all of its permitted home space.
    if (hasCall) for (let i = 0; i < 32; i++) body.mem.set(body.r.rsp + BigInt(i), 0xA5);
    const saved = new Set(codes.map(c => c.detail.match(/reg=(\w+)/)?.[1]?.toLowerCase()).filter(Boolean));
    const changed = ['closure', 'c2n', 'callee', 'exclusive'].includes(mode) ? stored : mode === 'n2c' && name !== 'ExecuteCangjieStub' ? nonvolatile : saved;
    for (const reg of changed) if (reg !== frame && reg !== 'rbp') body.r[reg] = 0xDEADn;
    unwind(body, prolog + 1);
  }
  if (mode === 'callee') {
    const state = {r: {...s.r}, mem: new Map(s.mem)};
    const fifth = 0x512345n, result = 0x678901n, tls = 0xCAFE1234n;
    write(state.mem, stack + 40n, fifth, 8);
    const moreArgs = functionBody.some(([, i]) => i[0] === 'movq' && i[1] === '48(%rbp), %rax');
    let calls = 0, returned = false;
    for (const [pc, insn] of functionBody) {
      if (insn[0] === 'callq') {
        calls++;
        if (calls === 1) {
          for (const reg of ['rcx', 'rdx', 'r8', 'r9']) assert.equal(state.r[reg], initial[reg], name + ': forwarded argument changed');
          if (moreArgs) assert.equal(read(state.mem, state.r.rsp + 32n, 8), fifth, name + ': fifth argument outside native slot');
        } else if (calls === 2) {
          // The result must not overwrite the saved r15 used for unwinding
          // while MRT_GetThreadLocalData is executing.
          unwind(state, prolog + 1);
        } else {
          assert.equal(calls, 3, name + ': unexpected wrapper call');
          assert.equal(callTargets.get(pc), 'MRT_CompleteWindowsExceptionReturn', name + ': real exception bridge edge');
          assert.equal(state.r.rcx, s.r.rbp, name + ': exception bridge frame');
          assert.equal(state.r.rdx, 0n, name + ': exception bridge private cleanup');
          unwind(state, prolog + 1);
        }
        for (let i = 0; i < 32; i++) state.mem.set(state.r.rsp + BigInt(i), 0xA5);
        for (const reg of ['rcx', 'rdx', 'r8', 'r9', 'r10', 'r11']) state.r[reg] = 0xBADn;
        state.r.rax = calls === 1 ? result : calls === 2 ? tls : 0xBADn;
      } else {
        execute(state, insn);
        if (insn[0] === 'retq') { returned = true; break; }
      }
    }
    assert.equal(calls, 3); assert.ok(returned, name + ': wrapper return not reached');
    for (const reg of nonvolatile) assert.equal(state.r[reg], reg === 'r15' ? tls : initial[reg], name + ': wrapper return lost ' + reg);
    assert.equal(state.r.rax, result, name + ': wrapper result lost');
    assert.equal(state.r.rip, returnPc); assert.equal(state.r.rsp, stack + 8n); checks++;
  }
  if (mode === 'exclusive' && name === 'ExecuteExclusiveCangjieStub') {
    const state = {r: {...s.r}, mem: new Map(s.mem)};
    const tls = 0xCAFE1234n, functionPc = 0x456789n;
    const result = 0x123456789n, float0 = 0x123456789ABCDEFn, float1 = 0xFEDCBA987654321n;
    write(state.mem, stack + 40n, tls, 8);
    write(state.mem, initial.r9 + 8n, functionPc, 8);
    let calls = 0, returned = false;
    for (const [pc, insn] of functionBody) {
      if (insn[0] === 'callq') {
        calls++;
        if (calls === 1) {
          assert.equal(state.r.rcx, s.r.rbp); assert.equal(state.r.r15, tls);
          // Actual N2CSlotData occupies three words immediately below FA.
          for (let i = 1; i <= 3; i++) write(state.mem, state.r.rcx - BigInt(i * 8), 0xBAD000n + BigInt(i), 8);
        } else if (calls === 2) {
          assert.equal(insn[1], '*%r11'); assert.equal(state.r.r11, functionPc);
          assert.equal(state.r.rcx, initial.rcx, 'exclusive sret argument');
          assert.equal(state.r.rdx, initial.r8, 'exclusive closure argument');
        } else if (calls === 3) {
          assert.equal(calls, 3); assert.equal(state.r.rcx, s.r.rbp);
        } else {
          assert.equal(calls, 4);
          assert.equal(callTargets.get(pc), 'MRT_CompleteWindowsExceptionReturn');
          assert.equal(state.r.rcx, s.r.rbp, 'exclusive exception boundary');
          assert.equal(state.r.rdx, 0n, 'exclusive private cleanup');
        }
        for (let i = 0; i < 32; i++) state.mem.set(state.r.rsp + BigInt(i), 0xA5);
        for (const reg of ['rax', 'rcx', 'rdx', 'r8', 'r9', 'r10', 'r11', 'xmm0', 'xmm1']) state.r[reg] = 0xBADn;
        if (calls === 2) { state.r.rax = result; state.r.xmm0 = float0; state.r.xmm1 = float1; }
        unwind(state, prolog + 1);
      } else {
        execute(state, insn);
        if (insn[0] === 'retq') { returned = true; break; }
      }
    }
    assert.equal(calls, 4); assert.ok(returned, 'exclusive managed return not reached');
    for (const reg of nonvolatile) assert.equal(state.r[reg], initial[reg], 'exclusive return lost ' + reg);
    assert.equal(state.r.rax, result); assert.equal(state.r.xmm0, float0); assert.equal(state.r.xmm1, float1);
    assert.equal(state.r.rip, returnPc); assert.equal(state.r.rsp, stack + 8n); checks++;
  }
  if (mode === 'c2n' || (mode === 'closure' && name === 'CJ_MCC_C2NStub')) {
    const bridges = functionBody.filter(([pc, i]) => i[0] === 'callq' &&
      callTargets.get(pc) === 'MRT_CompleteWindowsExceptionReturn');
    assert.equal(bridges.length, 2, 'C2N normal and stack-check return must both reach the exception bridge');
    for (const bridge of bridges) {
      const i = functionBody.indexOf(bridge);
      const state = {r: {...s.r}, mem: new Map(s.mem)};
      execute(state, functionBody[i - 2][1]); execute(state, functionBody[i - 1][1]);
      assert.equal(state.r.rcx, s.r.rbp, 'C2N actual exception frame');
      assert.equal(state.r.rdx, 16n, 'C2N exception transfer must account for RET16');
      checks++;
    }
    const index = functionBody.findIndex(([,i])=>i[0] === 'movapd' && i[1] === '-176(%rbp), %xmm6');
    assert.ok(index >= 0, 'C2N production restore must be present');
    const tail = functionBody.slice(index);const end=tail.findIndex(([,i])=>i[0]==='retq');assert.ok(end>=0);tail.length=end+1;
    const epilogue=tail.findIndex(([,i])=>i[0]==='leaq' && i[1].endsWith(', %rsp'));assert.ok(epilogue>=0);
    assert.ok(tail[epilogue][1][1].includes('(%'+frame+')'), 'C2N epilogue actual FP');
    for(const [,i] of tail.slice(epilogue+1,-1))assert.equal(i[0],'popq','C2N epilogue contains a non-POP');
    for(const delta of [0,32,48,96,4096]){
      const state={r:{...s.r},mem:new Map(s.mem)};state.r.rsp-=BigInt(delta);
      for(const r of nonvolatile)if(r!==frame && r!=='rbp')state.r[r]=0xDEADn;
      state.r.rax=0x123456789n;state.r.xmm0=0x123456789ABCDEF123456789n;
      // MRT_C2N_Leave refreshes the managed TLS register in its established slot.
      const tls=0xCAFE1234n;write(state.mem,state.r.rbp-8n,tls,8);
      for(const [,i] of tail)execute(state,i);
      for(const r of nonvolatile)assert.equal(state.r[r],r==='r15'?tls:initial[r],'C2N normal/guard exit lost '+r);
      assert.equal(state.r.rax,0x123456789n);assert.equal(state.r.xmm0,0x123456789ABCDEF123456789n);
      assert.equal(state.r.rip,returnPc);assert.equal(state.r.rsp,stack+24n,'C2N private callee/size cleanup');checks++;
    }
  }
  if (mode === 'n2c') {
    const nextFunction = Math.min(...['CJ_MCC_N2CStub', 'ExecuteCangjieStub', 'ApplyCangjieMethodStub'].map(n => symbols.get(n)).filter(x => x > start), Infinity);
    const bodyIns = [...instructions].filter(([pc]) => pc >= start + prolog && pc < nextFunction).sort((a,b) => a[0]-b[0]);
    if (name === 'ExecuteCangjieStub') {
      const callState = {r: {...s.r}, mem: new Map(s.mem)};
      let calls = 0;
      for (const [,insn] of bodyIns) {
        if (insn[0] === 'callq') {
          if (++calls === 2) {
            for (const reg of ['rcx', 'rdx', 'r8', 'r9']) assert.equal(callState.r[reg], initial[reg], 'ExecuteCangjieStub: home space corrupted ' + reg);
            assert.equal(callState.r.r15, 0x1234n); checks++; break;
          }
          for (let i = 0; i < 32; i++) callState.mem.set(callState.r.rsp + BigInt(i), 0xA5);
          for (const reg of ['rcx', 'rdx', 'r8', 'r9']) callState.r[reg] = 0xBADn;
          callState.r.rax = 0x1234n;
        } else execute(callState, insn);
      }
      assert.equal(calls, 2, 'ExecuteCangjieStub: real helper and managed call must both be reached');
    }
    const restoreStart = name === 'ExecuteCangjieStub'
      ? bodyIns.findIndex(([,i]) => i[0] === 'leaq' && i[1] === '-8(%rbp), %rsp')
      : bodyIns.findIndex(([,i]) => i[0] === 'movq' && i[1] === '-72(%rbp), %r15');
    assert.ok(restoreStart >= 0, name + ': production restore sequence missing');
    const tail = bodyIns.slice(restoreStart);
    const retIndex = tail.findIndex(([,i]) => i[0] === 'retq');
    assert.ok(retIndex >= 0); tail.length = retIndex + 1;
    const officialTail = tail.findIndex(([,i]) => i[0] === 'leaq' && i[1].endsWith(', %rsp'));
    assert.ok(officialTail >= 0, name + ': frame-relative epilogue missing');
    assert.ok(tail[officialTail][1][1].includes('(%' + frame + ')'), name + ': epilogue must use its actual frame register');
    for (const [,i] of tail.slice(officialTail + 1, -1)) assert.equal(i[0], 'popq', name + ': non-POP instruction inside epilogue');
    for (const delta of [32, 80, 4096]) {
      const state = {r: {...s.r}, mem: new Map(s.mem)};
      state.r.rsp -= BigInt(delta);
      if (name === 'ExecuteCangjieStub') state.r.r15 = 0xBADn;
      else for (const reg of nonvolatile) if (reg !== frame && reg !== 'rbp') state.r[reg] = 0xBADn;
      state.r.rax = 0x123456789n; state.r.xmm0 = 0x123456789ABCDEF123456789n;
      for (let i = 0; i < tail.length; i++) {
        if (i < officialTail) unwind(state, prolog + 1);
        execute(state, tail[i][1]);
      }
      for (const reg of nonvolatile) assert.equal(state.r[reg], initial[reg], name + ': normal return lost ' + reg);
      assert.equal(state.r.rax, 0x123456789n); assert.equal(state.r.xmm0, 0x123456789ABCDEF123456789n);
      assert.equal(state.r.rip, returnPc);
      assert.equal(state.r.rsp, stack + (name === 'CJ_MCC_N2CStub' ? 24n : 8n), name + ': private cleanup/return stack');
      checks++;
    }
  }
  console.log(name + (mode === 'closure' ? ': production COFF encoding/prologue/body assertions reached; full execution NOT_RUN' : ': production COFF prologue, body and return unwind assertions reached'));
}
assert.ok(checks > 0 || (mode === 'closure' && symbols.has('RestoreContextForEH')), 'no selected production function tested');
console.log(JSON.stringify({mode, checks, scope: 'decoded COFF + instruction semantics; Windows OS unwinder and execution NOT_RUN'}));
