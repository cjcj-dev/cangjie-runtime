#!/usr/bin/env zx
// The real prepare body uses real filesystem entities. No Git/tool is spawned.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';

const here=path.dirname(new URL(import.meta.url).pathname);
const source=fs.readFileSync(process.env.SCENE_BOUNDS_SOURCE||path.join(here,'android-scene.mjs'),'utf8');
const fixed=JSON.parse(fs.readFileSync(path.join(here,'android-scene/input.json')));
const start=source.indexOf('function prepareBoundsDirectory(')>=0
 ? source.indexOf('function prepareBoundsDirectory('):source.indexOf('function prepare() {');
const end=source.indexOf('\nasync function execute()',start);
assert(start>=0&&end>start);
const fixtureRoot=process.env.SCENE_BOUNDS_TEST_ROOT;
assert(fixtureRoot&&path.isAbsolute(fixtureRoot),'explicit owned scratch directory required');
fs.mkdirSync(fixtureRoot,{recursive:true});

function attempt(t,kind){
 const root=fs.mkdtempSync(path.join(fixtureRoot,'bounds-'));
 t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const out=path.join(root,'readscope-android-scene');
 const bounds=path.join(out,'fixed-runtime/runtime/third_party/third_party_bounds_checking_function');
 const external=path.join(root,'external');
 const calls=[],nextResource=new Error('PURE_FIXTURE_ARTIFACT_CHECK');
 const prepare=vm.runInNewContext(source.slice(start,end)+'\nprepare',{
  fs:{...fs,statfsSync:()=>({bavail:16n*1024n**3n,bsize:1n})},path,assert,Date,fixed,here,
  process:{env:{RUNNER_TEMP:root}},context:()=>({out,sdk:'/unused-sdk',env:{}}),
  json:()=>{},checkBundle:()=>({}),files:()=>{throw nextResource;},
  runner:()=>({rows:[],run:(name,args)=>{
   calls.push({name,args:Array.from(args)});
   if(name==='fixed-worktree'){
    fs.mkdirSync(path.dirname(bounds),{recursive:true});
    if(kind==='empty'||kind==='nonempty')fs.mkdirSync(bounds);
    if(kind==='nonempty')fs.writeFileSync(path.join(bounds,'keep'),'original');
    if(kind==='file')fs.writeFileSync(bounds,'original');
    if(kind==='symlink'){fs.mkdirSync(external);fs.symlinkSync(external,bounds);}
    if(kind==='dangling')fs.symlinkSync(external,bounds);
   }
   if(name==='fixed-source')return fixed.runtime;
   if(name==='blob-elf-fixture.cpp')return fixed.fixtureBlob;
   if(name==='blob-elf-consumer.cpp')return fixed.consumerBlob;
   return '';
  }}),
 });
 let error;try{prepare();}catch(e){error=e;}
 return{bounds,external,calls,error,nextResource};
}

for(const kind of ['fresh','empty'])test(`${kind} bounds directory reaches the unchanged fixed dependency producer`,t=>{
 const r=attempt(t,kind);
 assert.equal(r.error,r.nextResource,'TARGET: directory preparation reaches the original producer');
 assert(fs.lstatSync(r.bounds).isDirectory());assert.deepEqual(fs.readdirSync(r.bounds),[]);
 assert.deepEqual(r.calls.filter(x=>x.name.startsWith('bounds-')),[
  {name:'bounds-init',args:['git','init',r.bounds]},
  {name:'bounds-remote',args:['git','-C',r.bounds,'remote','add','origin','https://gitcode.com/openharmony/third_party_bounds_checking_function.git']},
  {name:'bounds-fetch',args:['git','-C',r.bounds,'fetch','--depth','1','origin',fixed.bounds]},
  {name:'bounds-checkout',args:['git','-C',r.bounds,'checkout','--detach',fixed.bounds]},
 ]);
});

for(const kind of ['nonempty','file','symlink','dangling'])test(`${kind} bounds input is rejected without changing its contents or target`,t=>{
 const r=attempt(t,kind);
 assert(r.error&&r.error!==r.nextResource,'TARGET: illegal dependency rejected');
 assert(!r.calls.some(x=>x.name.startsWith('bounds-')),'no dependency producer on rejected entity');
 if(kind==='nonempty')assert.equal(fs.readFileSync(path.join(r.bounds,'keep'),'utf8'),'original');
 if(kind==='file')assert.equal(fs.readFileSync(r.bounds,'utf8'),'original');
 if(kind==='symlink'||kind==='dangling'){
  assert(fs.lstatSync(r.bounds).isSymbolicLink());assert.equal(fs.readlinkSync(r.bounds),r.external);
  if(kind==='symlink')assert.deepEqual(fs.readdirSync(r.external),[]);
  else assert.equal(fs.existsSync(r.external),false);
 }
});
