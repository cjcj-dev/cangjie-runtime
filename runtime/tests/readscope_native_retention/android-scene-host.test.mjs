#!/usr/bin/env zx
// Actual prepare and Grok probe functions, sealed text/file inputs, no tools.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { createHash } from 'node:crypto';

const here=path.dirname(new URL(import.meta.url).pathname);
const bundle=path.join(here,'android-scene');
const source=fs.readFileSync(process.env.SCENE_HOST_SOURCE||path.join(here,'android-scene.mjs'),'utf8');
const fixed=JSON.parse(fs.readFileSync(path.join(bundle,'input.json')));
const begin=source.indexOf('function assertHostDependencies(');
const end=source.indexOf('\nasync function execute()',begin);
assert(begin>=0&&end>begin);
const missing='\tlibpython3.11.so.1.0 => not found\n';
const resolved='libpython3.11.so.1.0 => /sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/linux-x86_64/python3/lib/libpython3.11.so.1.0\n';
function attempt({stdout,stderr='',probeFailure=false,outcome='HOST_READY',missingRecord=false}={}){
 const calls=[],nextResource=new Error('PURE_FIXTURE_DEBUGGER_RECORD');
 const elf=Buffer.alloc(64);elf.write('7f454c46',0,'hex');elf[4]=2;elf[5]=1;elf.writeUInt16LE(62,18);
 const out='/owned/readscope-android-scene',tools=`/sdk/ndk/${fixed.ndk}/toolchains/llvm/prebuilt/linux-x86_64`;
 const prepare=vm.runInNewContext(source.slice(begin,end)+'\nprepare',{
  path,assert,Date,fixed,here,bundle,process:{env:{RUNNER_TEMP:'/owned'},execPath:process.execPath},
  context:()=>({out,sdk:'/sdk',env:{LD_LIBRARY_PATH:'/prior/lib'}}),checkBundle:()=>({}),files:()=>[],sha:()=>fixed.runtimeSha256,
  json:p=>{if(p.endsWith('/debugger-tools.json'))throw nextResource;},
  fs:{mkdirSync:()=>{},realpathSync:p=>p,statfsSync:()=>({bavail:16n*1024n**3n,bsize:1n}),
   lstatSync:()=>({isDirectory:()=>true,isFile:()=>true,isSymbolicLink:()=>false}),readdirSync:()=>[],statSync:()=>({isFile:()=>true}),
   readFileSync:p=>{
    if(p.endsWith('/host-check.json')){if(missingRecord)throw new Error('ENOENT host-check.json');return JSON.stringify({outcome,meta:{remoteAndroid:true,launcher:tools+'/bin/lldb.sh'},log:[{step:'host-version',rc:0,stdout:'fixture-version'}]});}
    return p.endsWith('/identity.json')?JSON.stringify({ref:fixed.runtime,ndkVersion:fixed.ndk,built:[{arch:'x86_64',runtime:{sha256:fixed.runtimeSha256}}]}):
     p.endsWith('/source.properties')?`Pkg.Revision = ${fixed.ndk}`:p.endsWith('/host-lldb-dependencies.stderr')?stderr:elf;
   }},
  runner:()=>({rows:[],run:(name,args)=>{calls.push({name,args:Array.from(args)});
   if(name==='fixed-source')return fixed.runtime;
   if(name==='blob-elf-fixture.cpp')return fixed.fixtureBlob;
   if(name==='blob-elf-consumer.cpp')return fixed.consumerBlob;
   if(name==='host-check'&&probeFailure)throw new Error('host-check rc1');
   if(name==='host-lldb-dependencies')return stdout??(args[0]==='env'?resolved:missing);
   return '';
  }}),
 });
 let error;try{prepare();}catch(e){error=e;}
 return{error,nextResource,calls,tools};
}
test('launcher environment closes packaged Python before capability probe',()=>{
 const r=attempt();
 assert.equal(r.error,r.nextResource,'TARGET: formal prepare must accept packaged launcher closure and reach its host-check');
 assert.deepEqual(r.calls.find(c=>c.name==='host-lldb-dependencies').args,
  ['env',`PYTHONHOME=${r.tools}/python3`,`LD_LIBRARY_PATH=${r.tools}/python3/lib:/prior/lib`,'ldd',`${r.tools}/bin/lldb`]);
 assert.deepEqual(r.calls.at(-1),{name:'host-check',args:[process.execPath,path.join(bundle,'scene.mjs'),'host-check']});
 assert(!r.calls.some(c=>c.name==='fixture-link'||c.name==='create-avd'));
});
test('unresolved Python under the actual launcher environment is rejected',()=>{
 const r=attempt({stdout:missing});assert.equal(r.error?.message,'NDK_HOST_DEPENDENCY_MISSING');
 assert.equal(r.calls.at(-1).name,'host-lldb-dependencies');
});
test('other unresolved soname is not exempted',()=>{
 const r=attempt({stdout:resolved+'libunrelated.so => not found\n'});
 assert.equal(r.error?.message,'NDK_HOST_DEPENDENCY_MISSING');
 assert.equal(r.calls.at(-1).name,'host-lldb-dependencies');
});
test('loader error on stderr rejects before capability probe',()=>{
 const r=attempt({stdout:resolved,stderr:'lldb: error while loading shared libraries: libother.so: cannot open shared object file'});
 assert.equal(r.error?.message,'NDK_HOST_DEPENDENCY_MISSING');
 assert.equal(r.calls.at(-1).name,'host-lldb-dependencies');
});
test('host-check child failure prevents later prepare resources and AVD',()=>{
 const r=attempt({probeFailure:true});assert.equal(r.error?.message,'host-check rc1');
 assert.equal(r.calls.at(-1).name,'host-check');
});
test('rc0 cannot replace structured host capability result',()=>{
 const r=attempt({outcome:'PLATFORM_ABSENT'});
 assert.match(r.error?.message||'',/Grok host-check must establish tool capability/);
 assert.equal(r.calls.at(-1).name,'host-check');
});
test('missing host-check record does not grant capability',()=>{
 const r=attempt({missingRecord:true});assert.equal(r.error?.message,'ENOENT host-check.json');
 assert.equal(r.calls.at(-1).name,'host-check');
});
test('fixed Grok capability probe uses lldb.sh for both queries and real launch route',async()=>{
 const grok=fs.readFileSync(path.join(bundle,'scene.mjs'),'utf8');
 const part=(start,end)=>grok.slice(grok.indexOf(start),grok.indexOf(end,grok.indexOf(start)));
 const script=part('const NDK =','const REMOTE =')+
  part('const PYTHON_SONAME =','function fail(')+
  part('function missingSonames(','function launchArgv(')+
  part('async function probeHost(','async function drive(')+'\n({probeHost, OFFICIAL_LAUNCHER})';
 const {probeHost,OFFICIAL_LAUNCHER}=vm.runInNewContext(script,{join:path.join,sha256:text=>createHash('sha256').update(text).digest('hex')});
 const calls=[];
 const result=await probeHost({stat:()=>true,readText:()=>OFFICIAL_LAUNCHER},'/sdk',async(step,args)=>{
  calls.push({step,args:Array.from(args)});
  return{rc:0,timedOut:false,stdout:step==='host-ldd'?missing:step==='platform-list'?'remote-android\n':'fixture-version',stderr:''};
 });
 assert.equal(result.outcome,null);assert.equal(calls.length,3);
 const launcher=`/sdk/ndk/${fixed.ndk}/toolchains/llvm/prebuilt/linux-x86_64/bin/lldb.sh`;
 assert.equal(calls[1].args[0],launcher);assert.equal(calls[2].args[0],launcher);
 assert(grok.includes("launchArgv(join(sdk, TOOLS.hostWrapper))"));
 assert(!calls.some(c=>c.args.some(x=>/adb|emulator|lldb-server/.test(x))));
});
