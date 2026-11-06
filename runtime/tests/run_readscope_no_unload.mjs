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
environment.LD_LIBRARY_PATH=[runtime,sdk+'/runtime/lib/linux_x86_64_cjnative',sdk+'/tools/lib'].join(':');
const results=[];
function run(name,exe,args,env=environment,expectedFailure=false) {
  const start=new Date().toISOString();
  const r=spawnSync(exe,args,{env,encoding:'utf8',timeout:120000,maxBuffer:16*1024*1024});
  fs.writeFileSync(`${out}/${name}.stdout`,r.stdout??'');
  fs.writeFileSync(`${out}/${name}.stderr`,r.stderr??'');
  const target=(r.stdout??'').includes('TARGET direct-close-');
  const guard=(r.stderr??'').includes('ELF unload attempted in no-unload mode')||(r.stdout??'').includes('ELF unload attempted in no-unload mode');
  const pass=expectedFailure?target&&guard&&(r.signal==='SIGABRT'||r.status===134):r.status===0;
  results.push({name,exe,args,CJ_READSCOPE_FAST:env.CJ_READSCOPE_FAST??'unset',start,end:new Date().toISOString(),status:r.status,signal:r.signal,error:r.error?.message,expectedFailure,target,guard,pass});
  fs.writeFileSync(out+'/results.json',JSON.stringify(results,null,2)+'\n');
  console.log(name,JSON.stringify(results.at(-1)));
  if(!pass){console.log(r.stdout,r.stderr);process.exit(1);}
  return r.stdout;
}
run('compile-plugin',sdk+'/bin/cjc',[source+'/readscope_no_unload_plugin.cj','--output-type=dylib','-o',out+'/libreadscopeprobe.so']);
const symbols=run('plugin-symbols','nm',['-D','--defined-only',out+'/libreadscopeprobe.so']);
const symbol=symbols.split('\n').find(s=>/readScopeValue/.test(s))?.trim().split(/\s+/).at(-1);
if(!symbol)throw Error('managed symbol missing');
run('compile-harness',process.env.CXX??'clang++',['-std=c++17','-O2','-pthread','-I'+source+'/../src','-I'+source+'/../include',source+'/readscope_no_unload_harness.cpp','-L'+runtime,'-Wl,-rpath,'+runtime,'-lcangjie-runtime','-lboundscheck','-ldl','-o',out+'/harness']);
for(const value of [undefined,'0','1','10']) {
  const env={...environment};if(value!==undefined)env.CJ_READSCOPE_FAST=value;
  run('lifetime-'+(value??'unset'),out+'/harness',['lifetime',out+'/libreadscopeprobe.so',symbol,value==='1'?'1':'0'],env);
}
for(const mode of ['direct','preinit-direct']) {
  for(const value of ['0','1'])run(mode+'-'+value,out+'/harness',[mode,out+'/libreadscopeprobe.so',symbol,value],{...environment,CJ_READSCOPE_FAST:value},value==='1');
}
