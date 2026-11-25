#!/usr/bin/env zx
// Extract the actual production functions; no debugger or native tool is imported or spawned.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import {EventEmitter} from 'node:events';

const bundle=new URL('./android-scene/',import.meta.url);
const source=fs.readFileSync(process.env.SCENE_INTERFACE_SOURCE||new URL('scene.mjs',bundle),'utf8');
function part(start,end){
 const a=source.indexOf(start),b=source.indexOf(end,a);
 assert(a>=0&&b>a,`production fragment missing: ${start}`);
 return source.slice(a,b);
}
function child(pid,exitCode=null){
 const c=new EventEmitter();Object.assign(c,{pid,exitCode,signalCode:null,stdout:new EventEmitter(),stderr:new EventEmitter(),kills:[]});
 c.kill=s=>{c.kills.push(s);return true;};return c;
}
function ioFunctions(next){
 return vm.runInNewContext(part('function watchChild(','function plan(')+'\n({watchChild,realIO})',{
  spawn:()=>next(),setTimeout,clearTimeout,process,Date,Number,Map,Promise,
  join:path.join,HERE:'/unused-fixture',TOTAL_MS:300000,
  existsSync:()=>false,readFileSync:()=>{throw Error('unexpected file read');},
 });
}

test('crash detach supplies Boolean and intent is separate from actual completion',()=>{
 const funcs=vm.runInNewContext(part('function platformConnect(','function launchContract(')+
  part('function sbProcess(','async function drive(')+'\n({launchArgv,sbProcess})',{
  join:path.join,HERE:'/unused-fixture',REMOTE:'/data/local/tmp/readscope-lifetime',process:{env:{HOST_SYMBOLS:'/unused-consumer'}},
 });
 const argv=funcs.launchArgv('/unused-lldb','emulator-5554');
 assert(argv.includes('process detach --keep-stopped true'),'TARGET: Boolean detach argument required');
 const prefix="Process 2400 launched: '/data/local/tmp/readscope-lifetime/consumer' (x86_64)\nDETACH_INTENT\n";
 const intent=funcs.sbProcess(prefix);
 assert.equal(intent.controlled,false);assert.equal(intent.intent,true);
 const failed=funcs.sbProcess(prefix+'Detach failed: lost connection\n');
 assert.equal(failed.detachState,'UNKNOWN');assert.equal(failed.controlled,false);
 const complete=funcs.sbProcess(prefix+'Process 2400 detached\nProcess 2400 exited with status = 0 (0x00000000) \n');
 assert.equal(complete.detachState,'detached');assert.equal(complete.exits[0].controlled,true);
});

test('host exit without close is unconfirmed; actual close retains child rc',async()=>{
 let next=child(424242,0);const f=ioFunctions(()=>next),io=f.realIO();
 await io.run('start-server',['inert-event-emitter'],{background:true});
 const open=await io.reap(next.pid,5);
 assert.equal(open.closed,false,'TARGET: exitCode must not fabricate close');
 assert.equal(next.kills.length,0);assert(!/^REAPED$/m.test(open.stdout));
 next=child(424243);const closedIO=f.realIO();
 await closedIO.run('start-server',['inert-event-emitter'],{background:true});
 next.emit('exit',23,null);next.emit('close',23,null);
 const closed=await closedIO.reap(next.pid,5);
 assert.equal(closed.closed,true);assert.equal(closed.childRc,23);
 assert.equal(closed.childSignal,null);assert.equal(closed.rc,0);assert.equal(closed.stdout.trim(),'REAPED');
 assert.equal(next.kills.length,0);
});

test('foreground actual signal survives without timer intervention',async()=>{
 const f=ioFunctions(()=>{throw Error('spawn forbidden');});
 const c=child(424244),pending=f.watchChild(c,1000,20);
 queueMicrotask(()=>c.emit('close',null,'SIGKILL'));
 const external=await pending;
 assert.equal(external.signal,'SIGKILL','TARGET: preserve actual non-timer close signal');
 assert.equal(external.timer,false);assert.equal(external.timedOut,false);assert.equal(external.closed,true);
 const normal=child(424245),done=f.watchChild(normal,1000,20);
 queueMicrotask(()=>normal.emit('close',7,null));
 const ordinary=await done;
 assert.equal(ordinary.rc,7);assert.equal(ordinary.signal,null);assert.equal(ordinary.timer,false);
});
