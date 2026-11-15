#!/usr/bin/env zx
// Native Windows product/constructor test. No model/backend or SDK dependency.
import fs from 'node:fs';import path from 'node:path';import os from 'node:os';import assert from 'node:assert/strict';import {fileURLToPath} from 'node:url';import {spawnSync} from 'node:child_process';import {createHash} from 'node:crypto';
assert.equal(process.platform,'win32','native Windows execution is required');
const here=path.dirname(fileURLToPath(import.meta.url)),runtime=path.resolve(here,'../..'),out=path.resolve(process.env.RUNNER_TEMP,'readscope-native'),build=path.join(out,'build'),install=path.join(out,'install');fs.mkdirSync(out,{recursive:false});
const clang64=path.resolve(process.env.READSCOPE_CLANG64),tool=name=>path.join(clang64,name+'.exe');
// Normal COFF archive installation requires GNU BFD, not LLVM objcopy's
// unsupported COFF rename operation. Keep compiler selection absolute.
const binutils=path.resolve(clang64,'../../mingw64/bin'),bfd=name=>path.join(binutils,name+'.exe');
for(const name of ['objcopy','objdump','strip'])assert(fs.existsSync(bfd(name)), 'configured GNU binutils tool missing: '+bfd(name));
for(const name of ['clang','clang++','llvm-ar','ninja'])assert(fs.existsSync(tool(name)), 'configured CLANG64 tool missing: '+tool(name));
// Windows environment keys are case-insensitive. Pass one PATH spelling so an
// inherited Path cannot select the runner's unrelated standalone LLVM first.
const inherited=Object.fromEntries(Object.entries(process.env).filter(([key])=>key.toUpperCase()!=='PATH'));
const env={...inherited,PATH:binutils+path.delimiter+clang64+path.delimiter+process.env.PATH,CANGJIE_BUILD_JOBS:'2',CMAKE_BUILD_PARALLEL_LEVEL:'2',GC_UNIT_GATE_SKIP:'1',SCCACHE_GHA_ENABLED:'true'};
const rows=[],sha=p=>createHash('sha256').update(fs.readFileSync(p)).digest('hex');
function run(name,args,extra={},expected=0){const row={name,args,cwd:out,started:new Date().toISOString(),expected};rows.push(row);const fd=fs.openSync(path.join(out,name+'.stdout'),'wx'),err=fs.openSync(path.join(out,name+'.stderr'),'wx');const r=spawnSync(args[0],args.slice(1),{cwd:out,env:{...env,...extra},stdio:['ignore',fd,err],timeout:2700000});fs.closeSync(fd);fs.closeSync(err);Object.assign(row,{rc:r.status,signal:r.signal,error:r.error?.message,ended:new Date().toISOString()});fs.writeFileSync(path.join(out,'commands.json'),JSON.stringify(rows,null,2));console.log(name,row.rc,row.signal);if(expected!==null){assert.equal(r.status,expected,name);assert.equal(r.signal,null);}return fs.readFileSync(path.join(out,name+'.stdout'),'utf8');}
run('compiler-version',[tool('clang'),'--version']);run('ninja-version',[tool('ninja'),'--version']);
for(const name of ['objcopy','objdump','strip'])run(name+'-version',[bfd(name),'--version']);
const ref=run('source-ref',['git','-C',path.dirname(runtime),'rev-parse','HEAD']).trim();
run('configure',['cmake','-S',runtime,'-B',build,'-G','Ninja','-DWINDOWS_FLAG=1','-DCOPYGC_FLAG=1','-DDOPRA_FLAG=1','-DCMAKE_BUILD_TYPE=Release','-DRUNTIME_TRACE_FLAG=1','-DCJ_SDK_VERSION=0.0.1','-DDISABLE_VERSION_CHECK=1','-DCMAKE_C_COMPILER='+tool('clang'),'-DCMAKE_CXX_COMPILER='+tool('clang++'),'-DCMAKE_ASM_COMPILER='+tool('clang'),'-DCMAKE_AR_PATH='+tool('llvm-ar'),'-DCMAKE_MAKE_PROGRAM='+tool('ninja'),'-DWINDOWS_RUNTIME_OBJCOPY='+bfd('objcopy'),'-DWINDOWS_RUNTIME_OBJDUMP='+bfd('objdump'),'-DCMAKE_C_COMPILER_LAUNCHER='+process.env.SCCACHE_PATH,'-DCMAKE_CXX_COMPILER_LAUNCHER='+process.env.SCCACHE_PATH,'-DCJ_RUNTIME_COMMIT='+ref,'-DMRT_GC_UNIT_TESTS=OFF','-DCMAKE_INSTALL_PREFIX='+install]);
run('build',['cmake','--build',build,'--parallel','2']);run('install',['cmake','--install',build]);
function files(root){return fs.readdirSync(root,{withFileTypes:true}).flatMap(e=>e.isDirectory()?files(path.join(root,e.name)):[path.join(root,e.name)]);}
const libraries=files(install).filter(p=>/cangjie-runtime\.dll$/i.test(p)),imports=files(build).filter(p=>/cangjie-runtime\.dll\.a$/i.test(p));assert.equal(libraries.length,1);assert.equal(imports.length,1);const lib=path.dirname(libraries[0]);
const headers=[runtime+'/src',runtime+'/src/Loader/BinaryFile',runtime+'/include',runtime+'/src/CJThread/src/runtime/schedule/include',runtime+'/third_party/third_party_bounds_checking_function/include',runtime+'/src/os/Windows',...files(build).filter(p=>p.endsWith(path.sep+'log.h')).map(p=>path.dirname(p))];
const common=[tool('clang++'),'-std=gnu++17','-fms-extensions','-O2',...headers.map(p=>'-I'+p)],dll=path.join(out,'readscope-fixture.dll'),exe=path.join(out,'readscope-consumer.exe');
run('fixture-link',[...common,'-shared',path.join(here,'fixture.cpp'),imports[0],'-o',dll]);run('consumer-link',[...common,path.join(here,'consumer.cpp'),imports[0],'-Wl,--export-all-symbols','-o',exe]);
fs.writeFileSync(path.join(out,'identity.json'),JSON.stringify({ref,platform:process.platform,host:os.version(),runtime:{path:libraries[0],sha256:sha(libraries[0])},fixture:{path:dll,sha256:sha(dll)},consumer:{path:exe,sha256:sha(exe)},performance:'NOT_MEASURED',scope:'native Windows constructor -> product metadata registration/public close; no full SDK or language GC qualification'},null,2));
const companion=path.join(out,'readscope-companion.dll');fs.copyFileSync(dll,companion);const cases=[];
for(const mode of ['preinit','runtime-fini','native-failure','native-exception','native-reuse']){
  const output=run(mode,[exe,dll,mode,companion],{PATH:lib+path.delimiter+env.PATH},null),row=rows.at(-1),stderr=fs.readFileSync(path.join(out,mode+'.stderr'),'utf8');
  const initCount=(stderr.match(/READSCOPE_FIXTURE_INIT count=1/g)||[]).length,finiCount=(stderr.match(/READSCOPE_FIXTURE_FINI count=1/g)||[]).length;
  const pass=row.rc===0&&!row.signal&&!row.error&&output.includes('TARGET ')&&initCount>0&&(mode.startsWith('native-')||initCount===1&&finiCount===1);
  cases.push({mode,pass,rc:row.rc,signal:row.signal,initCount,finiCount,exceptionWithoutDetach:mode==='native-exception'?(output.includes('native-exception-observed executed=1 pass=1')&&output.includes('native-exception-no-detach executed=1 pass=1')&&finiCount===0?'OBSERVED':'NOT_EXERCISED'):null,addressReuse:mode==='native-reuse'?(output.includes('same_address=1')?'OBSERVED':'NOT_OBSERVED'):null});
  fs.writeFileSync(path.join(out,'lifecycle-summary.json'),JSON.stringify(cases,null,2));
}
process.exitCode=cases.every(c=>c.pass)?0:1;
