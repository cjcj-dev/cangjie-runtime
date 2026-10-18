#!/usr/bin/env zx
// Public SDK CLI / real stdlib top-level regression gates for review F1–F3.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import {spawn, spawnSync} from 'node:child_process';
import {setTimeout as delay} from 'node:timers/promises';
const [mode, arm='new', variant='first'] = process.argv.slice(2);
const base=process.env.JOBSERVER_REWORK_ROOT;
const R=process.env.JOBSERVER_REWORK_R,T=process.env.JOBSERVER_REWORK_T,sdk=process.env.JOBSERVER_TEST_SDK;
assert(base&&R&&T&&sdk);
const root=path.join(base,`${mode}-${arm}-${variant}`);
const resume=process.env.JOBSERVER_REWORK_RESUME==='1';
assert(resume===fs.existsSync(root),'fresh gate or explicit preparation recovery required');fs.mkdirSync(root,{recursive:true});
const entry=path.join(T,'ci/bootstrap/sdk-build.mjs');
const clean={...process.env,PATH:`${sdk}/bin:${sdk}/tools/bin:/usr/local/bin:/usr/bin:/bin`,CANGJIE_HOME:sdk,
  CJCJ_JOBSERVER_TRACE:'1',CMAKE_BUILD_PARALLEL_LEVEL:'16',CCACHE_DIR:path.join(base,'ccache'),
  LD_LIBRARY_PATH:`${sdk}/runtime/lib/linux_x86_64_cjnative:${sdk}/lib/linux_x86_64_cjnative`};
for(const k of ['CARGO_MAKEFLAGS','MAKEFLAGS','MFLAGS','CJPM_JOBSERVER','CANGJIE_JOBSERVER_ROOT','CANGJIE_JOBSERVER_JOBS','CJCJ_BACKEND_CONCURRENCY','NODE_OPTIONS','PYTHONPATH'])delete clean[k];
const records=resume?JSON.parse(fs.readFileSync(path.join(root,'commands.json'),'utf8')):[];
function run(name,args,{env=clean,cwd=root,...rest}={}) {
  const a=fs.openSync(path.join(root,name+'.stdout'),'w'),b=fs.openSync(path.join(root,name+'.stderr'),'w');
  const started=Date.now(),r=spawnSync(args[0],args.slice(1),{env,cwd,stdio:['ignore',a,b],timeout:300000,...rest});fs.closeSync(a);fs.closeSync(b);
  const record={name,args,rc:r.status,signal:r.signal,error:r.error?.message,wallMs:Date.now()-started};records.push(record);fs.writeFileSync(path.join(root,'commands.json'),JSON.stringify(records,null,2));console.log(JSON.stringify(record));
  return {...r,stdout:fs.readFileSync(path.join(root,name+'.stdout'),'utf8'),stderr:fs.readFileSync(path.join(root,name+'.stderr'),'utf8')};
}
const read=file=>fs.readFileSync(file,'utf8');
const hash=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const events=file=>read(file).trim().split('\n').filter(Boolean).map(JSON.parse);
function readyJson(file){try{return JSON.parse(read(file));}catch(e){if(e.code==='ENOENT'||e instanceof SyntaxError)return null;throw e;}}
function config(name,commands,env={}) {
  const work=path.join(root,name),file=path.join(root,name+'.json');
  fs.writeFileSync(file,JSON.stringify({work,install:path.join(work,'install'),repos:{},finalStage:'finite',env,stages:[{id:'finite',output:path.join(work,'install'),commands}],test:{}},null,2));return file;
}
function fifo(name,bytes='') {
  const file=path.join(root,name+'.fifo');assert.equal(run(name+'-mkfifo',['mkfifo',file]).status,0);
  const fd=fs.openSync(file,fs.constants.O_RDWR|fs.constants.O_NONBLOCK);if(bytes)fs.writeSync(fd,bytes);
  return {file,fd,auth:`--jobserver-auth=fifo:${file} -j4`};
}
function sourceCopy(destination) {
  fs.cpSync(path.join(R,'stdlib'),destination,{recursive:true,filter:p=>!['build','output'].includes(path.basename(p))});
  assert.equal(hash(path.join(destination,'CMakeLists.txt')),hash(path.join(R,'stdlib/CMakeLists.txt')));
  assert.equal(hash(path.join(destination,'build.py')),hash(path.join(R,'stdlib/build.py')));
}
function seed(source,extra='') {
  const build=path.join(source,'build/build');fs.mkdirSync(build,{recursive:true});
  fs.writeFileSync(path.join(build,'CMakeCache.txt'),'CANGJIE_ENABLE_CCACHE:BOOL=ON\n'+extra);return build;
}
function boundedNinja(source) {
  // Only the old build.py has no native-target selector. Keep its complete
  // configure path, then forward to the real Ninja with two permitted targets.
  const bin=path.join(root,'bounded-tools');fs.mkdirSync(bin);
  const f=path.join(bin,'ninja');fs.writeFileSync(f,`#!/usr/bin/env node
import {spawnSync} from 'node:child_process';
const args=process.argv.slice(2);if(process.cwd()===${JSON.stringify(path.join(source,'build/build'))}&&!args.includes('--version'))args.push('boundscheck','boundscheck-static');
const r=spawnSync('/usr/bin/ninja',args,{stdio:'inherit',env:process.env});process.exitCode=r.status??1;
`);fs.chmodSync(f,0o755);return bin;
}
async function f1() {
  const source=path.join(root,'stdlib');if(!resume)sourceCopy(source);const build=resume?path.join(source,'build/build'):seed(source);
  const parent=resume?null:fifo('busy-parent');const log=path.join(root,'budget.jsonl');
  const env={...clean,MAKEFLAGS:parent?.auth,CANGJIE_JOBSERVER_LOG:log};
  const args=['python3',path.join(source,'build.py'),'build','-t','release','--jobs','4'];
  if(arm==='old'&&!resume)env.PATH=boundedNinja(source)+':'+env.PATH;
  else args.push('--build-target','boundscheck','--build-target','boundscheck-static');
  if(!resume) {
    const file=config('flow',[{argv:args}]);const r=run('public-build',['node',entry,'--config',file,'--no-package'],{env});
    assert.equal(r.status,0,r.stderr);assert.match(r.stdout,/CCache found/);
  }
  const prefix=path.join(root,'installed');
  // The top-level owns the install rule; its complete install also needs std.
  // Finite native delivery explicitly copies this actually built target only.
  const library=path.join(prefix,'lib/linux_x86_64_cjnative/libboundscheck.so');
  fs.mkdirSync(path.dirname(library),{recursive:true});fs.copyFileSync(path.join(build,'lib/libboundscheck.so'),library);
  fs.writeFileSync(path.join(root,'finite-install-copy.json'),JSON.stringify({source:path.join(build,'lib/libboundscheck.so'),destination:library,sha256:hash(library)}));
  const control=run(resume?'installed-consumer-recovered':'installed-consumer',['python3','-c',`import ctypes; l=ctypes.CDLL(${JSON.stringify(library)}); b=ctypes.create_string_buffer(4); assert l.memcpy_s(b,4,b"abc",4)==0; assert b.value==b"abc"; print("NATIVE_INSTALLED_42")`]);assert.equal(control.status,0,control.stderr);
  const e=events(log),pool=e.find(x=>x.event==='pool-start');assert.equal(pool.jobs,1);assert.equal(pool.borrowed,0);assert.equal(e.find(x=>x.event==='pool-end').returned,1);
  const commands=e.filter(x=>x.event==='command-start').map(x=>x.command);
  const native=commands.filter(c=>c[0].endsWith('/ccache'));
  fs.writeFileSync(path.join(root,'F1-result.json'),JSON.stringify({pool,commands,native: native.length,installedSha256:hash(library),topLevelSha256:hash(path.join(source,'CMakeLists.txt')),ccacheModuleSha256:hash(path.join(source,'cmake/optional/CangjieCCache.cmake'))},null,2));
  if(parent)fs.closeSync(parent.fd);
  console.log('F1_NATIVE_BUDGET_ASSERTION_REACHED '+JSON.stringify({native:native.length,independentInstalledControl:true}));
  assert(native.length>2,'formal ccache compile/link must retain budget admission');
}
function processRows() {
  const rows=[];
  for(const pid of fs.readdirSync('/proc').filter(n=>/^\d+$/.test(n)))try {
    const text=read('/proc/'+pid+'/stat'),f=text.slice(text.lastIndexOf(')')+2).split(' ');
    rows.push({pid:+pid,ppid:+f[1],group:+f[2],state:f[0],start:f[19]});
  }catch(e){if(!['ENOENT','ESRCH'].includes(e.code))throw e;}
  return rows;
}
function rememberFamily(pid,known) {
  const rows=processRows();let owners=new Set([pid]),changed=true;
  while(changed){changed=false;for(const r of rows)if(owners.has(r.ppid)&&!owners.has(r.pid)){owners.add(r.pid);changed=true;}}
  for(const r of rows)if(owners.has(r.pid))known.set(r.pid,r);
}
const living=known=>processRows().filter(r=>known.has(r.pid)&&known.get(r.pid).start===r.start&&r.state!=='Z');
async function cleanupKnown(known) {
  for(const signal of ['SIGTERM','SIGKILL']) {for(const r of living(known))try{process.kill(r.pid,signal);}catch(e){if(e.code!=='ESRCH')throw e;}await delay(200);}
  assert.equal(living(known).length,0,'test cleans only captured owned PID/starttime pairs');
}
async function f2() {
  const reused=variant.startsWith('reconfigure')||variant==='control';
  const signal=variant.endsWith('-int')||['reconfigure','sdk-race'].includes(variant)?'SIGINT':'SIGTERM';
  let jobs='4';
  const source=reused?path.join(base,`f1-${arm}-first/stdlib`):path.join(root,'stdlib');
  if(!reused)sourceCopy(source);
  const marker=path.join(root,'configure-child.json'),childScript=path.join(root,'configure-child.mjs');
  fs.writeFileSync(childScript,`import fs from 'node:fs';fs.writeFileSync(${JSON.stringify(marker)},JSON.stringify({pid:process.pid,cmake:process.ppid,parallel:process.env.CMAKE_BUILD_PARALLEL_LEVEL}));console.log('ACTUAL_CMAKE_CHILD_REACHED');${variant==='control'?'':'setInterval(()=>{},1000);'}`);
  const hook=path.join(root,'configure-hook.cmake');fs.writeFileSync(hook,`execute_process(COMMAND node "${childScript}" RESULT_VARIABLE status)\nif(NOT status EQUAL 0)\n message(FATAL_ERROR "controlled configure child stopped: \${status}")\nendif()\n`);
  if(!reused)seed(source,`CMAKE_PROJECT_INCLUDE_BEFORE:FILEPATH=${hook}\n`);
  else {
    const cache=path.join(source,'build/build/CMakeCache.txt');
    const previous=read(cache).match(/^CANGJIE_CPU_JOBS:STRING=(\d+)$/m);assert(previous);jobs=String(Number(previous[1])+1);
    fs.appendFileSync(cache,`\nCMAKE_PROJECT_INCLUDE_BEFORE:FILEPATH=${hook}\n`);
  }
  const parent=fifo('parent','abc');const log=path.join(root,'budget.jsonl');
  const env={...clean,MAKEFLAGS:parent.auth,CANGJIE_JOBSERVER_LOG:log};
  const args=['python3',path.join(source,'build.py'),'build','-t','release','--jobs',jobs,'--build-target','boundscheck','--build-target','boundscheck-static'];
  if(variant==='control') {
    // Reconfigure normally and exercise a genuine native dependency edge again.
    const touched=path.join(source,'third_party/boundscheck-v1.1.16/src/memcpy_s.c'),now=new Date();fs.utimesSync(touched,now,now);
    const file=config('flow',[{argv:args}]);const r=run('public-control',['node',entry,'--config',file,'--no-package'],{env});assert.equal(r.status,0,r.stderr);
    assert.equal(JSON.parse(read(marker)).parallel,'1');
    const e=events(log);assert(e.some(x=>x.event==='command-start'&&x.command[0].endsWith('/ccache')));
    const original=Buffer.alloc(3);assert.equal(fs.readSync(parent.fd,original,0,3,null),3);assert.equal([...original].sort().join(),[...Buffer.from('abc')].sort().join());fs.closeSync(parent.fd);
    console.log('F2_NORMAL_RECONFIGURE_AND_F1_CCACHE_CONTROL_PASS');return;
  }
  const race=path.join(root,'launch-race.json');
  if(variant.startsWith('python-race')) {
    const dir=path.join(root,'python-hook');fs.mkdirSync(dir);
    // Fault-injection fixture: hold the *real* Popen return boundary, after
    // the actual CMake child exists and before the product can assign it.
    fs.writeFileSync(path.join(dir,'sitecustomize.py'),`import subprocess,os,json,time\n_real=subprocess.Popen\ndef _launch(*a,**k):\n p=_real(*a,**k)\n if a and isinstance(a[0],list) and a[0][0]=='cmake':\n  with open(${JSON.stringify(race)},'w') as f: json.dump({'python':os.getpid(),'child':p.pid},f)\n  time.sleep(1)\n return p\nsubprocess.Popen=_launch\n`);env.PYTHONPATH=dir;
  }
  if(variant.startsWith('sdk-race')) {
    const injection=path.join(root,'sdk-launch-race.mjs');
    fs.writeFileSync(injection,`import cp from 'node:child_process';import fs from 'node:fs';import {syncBuiltinESMExports} from 'node:module';const real=cp.spawn;cp.spawn=function(...a){const c=real(...a);if(a[0]==='python3'){fs.writeFileSync(${JSON.stringify(race)},JSON.stringify({sdk:process.pid,child:c.pid}));cp.spawnSync('/bin/kill',[${JSON.stringify(signal==='SIGINT'?'-INT':'-TERM')},String(process.pid)]);}return c;};syncBuiltinESMExports();`);
    env.NODE_OPTIONS='--import='+injection;
  }
  const file=config('flow',[{argv:args,env:{NODE_OPTIONS:''}}]);
  const a=fs.openSync(path.join(root,'cancel.stdout'),'w'),b=fs.openSync(path.join(root,'cancel.stderr'),'w');
  const child=spawn('node',[entry,'--config',file,'--no-package'],{env,cwd:root,stdio:['ignore',a,b]});
  let result;const done=new Promise(resolve=>child.once('exit',(rc,signal)=>{result={rc,signal};resolve(result);}));
  const known=new Map();let reached=false;
  try {
    for(let n=0;n<2000;n++) {
      rememberFamily(child.pid,known);
      const launched=readyJson(race);if(launched?.child)rememberFamily(launched.child,known);
      const marked=readyJson(marker);
      if(marked) {
        const m=marked;const rows=processRows();for(const row of rows)if([m.pid,m.cmake].includes(row.pid))known.set(row.pid,row);
        reached=true;
      }
      if(variant.startsWith('sdk-race')&&result&&(arm==='new'||reached))break;
      if(!variant.startsWith('sdk-race')&&reached&&(!variant.startsWith('python-race')||fs.existsSync(race)))break;
      if(result&&!variant.startsWith('sdk-race'))break;await delay(10);
    }
    if(!variant.startsWith('sdk-race')) {
      assert(reached,'actual CMake configure and legal execute_process child must be reached');
      const m=JSON.parse(read(marker));assert(fs.readlinkSync('/proc/'+m.cmake+'/exe').endsWith('/cmake'));
      child.kill(signal);
    } else assert(fs.existsSync(race),'actual SDK spawn boundary must be injected');
    await Promise.race([done,delay(10000).then(()=>{if(!result)throw Error('SDK cancellation did not finish');})]);
    await delay(50);
    const left=living(known).filter(r=>r.pid!==child.pid);
    const bytes=Buffer.alloc(3);assert.equal(fs.readSync(parent.fd,bytes,0,3,null),3);assert.equal([...bytes].sort().join(),[...Buffer.from('abc')].sort().join());
    const record={variant,result,reached,known:[...known.values()],left,returned:bytes.toString(),source,topLevelSha256:hash(path.join(source,'CMakeLists.txt'))};
    fs.writeFileSync(path.join(root,'F2-result.json'),JSON.stringify(record,null,2));
    console.log('F2_NO_DESCENDANTS_ASSERTION_REACHED '+JSON.stringify({variant,left:left.length,result,reached,parentReturned:bytes.toString()}));
    await cleanupKnown(known);
    assert.equal(left.length,0,'SDK completion must wait/drain configure and launch-boundary descendants');
    assert.equal(result.rc,signal==='SIGINT'?130:143);
  } finally {
    if(!result){child.kill('SIGTERM');await done;}await cleanupKnown(known);fs.closeSync(a);fs.closeSync(b);fs.closeSync(parent.fd);
  }
}
async function f3() {
  if(variant==='ordinary') {
    const rows=[
      ['no-pool',{},0],
      ['disabled',{MAKEFLAGS:'-j4 --jobserver-auth=-2,-2'},0],
      ['mixed-negative',{MAKEFLAGS:'--jobserver-auth=-2,7 -j4'},0],
      ['priority-disabled',{CARGO_MAKEFLAGS:'--jobserver-auth=-2,-2',MAKEFLAGS:'--jobserver-auth=999,998'},0],
      ['last-disabled',{MAKEFLAGS:'--jobserver-auth=999,998 --jobserver-auth=-2,-2'},0],
      ['closed-positive',{MAKEFLAGS:'--jobserver-auth=999,998'},1],
      ['priority-invalid',{CARGO_MAKEFLAGS:'--jobserver-auth=999,998',MAKEFLAGS:'--jobserver-auth=-2,-2'},1],
      ['malformed',{MAKEFLAGS:'--jobserver-auth=oops'},1],
    ];
    for(const [name,flags,expected]of rows) {
      const file=config(name,[{argv:['node','-e','console.log("ORDINARY_CONTROL_42")']}]);
      const r=run(name,['node',entry,'--config',file,'--no-package'],{env:{...clean,...flags}});
      console.log('F3_PUBLIC_DISABLED_AUTH_ASSERTION_REACHED '+JSON.stringify({name,rc:r.status,expected}));
      assert.equal(r.status,expected,'valid disabled jobserver must allow ordinary public SDK commands; invalid positive auth must fail');
      if(expected===0)assert.match(r.stdout,/ORDINARY_CONTROL_42/);else assert.doesNotMatch(r.stdout,/ORDINARY_CONTROL_42/);
    }
    return;
  }
  const source=path.join(root,'fixture');if(!resume)fs.mkdirSync(source);
  const template=process.env.JOBSERVER_REWORK_TEMPLATE;assert(template);
  if(!resume) {
  for(const name of ['budget_a','budget_b','main.cj'])fs.cpSync(path.join(template,name),path.join(source,name),{recursive:true});
  fs.copyFileSync(path.join(R,'stdlib/build.py'),path.join(source,'build.py'));
  fs.cpSync(path.join(R,'stdlib/cmake'),path.join(source,'cmake'),{recursive:true});
  fs.symlinkSync(path.join(R,'stdlib/scripts'),path.join(source,'scripts'));
  let cmake=read(path.join(template,'CMakeLists.txt'));
  cmake=cmake.replace(/include\("[^"\n]*AddCangjieSource.cmake"\)/,
    `include("${R}/stdlib/cmake/modules/AddCangjieSource.cmake")\noption(CANGJIE_ENABLE_CCACHE "Build cangjie with ccache" ON)\nif(CANGJIE_ENABLE_CCACHE OR CMAKE_BUILD_TYPE MATCHES Debug OR CMAKE_BUILD_TYPE MATCHES RelWithDebInfo)\n include("${R}/stdlib/cmake/optional/CangjieCCache.cmake")\nendif()`);
  fs.writeFileSync(path.join(source,'CMakeLists.txt'),cmake+'\nadd_executable(native_probe native.c)\ninstall(TARGETS native_probe RUNTIME DESTINATION bin)\n');
  fs.writeFileSync(path.join(source,'native.c'),'#include <stdio.h>\nint main(void){puts("NATIVE_CONTROL_42");return 0;}\n');
  } else {
    // Only the install helper assignment changed after a successful build.
    // Preserve the compiler inputs, graph, logs and compiled outputs.
    fs.copyFileSync(path.join(R,'stdlib/build.py'),path.join(source,'build.py'));
  }
  const log=path.join(root,'budget.jsonl'),env={...clean,CANGJIE_JOBSERVER_LOG:log};
  const commands=[{argv:['python3',path.join(source,'build.py'),'build','-t','release','--jobs','4']},
    {argv:['python3',path.join(source,'build.py'),'install','--prefix','${install}']},
    {argv:['${install}/bin/probe']},{argv:['${install}/bin/native_probe']}];
  const file=config(resume?'recovered-install':'flow',resume?commands.slice(1):commands);
  let r;
  if(variant==='valid-pipe') {
    const mk=path.join(root,'Makefile');fs.writeFileSync(mk,`all:\n\t+node ${entry} --config ${file} --no-package\n`);
    r=run('public-build',['make','-s','-j2','-f',mk],{env});
  } else {
    env.MAKEFLAGS='--jobserver-auth=-2,-2 -j4';
    r=run(resume?'public-install-recovered':'public-build',['node',entry,'--config',file,'--no-package'],{env});
  }
  console.log('F3_PUBLIC_BUDGET_BUILD_ASSERTION_REACHED '+JSON.stringify({variant,rc:r.status}));
  assert.equal(r.status,0,r.stderr);
  if(resume)r.stdout=read(path.join(root,'public-build.stdout'))+r.stdout;
  assert.match(r.stdout,/NATIVE_CONTROL_42/);assert.match(r.stdout,/(?:^|\n)16\s*(?:\n|$)/);
  const e=events(log),pool=e.find(x=>x.event==='pool-start');const capacity=variant==='valid-pipe'?2:1;
  assert.equal(pool.jobs,capacity);assert.equal(pool.disabled,variant!=='valid-pipe');
  assert.equal(e.find(x=>x.event==='pool-end').returned,capacity);
  const commandsStarted=e.filter(x=>x.event==='command-start');
  assert.equal(commandsStarted.filter(x=>x.command[0]==='cjc').length,5);
  assert(commandsStarted.some(x=>x.command[0].endsWith('/ccache')));assert(commandsStarted.some(x=>x.command[0]==='ar'));
  const peaks=[...(r.stdout+r.stderr).matchAll(/CJCJ_JOBSERVER acquired=(\d+) released=(\d+) active=(\d+) peak=(\d+) held_tokens=(\d+)/g)].map(m=>m.slice(1).map(Number));
  assert.equal(peaks.length,5);assert(peaks.every(([a,b,c,p,h])=>a>0&&a===b&&c===0&&p<=capacity&&h===0));
  const intervals=commandsStarted.map(s=>({command:s.command,start:BigInt(s.time_ns),end:BigInt(e.find(x=>x.event==='command-end'&&x.pid===s.pid).time_ns)}));
  const overlap=intervals.some((a,i)=>intervals.slice(i+1).some(b=>a.start<b.end&&b.start<a.end));
  if(capacity===1){assert(peaks.every(x=>x[3]===1));assert(!overlap,'disabled pool serializes native/cjc/ar actual command intervals');}
  else {assert(peaks.some(x=>x[3]>1));assert.equal(e.find(x=>x.event==='parent-return').count,1);}
  fs.writeFileSync(path.join(root,'F3-result.json'),JSON.stringify({pool,peaks,overlap,commands:commandsStarted.map(x=>x.command)},null,2));
  console.log('F3_REAL_NATIVE_CJC_AR_AND_INSTALL_PASS '+JSON.stringify({capacity,peaks,overlap}));
}
try {
  if(mode==='f1')await f1();else if(mode==='f2')await f2();else if(mode==='f3')await f3();else throw Error('unknown gate '+mode);
  console.log('REWORK_GATE_PASS');
} catch(error) {console.error(error.stack);process.exitCode=1;}
