#!/usr/bin/env zx
// Real qualification orchestration with bounded subprocess fixtures. NOT native qualification.
import {readFileSync,writeFileSync,mkdirSync,cpSync,chmodSync,existsSync} from 'node:fs';
import {resolve,join,dirname} from 'node:path';
import {fileURLToPath,pathToFileURL} from 'node:url';
import {spawnSync} from 'node:child_process';
const [workArg,moduleArg,only]=process.argv.slice(2);
if(!workArg) throw new Error('usage: zx test_teardown_qualification.mjs WORK [MODULE] [CASE]');
const work=resolve(workArg),root=resolve(dirname(fileURLToPath(import.meta.url)),'../../..');
const {Qualification,PRODUCT,OBSERVER,TEST,validate}=await import(pathToFileURL(moduleArg??join(root,'runtime/tests/teardown_qualification.mjs')));
mkdirSync(work,{recursive:true});
const sources=resolve(process.env.QUALIFICATION_CONTROL_SOURCES??join(work,'../sources'));
const scheduler=join(sources,'scheduler');
const git=spawnSync('git',['-C',scheduler,'rev-parse','HEAD'],{encoding:'utf8'});
if(git.status!==0) throw new Error('real source checkouts unavailable');
const candidate=git.stdout.trim(),bin=join(work,'bin'); mkdirSync(bin);
const fixture=join(work,'fixture.mjs');
// Fixtures never execute a runtime, gdb, ptrace or native build. Their invocations
// are persisted before dispatch; the actual Python record consumer runs normally.
writeFileSync(fixture,`#!/usr/bin/env node
import fs from 'node:fs'; import path from 'node:path'; import cp from 'node:child_process';
const cmd=path.basename(process.argv[1]),a=process.argv.slice(2),mode=process.env.CONTROL_MODE??'success';
const log=process.env.CONTROL_DISPATCH;
fs.appendFileSync(log,JSON.stringify({cmd,a,buildOnly:process.env.GC_UNIT_BUILD_ONLY??null,gateSkip:process.env.GC_UNIT_GATE_SKIP??null,lib:process.env.GCV2_RUNTIME_LIB_DIR??null,root:process.env.GCV2_RUNTIME_OUTPUT_ROOT??null,out:process.env.GC_UNIT_OUT??null})+'\\n');
const elf=()=>{const b=Buffer.alloc(64); b.set([127,69,76,70,2,1]); b.writeUInt16LE(62,18); return b;};
if(cmd==='sccache') process.exit(0);
if(cmd==='cmake') {
 if(a.includes('-B')) {
  const build=a[a.indexOf('-B')+1],pub=build+'/publication'; fs.mkdirSync(pub+'/lib/native',{recursive:true});
  for(const n of ['libcangjie-runtime.so','libboundscheck.so']) if(!(mode==='missing-so' && n==='libboundscheck.so')) fs.writeFileSync(pub+'/lib/native/'+n,elf());
  const cache=a.find(s=>s.startsWith('-DCANGJIE_COMPILER_CACHE=')).split('=')[1];
  fs.writeFileSync(build+'/CMakeCache.txt','OUTPUT_TEMP_PATH:INTERNAL='+pub+'\\n'+['CANGJIE_COMPILER_CACHE',...['C','CXX','ASM'].map(l=>'CMAKE_'+l+'_COMPILER_LAUNCHER')].map(k=>k+':STRING='+(mode==='wrong-cache'?'ccache':cache)+'\\n').join(''));
 }
 process.exit(a.includes('--build') && mode==='build-fail'?9:0);
}
if(cmd==='bash' && a[0].endsWith('/run_standalone.sh')) {
 if(process.env.GC_UNIT_BUILD_ONLY!=='1' || process.env.GC_UNIT_GATE_SKIP!=='1') process.exit(86);
 fs.mkdirSync(process.env.GC_UNIT_OUT,{recursive:true});
 if(mode!=='missing-elf') fs.writeFileSync(process.env.GC_UNIT_OUT+'/cj_gc_unit',elf());
 if(mode!=='missing-marker') console.log('GC_UNIT_BUILD_ONLY_DONE tests_executed=0'); process.exit(0);
}
if(cmd==='readelf') {console.log('controlled ELF metadata; no native qualification');process.exit(0);}
if(cmd==='nm') {if(a.includes('--defined-only')) console.log('0000000000001000 T MapleRuntime::GcUnit::CompleteTestRun(int)');process.exit(0);}
if(cmd==='gdb') {console.log(fs.readFileSync(process.env.CONTROL_GDB,'utf8'));process.exit(0);}
if(cmd==='python3' && a[0].endsWith('/check_teardown_exit.py')) {
 const live=a.includes('--live'); console.log(fs.readFileSync(live?process.env.CONTROL_LIVE:process.env.CONTROL_EXITED,'utf8'));
 process.exit(mode==='child77'?77:mode==='child124'?124:live?1:0);
}
if(cmd==='python3' && a[0].endsWith('/check_teardown_records.py') && ['child77','child124','bad-live'].includes(mode)) {
 // Even an erroneously accepting downstream consumer cannot hide original child rc.
 console.log('TEARDOWN_RECORDS_ACCEPT {}');process.exit(0);
}
const actual=cmd==='bash'?'/bin/bash':cmd==='python3'?'/usr/bin/python3':null;
if(!actual) process.exit(125);
const p=cp.spawnSync(actual,a,{stdio:'inherit'});process.exit(p.status??125);
`);
for(const cmd of ['cmake','bash','python3','sccache','readelf','nm','gdb']) {cpSync(fixture,join(bin,cmd));chmodSync(join(bin,cmd),0o755);}
const wrapper=join(work,'clang++');writeFileSync(wrapper,'#!/bin/sh\nexec sccache /usr/bin/clang++ "$@"\n');chmodSync(wrapper,0o755);
// Complete finite recorded-state fixtures for the real consumer, with no native
// capability claim. These cover original normal/live exit semantics and progress.
const pid=1700,tid=1749,handle=0x701000,output=0x7ff010,ptid=0x701100,ctid=0x701108;
const main=0x600000,create=0x700000,j=0x700004,complete=0x600004,watched=[create,j,complete];
const TESTLINE='[  RUN   ] '+TEST,sentinel='GC_UNIT_OTHER_VM_OKIDOKI '+TEST;
const gdb=[TESTLINE,'ASSERT_TEARDOWN_BEFORE_SENTINEL samples=1 PASS','ASSERT_TEARDOWN_LIVE samples=1 PASS','ASSERT_TEARDOWN_POOL_STOPPED samples=1 PASS','ASSERT_TEARDOWN_EXECUTED exits=[0] errors=[] PASS',sentinel].join('\n')+'\n';
function exitLog(live=false,progress=true) {
 const lines=[TESTLINE,'RUNTIME_WORKERS_LIVE created=1 active=1','CONSTRUCT_HOLD_EXIT tid='+tid+' name=RuntimeWorker#0','CONSTRUCT_JOIN_WAIT tid='+tid+' syscall=202'];
 const emit=(k,r)=>lines.push(k+' '+JSON.stringify(r));
 const clone={caller:pid,syscall:56,args:[0x3d0f00,0x801000,ptid,ctid,0x901000,0],flags:0x3d0f00,parent_tid:ptid,child_tid:ctid,clone3_raw:null};
 const waitArgs=[ctid,265,1,0,0,0xffffffff];
 emit('CONSTRUCTION_DOMAIN',{machine:'x86_64'});emit('CLONE_ABI',clone);emit('CLONE',{parent:pid,child:tid});emit('PTHREAD_CLONE',{...clone,tid,handle,output,parent_value:tid,child_value:1});
 emit('SYSCALL_ENTRY',{tid:pid,nr:56,args:clone.args,length:80});emit('SYSCALL_ENTRY',{tid:pid,nr:202,args:waitArgs,length:80});emit('WAIT_EVENT',{tid,event:6});
 for(const state of ['t','Z']) emit('TASK_STATE',{pid,tid,name:'RuntimeWorker#0',tgid:pid,state});
 for(let i=0;i<3;i++) emit('REGSET',{writing:false,length:216,raw:'00'.repeat(216)});
 emit('ELF_BINDING',{name:'main',address:main});emit('ELF_BINDING',{name:'MapleRuntime::GcUnit::CompleteTestRun(int)',address:complete});
 emit('PTHREAD_PROVIDER',{symbol:'pthread_create',address:create});emit('PTHREAD_PROVIDER',{symbol:'pthread_join',address:j});
 const hardware=addresses=>emit('HARDWARE_BREAKPOINTS',{tid:pid,machine:'x86_64',addresses,slots:[...addresses.map(address=>({address,control:1})),...Array(4-addresses.length).fill({address:0,control:0})]});
 hardware([main]);hardware(watched);
 for(const [kind,pc,extra] of [['PTHREAD_CREATE_ENTRY',create,{output}],['PTHREAD_JOIN_ENTRY',j,{handle,target:tid}]]) {
   emit(kind,{caller:pid,pc,...extra});hardware([]);hardware(watched);emit('BREAKPOINT_STEP',{tid:pid,pc,after:progress?pc+4:pc,si_code:2,addresses:watched,text_written:false,registers_written:false});
 }
 hardware([complete]);hardware([]);
 emit('JOIN_ABI',{caller:pid,tid,handle,parent_tid:ptid,child_tid:ctid,uaddr:ctid,value:1,args:waitArgs,syscall:202});
 emit('WORKER_SET',{workers:[tid],exit_events:[tid],held:tid,tgid:pid,created:1,active:1,names:{[tid]:'RuntimeWorker#0'}});
 if(live){lines.push('ASSERT_TEARDOWN_BEFORE_SENTINEL samples=1 FAIL');lines.push('TEARDOWN_CONSTRUCT_PRE_EXIT tid='+tid+' accepted=False');}
 emit('HELD_EXIT_READY',{tid,si_pid:tid,si_code:1,si_status:0,state:'Z',reaped:false});emit('JOIN_CLEARED',{tid,handle,child_tid:ctid,value:0});
 lines.push('COMPLETE_AFTER_JOIN held_tid='+tid+' comm=RuntimeWorker#0 state=Z');if(!live) lines.push('ASSERT_TEARDOWN_BEFORE_SENTINEL samples=1 PASS');
 lines.push('AFTER_TRACER_REAP task_exists=False');emit('BREAKPOINT_RESTORED',{text_written:false,registers_written:false});lines.push(sentinel);lines.push('TEARDOWN_CONSTRUCT_EXECUTED product_rc=0');emit('CONSTRUCTION_CLEANUP',{owned:[pid,tid],reaped:[pid,tid]});
 return lines.join('\n')+'\n';
}
for(const [name,content] of [['gdb',gdb],['exited',exitLog()],['live',exitLog(true)]]) writeFileSync(join(work,name+'.txt'),content);
const args={candidate,productRef:PRODUCT,elfRef:PRODUCT,observerRef:OBSERVER,filter:TEST,host:'ubuntu-26.04',productSource:join(sources,'product'),observerSource:join(sources,'observer'),schedulerSource:scheduler};
const checks=[];
function check(n,v,detail=''){checks.push({name:n,pass:!!v,detail});console.log('ASSERT '+n+' '+(v?'PASS':'FAIL')+' '+detail);}
function runCase(name,body){if(only==='remaining' ? name==='prepare.success' : only && only!==name)return;try{body();}catch(e){check(name+'.unexpected',false,e.stack);}}
function qualify(name,mode='success',patch={}) {
 const evidence=join(work,name);const q=new Qualification({...args,...patch,evidence});
 q.env={...q.env,PATH:bin+':'+process.env.PATH,CXX:wrapper,CONTROL_MODE:mode,CONTROL_DISPATCH:join(work,name+'.dispatch'),CONTROL_GDB:join(work,'gdb.txt'),CONTROL_EXITED:join(work,'exited.txt'),CONTROL_LIVE:join(work,'live.txt'),PYTHONDONTWRITEBYTECODE:'1'};
 return q;
}
runCase('identity',()=>{
 for(const [name,patch] of [['correct',{}],['wrong-product',{productRef:candidate}],['wrong-unit',{elfRef:OBSERVER}],['wrong-observer',{observerRef:PRODUCT}],['wrong-filter',{filter:'Other.Test'}],['old-retained',{reuse:'retained'}]]) {
  let rejected=false;try{validate({...args,...patch});}catch{rejected=true;}check('identity.'+name,rejected===(name!=='correct'));
 }
 const q=qualify('identity-real');q.identities();check('identity.real-checkouts',q.stages.identity==='PASS');
 const bad=qualify('identity-wrong-checkout','success',{productSource:join(sources,'observer'),observerSource:join(sources,'product')});
 check('identity.wrong-checkout',bad.execute()===1 && JSON.parse(readFileSync(join(bad.evidence,'summary.json'))).status==='NOT_QUALIFIED');
});
runCase('domain',()=>{const q=qualify('domain');const rc=q.execute();check('domain.no-platform-claim',rc===1 && JSON.parse(readFileSync(join(q.evidence,'summary.json'))).status==='NOT_QUALIFIED' && !existsSync(join(q.evidence,'product-build.rc')));});
runCase('prepare.success',()=>{
 const q=qualify('prepare-success');q.identities();let error=null;try{q.prepare();}catch(e){error=e.message;}
 const calls=readFileSync(join(work,'prepare-success.dispatch'),'utf8').trim().split('\n').map(JSON.parse),standalone=calls.find(c=>c.cmd==='bash' && c.a[0].endsWith('/run_standalone.sh'));
 check('prepare.dispatch.flags',standalone?.buildOnly==='1' && standalone?.gateSkip==='1' && !!standalone?.lib && !!standalone?.root && !!standalone?.out,JSON.stringify(standalone));
 check('prepare.success',!error && q.stages.prepare==='PASS' && existsSync(join(q.evidence,'entities-before-run.json')),String(error));
});
runCase('arm.normal',()=>{const q=qualify('arm-normal');q.identities();q.prepare();q.arm('candidate',q.scripts);check('arm.original-normal-live',q.stages.candidate==='PASS' && readFileSync(join(q.evidence,'candidate/teardown-live.rc'),'utf8')==='1\n');});
for(const mode of ['build-fail','missing-so','missing-elf','missing-marker','wrong-cache']) runCase('prepare.'+mode,()=>{
 const q=qualify('prepare-'+mode,mode);q.identities();let error=null;try{q.prepare();}catch(e){error=e.message;}
 const expected={'build-fail':'product-build: child rc=9','missing-so':'libboundscheck.so','missing-elf':'cj_gc_unit','missing-marker':'build-only completion not observed','wrong-cache':'effective sccache cache/launcher mismatch'}[mode];
 check('prepare.reject.'+mode,!!error && error.includes(expected) && q.stages.prepare==='NOT_RUN' && !existsSync(join(q.evidence,'QUALIFICATION_DONE')),String(error));
 const calls=readFileSync(join(work,'prepare-'+mode+'.dispatch'),'utf8');if(mode==='build-fail')check('prepare.build-fail.no-standalone',!calls.includes('run_standalone.sh') && readFileSync(join(q.evidence,'product-build.rc'),'utf8')==='9\n');
});
for(const mode of ['child77','child124']) runCase('arm.'+mode,()=>{
 const q=qualify('arm-'+mode,mode);q.identities();q.prepare();let error=null;try{q.arm('candidate',q.scripts);}catch(e){error=e.message;}
 check('admission.'+mode,!!error && q.stages.candidate==='NOT_RUN' && readFileSync(join(q.evidence,'candidate/teardown-exited.rc'),'utf8')===(mode==='child77'?'77\n':'124\n'),String(error));
 check('admission.'+mode+'.no-success',!existsSync(join(q.evidence,'QUALIFICATION_DONE')));
});
runCase('records.progress',()=>{
 const q=qualify('records-progress');q.identities();q.prepare();writeFileSync(join(work,'exited.txt'),exitLog(false,false));let error=null;
 try{q.arm('candidate',q.scripts);}catch(e){error=e.message;}
 check('records.exact-progress-rejection',!!error && readFileSync(join(q.evidence,'candidate/teardown-records.log'),'utf8').includes('FAIL reason=entry-progress'),String(error));writeFileSync(join(work,'exited.txt'),exitLog());
});
jsonResult();
function jsonResult(){writeFileSync(join(work,'assertions.json'),JSON.stringify({scope:'APPARATUS_ONLY; native qualification NOT_RUN',checks},null,2));console.log('CONTROL_ASSERTIONS total='+checks.length+' passed='+checks.filter(c=>c.pass).length+' failed='+checks.filter(c=>!c.pass).length);process.exitCode=checks.length>0 && checks.every(c=>c.pass)?0:1;}
