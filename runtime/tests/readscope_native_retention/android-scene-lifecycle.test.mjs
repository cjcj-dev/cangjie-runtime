#!/usr/bin/env zx
// Local Node children and inert EventEmitters only. Never starts Android tools.
import test from 'node:test';
import assert from 'node:assert/strict';
import {EventEmitter} from 'node:events';
import {spawn} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {ownChild,settleOwnedChild,asyncRunner} from './android-scene.mjs';

async function localChild(body) {
  const child=spawn(process.execPath,['-e',body],{stdio:['ignore','pipe','pipe']});
  const owned=ownChild(child);
  child.stderr.resume();
  await new Promise((resolve,reject)=>{
    const timer=setTimeout(()=>{child.kill('SIGKILL');reject(Error('local fixture not ready'));},2000);
    child.stdout.on('data',chunk=>{if(chunk.toString().includes('READY')){clearTimeout(timer);resolve();}});
    child.once('error',error=>{clearTimeout(timer);reject(error);});
  });
  child.stdout.resume();
  return owned;
}

test('ordinary close preserves its own rc without intervention',async()=>{
  const owned=await localChild('console.log("READY");setTimeout(()=>process.exit(7),30)');
  const now=Date.now();
  const result=await settleOwnedChild(owned,{naturalUntilMs:now+1000,deadlineMs:now+1500});
  assert.equal(result.reaped,true);assert.equal(result.close.rc,7);
  assert.equal(result.timedOut,false);assert.deepEqual(result.signals,[]);
});

test('TERM is followed by actual delayed close before return',async()=>{
  const owned=await localChild('process.on("SIGTERM",()=>setTimeout(()=>process.exit(23),80));console.log("READY");setInterval(()=>{},1000)');
  const now=Date.now();
  const result=await settleOwnedChild(owned,{naturalUntilMs:now+10,deadlineMs:now+1000,termGraceMs:300,killGraceMs:300});
  assert.equal(result.timedOut,true);assert.equal(result.reaped,true);
  assert.equal(result.close.rc,23);assert.equal(result.close.signal,null);
  assert.deepEqual(result.signals.map(row=>row.signal),['SIGTERM']);
  assert(result.close.at>=result.signals[0].at+60);
});

test('ignored TERM escalates only the owned child and waits for SIGKILL close',async()=>{
  const owned=await localChild('process.on("SIGTERM",()=>{});console.log("READY");setInterval(()=>{},1000)');
  const now=Date.now();
  const result=await settleOwnedChild(owned,{naturalUntilMs:now+10,deadlineMs:now+900,termGraceMs:100,killGraceMs:200});
  assert.equal(result.reaped,true);assert.equal(result.close.signal,'SIGKILL');
  assert.equal(result.close.rc,null);assert.equal(result.pid,owned.child.pid);
  assert.deepEqual(result.signals.map(row=>row.signal),['SIGTERM','SIGKILL']);
  assert(result.signals.every(row=>row.accepted));assert(result.observedAt<=result.deadlineMs);
});

test('exit with open pipes is not close and must not be signalled again',async()=>{
  const child=new EventEmitter();child.pid=999999;
  child.kill=()=>assert.fail('must not signal an exited child');
  const owned=ownChild(child);child.emit('exit',0,null);
  const timer=setTimeout(()=>child.emit('close',0,null),40);
  try {
    const now=Date.now();
    const result=await settleOwnedChild(owned,{naturalUntilMs:now+5,deadlineMs:now+300,termGraceMs:50,killGraceMs:50});
    assert.equal(result.reaped,true);assert.equal(result.close.rc,0);
    assert.equal(result.timedOut,true);assert.deepEqual(result.signals,[]);
  } finally {clearTimeout(timer);}
});

test('expired shared deadline returns UNKNOWN without adding a new wait',async()=>{
  const child=new EventEmitter();child.pid=999998;
  child.kill=()=>assert.fail('deadline exhausted');
  const owned=ownChild(child),now=Date.now();
  const result=await settleOwnedChild(owned,{naturalUntilMs:now-100,deadlineMs:now-20});
  assert.equal(result.reaped,false);assert.equal(result.close,null);
  assert.equal(result.timedOut,true);assert.deepEqual(result.signals,[]);
  assert(Date.now()-now<100);
});

test('async wrapper runner persists close and timeout as separate facts',async()=>{
  const root=process.env.LIFECYCLE_TEST_OUT;
  assert(root && path.isAbsolute(root),'set LIFECYCLE_TEST_OUT to an isolated evidence directory');
  fs.mkdirSync(root,{recursive:true});
  const out=fs.mkdtempSync(path.join(root,'runner-'));
  const run=asyncRunner(out,process.env),now=Date.now();
  const result=await run('local-scene-surrogate',[process.execPath,'-e',
    'process.on("SIGTERM",()=>setTimeout(()=>process.exit(23),80));console.log("READY");setInterval(()=>{},1000)'],
  {naturalUntilMs:now+250,deadlineMs:now+1000,termGraceMs:250,killGraceMs:250});
  assert.equal(result.reaped,true);assert.equal(result.timedOut,true);
  assert.equal(result.rc,23);assert.equal(result.signal,null);
  assert.equal(result.close.rc,23);assert(result.close.at>=result.signals[0].at+60);
  assert.equal(fs.readFileSync(path.join(out,'local-scene-surrogate.stdout'),'utf8').trim(),'READY');
  const saved=JSON.parse(fs.readFileSync(path.join(out,'commands.json')))[0];
  assert.equal(saved.pid,result.pid);assert.equal(saved.rc,23);
  assert.equal(saved.timedOut,true);assert.equal(saved.reaped,true);
});
