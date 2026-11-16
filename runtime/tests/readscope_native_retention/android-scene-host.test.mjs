#!/usr/bin/env zx
// Exercise the actual prepare call site with sealed text/file inputs, no tools.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';

const here=path.dirname(new URL(import.meta.url).pathname);
const source=fs.readFileSync(process.env.SCENE_HOST_SOURCE||path.join(here,'android-scene.mjs'),'utf8');
const fixed=JSON.parse(fs.readFileSync(path.join(here,'android-scene/input.json')));
const begin=source.indexOf('function assertHostDependencies(')>=0
 ?source.indexOf('function assertHostDependencies('):source.indexOf('function prepareBoundsDirectory(');
const end=source.indexOf('\nasync function execute()',begin);
assert(begin>=0&&end>begin);
function attempt(stdout,stderr){
 const calls=[],nextResource=new Error('PURE_FIXTURE_DEBUGGER_RECORD');
 const elf=Buffer.alloc(64);elf.write('7f454c46',0,'hex');elf[4]=2;elf[5]=1;elf.writeUInt16LE(62,18);
 const out='/owned/readscope-android-scene';
 const prepare=vm.runInNewContext(source.slice(begin,end)+'\nprepare',{
  path,assert,Date,fixed,here,process:{env:{RUNNER_TEMP:'/owned'}},
  context:()=>({out,sdk:'/sdk',env:{}}),checkBundle:()=>({}),files:()=>[],sha:()=>fixed.runtimeSha256,
  json:(p)=>{if(p.endsWith('/debugger-tools.json'))throw nextResource;},
  fs:{mkdirSync:()=>{},realpathSync:p=>p,statfsSync:()=>({bavail:16n*1024n**3n,bsize:1n}),
   lstatSync:()=>({isDirectory:()=>true,isSymbolicLink:()=>false}),readdirSync:()=>[],statSync:()=>({isFile:()=>true}),
   readFileSync:p=>p.endsWith('/identity.json')?JSON.stringify({ref:fixed.runtime,ndkVersion:fixed.ndk,built:[{arch:'x86_64',runtime:{sha256:fixed.runtimeSha256}}]}):
    p.endsWith('/source.properties')?`Pkg.Revision = ${fixed.ndk}`:p.endsWith('/host-lldb-dependencies.stderr')?stderr:elf},
  runner:()=>({rows:[],run:(name,args)=>{calls.push({name,args:Array.from(args)});
   if(name==='fixed-source')return fixed.runtime;
   if(name==='blob-elf-fixture.cpp')return fixed.fixtureBlob;
   if(name==='blob-elf-consumer.cpp')return fixed.consumerBlob;
   return name==='host-lldb-dependencies'?stdout:'';
  }}),
 });
 let error;try{prepare();}catch(e){error=e;}
 return{error,nextResource,calls};
}
const observedMissing=process.env.SCENE_HOST_LDD_RECORD
 ?fs.readFileSync(process.env.SCENE_HOST_LDD_RECORD,'utf8'):'\tlibpython3.11.so.1.0 => not found\n';
test('ldd rc0 with the observed missing library is rejected at the formal prepare call site',()=>{
 const r=attempt(observedMissing,'');
 assert.equal(r.error?.message,'NDK_HOST_DEPENDENCY_MISSING','TARGET: reject missing host dependency before later resources');
 assert.equal(r.calls.at(-1).name,'host-lldb-dependencies');
});
test('dependency loader error on stderr also rejects before later resources',()=>{
 const r=attempt('liblldb.so => /ndk/lib/liblldb.so\n','lldb: error while loading shared libraries: libpython3.11.so.1.0: cannot open shared object file\n');
 assert.equal(r.error?.message,'NDK_HOST_DEPENDENCY_MISSING');
 assert.equal(r.calls.at(-1).name,'host-lldb-dependencies');
});
test('resolved ldd output continues past the same formal call site',()=>{
 const r=attempt('liblldb.so => /ndk/lib/liblldb.so\nlibpython3.11.so.1.0 => /ndk/python3/lib/libpython3.11.so.1.0\n','');
 assert.equal(r.error,r.nextResource);
 assert.equal(r.calls.at(-1).name,'host-lldb-dependencies');
});
