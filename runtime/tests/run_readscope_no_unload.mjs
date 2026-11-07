#!/usr/bin/env zx
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
const [sdk, runtime, out]=process.argv.slice(2);
if(!sdk||!runtime||!out)throw Error('usage: run_readscope_no_unload.mjs SDK RUNTIME_LIB_DIR NEW_OUTPUT');
const source=path.dirname(fileURLToPath(import.meta.url));
fs.mkdirSync(out,{recursive:false});
const environment={...process.env,CANGJIE_HOME:sdk,cjEnableGC:'1',cjHeapSize:'1GB',TMPDIR:out};
for(const k of ['CJ_READSCOPE_FAST','LD_PRELOAD','LD_AUDIT','CANGJIE_PATH'])delete environment[k];
environment.LD_LIBRARY_PATH=[runtime,sdk+'/runtime/lib/linux_x86_64_cjnative',sdk+'/lib/linux_x86_64_cjnative',sdk+'/tools/lib'].join(':');
const results=[];
let collectFailures=false;
function run(name,exe,args,env=environment,expect={}) {
  const start=new Date().toISOString();
  const r=spawnSync(exe,args,{env,cwd:out,encoding:'utf8',timeout:120000,maxBuffer:16*1024*1024});
  fs.writeFileSync(`${out}/${name}.stdout`,r.stdout??'');
  fs.writeFileSync(`${out}/${name}.stderr`,r.stderr??'');
  const combined=(r.stdout??'')+(r.stderr??'');
  const assertions=(r.stdout??'').split('\n').filter(s=>s.startsWith('ASSERT ')||s.startsWith('TARGET '));
  let pass=r.status===0;
  if(expect.pinFailure)pass=combined.includes('TARGET native-pin-failure')&&combined.includes('ELF no-unload pin: NODELETE promotion failed')&&(r.signal==='SIGABRT'||r.status===134);
  let fini;
  if(expect.order){
    // The fixed native fixtures produce AB and BA fini order respectively.
    // Require both callbacks and both orders, as in the frozen 9b evidence.
    const first=combined.indexOf('calling fini: '+out+'/libreadscopeprobe.so ');
    const second=combined.indexOf('calling fini: '+out+'/libreadscopeorder.so ');
    fini={first,second};pass&&=first>=0&&second>=0&&(expect.order==='ab'?first<second:second<first);
  }
  if(expect.output)pass&&=(r.stdout??'').includes(expect.output);
  results.push({name,exe,args,CJ_READSCOPE_FAST:env.CJ_READSCOPE_FAST??'unset',start,end:new Date().toISOString(),status:r.status,signal:r.signal,error:r.error?.message,expect,assertions,fini,pass});
  fs.writeFileSync(out+'/results.json',JSON.stringify(results,null,2)+'\n');
  console.log(JSON.stringify(results.at(-1)));
  if(!pass){console.log(r.stdout,r.stderr);if(!collectFailures)process.exit(1);}
  return r.stdout;
}
const runtimePaths=[runtime+'/libcangjie-runtime.so',sdk+'/runtime/lib/linux_x86_64_cjnative/libcangjie-runtime.so',sdk+'/.compiler-exec/runtime/lib/linux_x86_64_cjnative/libcangjie-runtime.so'];
const identities=run('installed-runtime-identity','sha256sum',[...runtimePaths,sdk+'/bin/cjc']);
if(new Set(identities.trim().split('\n').slice(0,3).map(line=>line.split(/\s+/)[0])).size!==1)
  throw Error('compiler host and target installations must consume the candidate runtime');
const plugin=out+'/libreadscopeprobe.so';
// Real normal compiler entry, with the removed selector absent.
run('compile-plugin',sdk+'/bin/cjc',[source+'/readscope_no_unload_plugin.cj','--output-type=dylib','-o',plugin],{...environment,LD_DEBUG:'libs'});
const compilerLoad=fs.readFileSync(out+'/compile-plugin.stderr','utf8');
if(!compilerLoad.includes('calling init: '+runtimePaths[2]))throw Error('normal compiler did not load its installed candidate runtime');
const symbols=run('plugin-symbols','nm',['-D','--defined-only',plugin]);
const symbol=symbols.split('\n').find(s=>/readScopeValue/.test(s))?.trim().split(/\s+/).at(-1);
if(!symbol)throw Error('managed symbol missing');
fs.writeFileSync(out+'/ordinary.cj','main(): Int64 { println("readscope-default-normal-entry"); return 0 }\n');
run('compile-ordinary',sdk+'/bin/cjc',[out+'/ordinary.cj','-o',out+'/ordinary'],{...environment,LD_DEBUG:'libs'});
run('ordinary-default',out+'/ordinary',[],environment,{output:'readscope-default-normal-entry'});
const flags=['-std=c++17','-O2','-pthread','-I'+source+'/../src','-I'+source+'/../include'];
for(const [name,file,linked] of [['harness','readscope_no_unload_harness.cpp',true],['first','readscope_first_load_harness.cpp',true],['reopen','readscope_public_reopen_harness.cpp',true],['pinned','readscope_pinned_image_harness.cpp',false]]){
  run('compile-'+name,process.env.CXX??'clang++',[...flags,source+'/'+file,...(linked?['-L'+runtime,'-Wl,-rpath,'+runtime,'-lcangjie-runtime','-lboundscheck']:[]),'-ldl','-o',out+'/'+name]);
}
run('compile-pin-failure',process.env.CXX??'clang++',['-shared','-fPIC',source+'/readscope_pin_failure.cpp','-ldl','-o',out+'/pin-failure.so']);
fs.mkdirSync(out+'/distinct');fs.copyFileSync(plugin,out+'/distinct/libreadscopeprobe.so');
fs.writeFileSync(out+'/readscopeorder.cj',fs.readFileSync(source+'/readscope_no_unload_plugin.cj','utf8').replace('package readscopeprobe','package readscopeorder'));
run('compile-companion',sdk+'/bin/cjc',[out+'/readscopeorder.cj','--output-type=dylib','-o',out+'/libreadscopeorder.so']);
// Complete independent behavioral cases while retaining a nonzero suite result.
collectFailures=true;
for(const value of [undefined,'0','1','10']) {
  const env={...environment};if(value!==undefined)env.CJ_READSCOPE_FAST=value;
  const suffix=value??'unset';
  run('public-reopen-'+suffix,out+'/reopen',[plugin,symbol],env);
  for(const mode of ['lifetime','direct','preinit-direct','retained'])run(mode+'-'+suffix,out+'/harness',[mode,plugin,symbol],env);
  for(const mode of ['exit','missing','distinct','public-freeze','native-freeze','live-exit-retained','live-exit-metadata','live-exit-order-ab','live-exit-order-ba']){
    const extra=mode==='distinct'?out+'/distinct/libreadscopeprobe.so':mode.startsWith('live-exit')?out+'/libreadscopeorder.so':out+'/missing/libreadscopeprobe.so';
    const order=mode.includes('order-')?mode.slice(-2):undefined;
    run(mode+'-'+suffix,out+'/first',[mode,plugin,symbol,extra],{...env,LD_DEBUG:'libs'},{order});
  }
  for(const namespace of ['base','namespace'])for(const lifecycle of ['never','after','live','postfini','live-exit','public-exit','public-live-exit'])
    run('pin-'+namespace+'-'+lifecycle+'-'+suffix,out+'/pinned',[runtime+'/libcangjie-runtime.so',plugin,symbol,namespace,lifecycle],env);
  // Cut the actual native promotion invoked by the public producer. Independent
  // native-only input proves this injection does not reject every dlopen.
  run('pin-failure-'+suffix,out+'/pinned',[runtime+'/libcangjie-runtime.so',plugin,symbol,'base','public-exit'],{...env,LD_PRELOAD:out+'/pin-failure.so'},{pinFailure:true});
  run('pin-restored-'+suffix,out+'/pinned',[runtime+'/libcangjie-runtime.so',plugin,symbol,'base','public-exit'],env);
}
fs.writeFileSync(out+'/cut.diff',`--- normal-public-exit environment\n+++ pin-failure environment\n@@ -1 +1 @@\n-LD_PRELOAD unset\n+LD_PRELOAD=${out}/pin-failure.so\n`);
fs.writeFileSync(out+'/native-control.cpp','#include <dlfcn.h>\nint main(){auto h=dlopen("libm.so.6",RTLD_NOW);return h?dlclose(h):1;}\n');
collectFailures=false;
run('compile-native-control',process.env.CXX??'clang++',[out+'/native-control.cpp','-ldl','-o',out+'/native-control']);
collectFailures=true;
run('pin-cut-independent-control',out+'/native-control',[],{...environment,LD_PRELOAD:out+'/pin-failure.so'});
fs.writeFileSync(out+'/summary.json',JSON.stringify({processes:results.length,failed:results.filter(r=>!r.pass).map(r=>r.name)},null,2)+'\n');
process.exitCode=results.some(r=>!r.pass)?1:0;
