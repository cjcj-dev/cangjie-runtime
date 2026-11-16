#!/usr/bin/env zx
// Run the real prepare body against pure filesystem inputs; no tool is spawned.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';

const source=fs.readFileSync(process.env.SCENE_DISK_SOURCE||new URL('./android-scene.mjs',import.meta.url),'utf8');
const start=source.indexOf('function prepare() {'),end=source.indexOf('\nasync function execute()',start);
assert(start>=0&&end>start,'actual prepare function required');
const body=source.slice(start,end);
const fixed=JSON.parse(fs.readFileSync(new URL('./android-scene/input.json',import.meta.url),'utf8'));
const temp='/runner/_temp',out=temp+'/readscope-android-scene';
function attempt(blocks){
 const calls=[],records=[];
 const firstResource=new Error('PURE_FIXTURE_FIRST_RESOURCE');
 const prepare=vm.runInNewContext(body+'\nprepare',{
  context:()=>({out,sdk:'/unused-sdk',env:{}}),
  process:{env:{RUNNER_TEMP:temp}},path,assert,Date,fixed,here:'/checkout/runtime/tests/readscope_native_retention',
  fs:{mkdirSync:p=>calls.push(['mkdir',p]),realpathSync:p=>{assert.equal(p,temp);return p;},
   statfsSync:(p,options)=>{assert.equal(p,temp);assert.equal(options.bigint,true);calls.push(['statfs',p]);return{bavail:blocks,bsize:4096n};}},
  json:(p,value)=>records.push({path:p,value}),checkBundle:()=>({pure:true}),
  runner:()=>({rows:[],run:(name)=>{calls.push(['resource',name]);throw firstResource;}}),
 });
 let error;try{prepare();}catch(e){error=e;}
 return{calls,records,error,firstResource};
}

test('insufficient prepare space records actual input and rejects before the first resource',()=>{
 const r=attempt(16n*1024n**3n/4096n-1n);
 assert.equal(r.error?.message,'SCENE_PREPARE_INSUFFICIENT_DISK','TARGET: disk rejection before resources');
 assert(!r.calls.some(c=>c[0]==='resource'));
 assert.deepEqual(r.calls.map(c=>c[0]),['mkdir','statfs']);
 assert.equal(r.records.length,1);
 const record=r.records[0];assert.equal(record.path,out+'/disk-admission.json');
 assert.equal(record.value.path,temp);assert.equal(record.value.availableBytes,'17179865088');
 assert.equal(record.value.requiredBytes,'17179869184');assert(Number.isFinite(Date.parse(record.value.observedAt)));
});

test('exactly 16GiB admits the unchanged first resource after recording the measurement',()=>{
 const r=attempt(16n*1024n**3n/4096n);
 assert.equal(r.error,r.firstResource,'independent control reaches the original first resource');
 assert.deepEqual(r.calls.map(c=>c[0]),['mkdir','statfs','resource']);
 assert.equal(r.calls.at(-1)[1],'fixed-fetch');
 const record=r.records.find(x=>x.path===out+'/disk-admission.json');assert(record);
 assert.equal(record.value.availableBytes,record.value.requiredBytes);
 assert(Number.isFinite(Date.parse(record.value.observedAt)));
});
