#!/usr/bin/env zx
// Official native macOS runner only; iOS requires its own target execution.
import fs from 'node:fs';import path from 'node:path';import os from 'node:os';import assert from 'node:assert/strict';import {fileURLToPath} from 'node:url';import {spawnSync} from 'node:child_process';import {createHash} from 'node:crypto';
assert.equal(process.platform,'darwin');
const here=path.dirname(fileURLToPath(import.meta.url)),runtime=path.resolve(here,'../..'),out=path.resolve(process.env.RUNNER_TEMP,'readscope-apple'),build=path.join(out,'build'),install=path.join(out,'install');fs.mkdirSync(out,{recursive:false});
const env={...process.env,CANGJIE_BUILD_JOBS:'2',CMAKE_BUILD_PARALLEL_LEVEL:'2',GC_UNIT_GATE_SKIP:'1',SCCACHE_GHA_ENABLED:'true'};
const rows=[],sha=p=>createHash('sha256').update(fs.readFileSync(p)).digest('hex');
function run(name,args,extra={},expected=0){const row={name,args,cwd:out,started:new Date().toISOString(),expected};rows.push(row);const fd=fs.openSync(path.join(out,name+'.stdout'),'wx'),err=fs.openSync(path.join(out,name+'.stderr'),'wx');const r=spawnSync(args[0],args.slice(1),{cwd:out,env:{...env,...extra},stdio:['ignore',fd,err],timeout:2700000});fs.closeSync(fd);fs.closeSync(err);Object.assign(row,{rc:r.status,signal:r.signal,error:r.error?.message,ended:new Date().toISOString()});fs.writeFileSync(path.join(out,'commands.json'),JSON.stringify(rows,null,2));console.log(name,row.rc,row.signal);if(expected!==null){assert.equal(r.status,expected,name);assert.equal(r.signal,null);}return fs.readFileSync(path.join(out,name+'.stdout'),'utf8');}

const ref=run('source-ref',['git','-C',path.dirname(runtime),'rev-parse','HEAD']).trim();
run('configure',['cmake','-S',runtime,'-B',build,'-G','Ninja','-DMACOS_FLAG=1','-DWINDOWS_FLAG=0','-DOHOS_FLAG=0','-DANDROID_FLAG=0','-DCOPYGC_FLAG=1','-DDOPRA_FLAG=1','-DCMAKE_BUILD_TYPE=Release','-DRUNTIME_TRACE_FLAG=1','-DCJ_SDK_VERSION=0.0.1','-DDISABLE_VERSION_CHECK=1','-DCMAKE_C_COMPILER=clang','-DCMAKE_CXX_COMPILER=clang++','-DCMAKE_AR_PATH=ar','-DCMAKE_C_COMPILER_LAUNCHER=sccache','-DCMAKE_CXX_COMPILER_LAUNCHER=sccache','-DCJ_RUNTIME_COMMIT='+ref,'-DMRT_GC_UNIT_TESTS=OFF','-DCMAKE_INSTALL_PREFIX='+install]);
run('build',['cmake','--build',build,'--parallel','2']);run('install',['cmake','--install',build]);
function files(root){return fs.readdirSync(root,{withFileTypes:true}).flatMap(e=>e.isDirectory()?files(path.join(root,e.name)):[path.join(root,e.name)]);}
const libraries=files(install).filter(p=>/libcangjie-runtime\.dylib$/.test(p));assert.equal(libraries.length,1);const lib=path.dirname(libraries[0]);
const headers=[runtime+'/src',runtime+'/src/Loader/BinaryFile',runtime+'/include',runtime+'/src/CJThread/src/runtime/schedule/include',runtime+'/third_party/third_party_bounds_checking_function/include',runtime+'/src/os/Macos',...files(build).filter(p=>p.endsWith(path.sep+'log.h')).map(p=>path.dirname(p))];
const common=['clang++','-std=gnu++17','-O2',...headers.map(p=>'-I'+p)],dll=path.join(out,'readscope-fixture.dylib'),exe=path.join(out,'readscope-consumer');
run('fixture-link',[...common,'-dynamiclib',path.join(here,'apple-fixture.cpp'),'-L'+lib,'-lcangjie-runtime','-Wl,-rpath,'+lib,'-o',dll]);
run('consumer-link',[...common,path.join(here,'apple-consumer.cpp'),'-L'+lib,'-lcangjie-runtime','-Wl,-rpath,'+lib,'-o',exe]);
fs.writeFileSync(path.join(out,'identity.json'),JSON.stringify({ref,platform:process.platform,arch:process.arch,host:os.version(),runtime:{path:libraries[0],sha256:sha(libraries[0])},fixture:{path:dll,sha256:sha(dll)},consumer:{path:exe,sha256:sha(exe)},performance:'NOT_MEASURED',scope:'native macOS constructor -> product registration/public close; iOS native execution NOT_RUN'},null,2));
const cases=[];
for(const mode of ['preinit','runtime-fini']){
 const output=run(mode,[exe,dll,mode],{DYLD_LIBRARY_PATH:lib,DYLD_PRINT_LIBRARIES:'1'},null),row=rows.at(-1),stderr=fs.readFileSync(path.join(out,mode+'.stderr'),'utf8');
 const initCount=(stderr.match(/APPLE_FIXTURE_INIT count=1/g)||[]).length,finiCount=(stderr.match(/APPLE_FIXTURE_FINI count=1/g)||[]).length;
 const pass=row.rc===0&&!row.signal&&!row.error&&output.includes('apple-reopen-no-double-init executed=1 pass=1')&&initCount===1&&finiCount===1;
 cases.push({mode,pass,rc:row.rc,signal:row.signal,initCount,finiCount});fs.writeFileSync(path.join(out,'lifecycle-summary.json'),JSON.stringify(cases,null,2));
}
process.exitCode=cases.every(c=>c.pass)?0:1;
