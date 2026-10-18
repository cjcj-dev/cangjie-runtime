#!/usr/bin/env zx
// F4: public SDK -> actual build.py -> unchanged stdlib top-level -> Ninja.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
const mode=process.argv[2],root=process.env.JOBSERVER_TARGET_ROOT;
const R=process.env.JOBSERVER_REWORK_R,T=process.env.JOBSERVER_REWORK_T,sdk=process.env.JOBSERVER_TEST_SDK;
assert(root&&R&&T&&sdk&&['old','new'].includes(mode));
const source=path.join(root,'stdlib'),build=path.join(source,'build/build');
const entry=path.join(T,'ci/bootstrap/sdk-build.mjs'),product=path.join(source,'build.py');
const input=path.join(source,'third_party/boundscheck-v1.1.16/src/securecutil.c');
const library=path.join(build,'lib/libboundscheck.so'),archive=path.join(build,'lib/libboundscheck-static.a');
const env={...process.env,PATH:`${sdk}/bin:${sdk}/tools/bin:/usr/local/bin:/usr/bin:/bin`,CANGJIE_HOME:sdk,
 CCACHE_DIR:path.join(root,'ccache'),LD_LIBRARY_PATH:`${sdk}/runtime/lib/linux_x86_64_cjnative:${sdk}/lib/linux_x86_64_cjnative`};
for(const k of ['MAKEFLAGS','MFLAGS','CARGO_MAKEFLAGS','CJPM_JOBSERVER','CANGJIE_JOBSERVER_ROOT','CANGJIE_JOBSERVER_JOBS','NODE_OPTIONS','PYTHONPATH'])delete env[k];
const digest=p=>crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
const records=fs.existsSync(path.join(root,'commands.json'))?JSON.parse(fs.readFileSync(path.join(root,'commands.json'))):[];
function run(name,argv,extra={}){
 const out=fs.openSync(path.join(root,name+'.stdout'),'w'),err=fs.openSync(path.join(root,name+'.stderr'),'w');
 const r=spawnSync(argv[0],argv.slice(1),{cwd:root,env:{...env,...extra},stdio:['ignore',out,err],timeout:300000});
 fs.closeSync(out);fs.closeSync(err);
 const row={name,argv,rc:r.status,signal:r.signal,error:r.error?.message};records.push(row);
 fs.writeFileSync(path.join(root,'commands.json'),JSON.stringify(records,null,2));console.log(JSON.stringify(row));
 return {...r,stdout:fs.readFileSync(path.join(root,name+'.stdout'),'utf8'),stderr:fs.readFileSync(path.join(root,name+'.stderr'),'utf8')};
}
function publicBuild(name,targets,completedBuild=false){
 const work=path.join(root,name),file=work+'.json',log=work+'.jsonl';
 fs.writeFileSync(file,JSON.stringify({work,install:work+'/install',repos:{},finalStage:'native',stages:[{id:'native',output:work+'/install',commands:completedBuild?[]:[{argv:['python3',product,'build','-t','release','--jobs','4',...targets]}]}],test:{}},null,2));
 return {...run(name,['node',entry,'--config',file,'--no-package'],{CANGJIE_JOBSERVER_LOG:log}),log};
}
function consume(name,expected){
 const work=path.join(root,name),file=work+'.json';
 const code='import ctypes,sys; l=ctypes.CDLL(sys.argv[1]); l.GetHwSecureCVersion.restype=ctypes.c_char_p; v=l.GetHwSecureCVersion(None).decode(); b=ctypes.create_string_buffer(4); assert l.memcpy_s(b,4,b"abc",4)==0 and b.value==b"abc"; print("F4_INSTALLED_VERSION_ASSERTION_REACHED "+v,flush=True); assert v==sys.argv[2], "BUILD_MUST_UPDATE_REQUESTED_TARGET"';
 fs.writeFileSync(file,JSON.stringify({work,install:work+'/install',repos:{},finalStage:'native',stages:[{id:'native',output:work+'/install',copies:[{from:library,to:'lib/libboundscheck.so'},{from:archive,to:'lib/libboundscheck-static.a'}]}],test:{required:['lib/libboundscheck.so','lib/libboundscheck-static.a'],commands:[{argv:['python3','-c',code,'${install}/lib/libboundscheck.so',expected]}]}},null,2));
 return run(name,['node',entry,'--config',file,'--no-package']);
}
function snapshot(){return {input:digest(input),topLevel:digest(path.join(source,'CMakeLists.txt')),cache:digest(path.join(build,'CMakeCache.txt')),library:digest(library),archive:digest(archive),product:digest(product)};}
const valid=['--build-target','boundscheck','--build-target=boundscheck-static'];
if(mode==='old'){
 if(process.env.JOBSERVER_TARGET_RESUME==='1'){
  const result=JSON.parse(fs.readFileSync(path.join(root,'old-initial-build/build-result.json')));
  assert.equal(result.commands.length,1);assert.equal(result.commands[0].rc,0,'resume only a successful real child build, not an assumed cache');
  const events=fs.readFileSync(path.join(root,'old-initial-build.jsonl'),'utf8').trim().split('\n').map(JSON.parse);
  assert(events.some(e=>e.event==='pool-end'&&e.rc===0&&e.missing===0));
  assert(events.some(e=>e.event==='command-start'&&e.command.some(x=>x.endsWith('/securecutil.c'))));
  const recovered=publicBuild('old-initial-build-recovered',[],true);assert.equal(recovered.status,0,recovered.stderr);
 }else{
 assert(!fs.existsSync(source),'fresh isolated source and normal configuration required');
 fs.cpSync(path.join(R,'stdlib'),source,{recursive:true,filter:p=>!['build','output'].includes(path.basename(p))});
 fs.copyFileSync(path.join(root,'old-build.py'),product);
 assert.equal(digest(path.join(source,'CMakeLists.txt')),digest(path.join(R,'stdlib/CMakeLists.txt')));
 fs.mkdirSync(build,{recursive:true});fs.writeFileSync(path.join(build,'CMakeCache.txt'),'CANGJIE_ENABLE_CCACHE:BOOL=ON\n');
 const initial=publicBuild('old-initial-build',valid);assert.equal(initial.status,0,initial.stderr);
 }
 const consumed=consume('old-initial-consume','1.1.16');assert.equal(consumed.status,0,consumed.stderr);
 const before=snapshot();fs.writeFileSync(path.join(root,'before-input-change.json'),JSON.stringify(before,null,2));
 const text=fs.readFileSync(input,'utf8');assert.equal((text.match(/#define SECUREC_VERSION_STR   "1\.1\.16"/g)||[]).length,1);
 fs.writeFileSync(input,text.replace('#define SECUREC_VERSION_STR   "1.1.16"','#define SECUREC_VERSION_STR   "F4-rebuilt"'));
 const pending=snapshot();assert.notEqual(pending.input,before.input);assert.equal(pending.library,before.library);
 fs.writeFileSync(path.join(root,'pending.json'),JSON.stringify(pending,null,2));
 const dry=publicBuild('old-option-dry-run',['--build-target=-n','--build-target=boundscheck']);
 assert.equal(dry.status,0,dry.stderr);assert.deepEqual(snapshot(),pending,'dry-run leaves pending source and old output bytes');
 const events=fs.readFileSync(dry.log,'utf8').trim().split('\n').map(JSON.parse);
 assert(events.some(e=>e.event==='pool-start'&&e.command.includes('-n')));
 assert.equal(events.filter(e=>e.event==='command-start').length,0,'Ninja only printed the pending build commands');
 const independent=consume('old-independent-control','1.1.16');assert.equal(independent.status,0,independent.stderr);
 const target=consume('old-target-red','F4-rebuilt');
 fs.writeFileSync(path.join(root,'old-result.json'),JSON.stringify({buildRc:dry.status,independentControlRc:independent.status,targetRc:target.status,pending,after:snapshot()},null,2));
 assert.equal(target.status,1);assert.match(target.stdout,/F4_INSTALLED_VERSION_ASSERTION_REACHED 1\.1\.16/);assert.match(target.stderr,/BUILD_MUST_UPDATE_REQUESTED_TARGET/);
 console.log('F4_OLD_PUBLIC_BUILD_FALSE_SUCCESS_AND_EXACT_CONSUMER_RED');
}else{
 const pending=JSON.parse(fs.readFileSync(path.join(root,'pending.json')));
 const prior=snapshot();
 for(const key of ['input','topLevel','cache','library','archive'])assert.equal(prior[key],pending[key],'old red state is reused without rebuilding or touching timestamps');
 if(process.env.JOBSERVER_TARGET_RESUME!=='1')assert.equal(prior.product,pending.product);
 fs.copyFileSync(path.join(R,'stdlib/build.py'),product);
 const before=snapshot(),invalid=[
  ['dry-run',['--build-target=-n','--build-target=boundscheck']],
  ['tool-clean',['--build-target=-t','--build-target=clean']],
  ['empty',['--build-target=']],
  ['blank',['--build-target',' ']],
  ['valid-then-option',['--build-target=boundscheck','--build-target=--version']],
  ['option-separate',['--build-target','-n']],
  ['assignment',['--build-target=CFLAGS=-n']],
  ['separator',['--build-target=--']],
 ];
 const rejected=[];
 for(const [name,targets]of invalid){
  const previous=records.find(r=>r.name==='new-reject-'+name);
  if(process.env.JOBSERVER_TARGET_RESUME==='1'&&previous?.rc===2){
   assert(!fs.existsSync(path.join(root,'new-reject-'+name+'.jsonl')));rejected.push({name,rc:2,reused:'same type-validation branch, before final-value check'});continue;
  }
  const suffix=previous?'-recovered':'';
  const r=publicBuild('new-reject-'+name+suffix,targets);assert.equal(r.status,2,r.stderr);
  assert.match(r.stderr,/argument --build-target:/);assert(!fs.existsSync(r.log),'rejected input must not launch Ninja or create a budget');
  assert.deepEqual(snapshot(),before,'rejection preserves the pending target and all existing outputs');rejected.push({name,rc:r.status});
 }
 const normal=publicBuild('new-normal-build',valid);assert.equal(normal.status,0,normal.stderr);
 const events=fs.readFileSync(normal.log,'utf8').trim().split('\n').map(JSON.parse);
 const commands=events.filter(e=>e.event==='command-start').map(e=>e.command);
 assert(commands.some(c=>c.some(x=>x.endsWith('/securecutil.c'))),'real native compiler must consume changed source');
 assert(commands.some(c=>c.includes('-o')&&c.some(x=>x.endsWith('libboundscheck.so'))),'real shared link must run');
 assert(commands.some(c=>c.some(x=>x.endsWith('libboundscheck-static.a'))),'second selected native target must rebuild its archive');
 const after=snapshot();assert.equal(after.input,before.input);assert.equal(after.topLevel,before.topLevel);assert.equal(after.cache,before.cache);
 assert.notEqual(after.library,before.library);assert.notEqual(after.archive,before.archive);
 const consumed=consume('new-installed-consume','F4-rebuilt');assert.equal(consumed.status,0,consumed.stderr);
 fs.writeFileSync(path.join(root,'new-result.json'),JSON.stringify({rejected,buildRc:normal.status,consumerRc:consumed.status,before,after,commands},null,2));
 console.log('F4_TARGET_BOUNDARY_RESTORED_REAL_COMPILE_AND_INSTALLED_CONSUMPTION_PASS');
}
