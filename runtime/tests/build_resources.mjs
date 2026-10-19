#!/usr/bin/env zx
// Finite CLI/child-argv apparatus; does not qualify a native Windows build or SDK.
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {mkdirSync, readFileSync, writeFileSync, copyFileSync, existsSync} from 'node:fs';
import {resolve, join, dirname, basename} from 'node:path';
import {parseArgs} from 'node:util';
const {values} = parseArgs({options: {source:{type:'string'},work:{type:'string'},select:{type:'string'}}});
if (!values.source || !values.work) throw Error('--source and --work are required');
const source = resolve(values.source), work = resolve(values.work);
mkdirSync(work,{recursive:true});
// Only observations/platform are controlled; entry parsing, jobs resolution and
// child launches execute the supplied product files, never copies of functions.
const launcher = String.raw`
import builtins, io, json, os, platform, runpy, sys
settings = json.loads(os.environ['RESOURCE_OBSERVATIONS'])
if 'system' in settings:
    platform.system = lambda: settings['system']
    platform.machine = lambda: 'x86_64'
    platform.uname = lambda: type('U', (), {'processor': 'x86_64', 'machine': 'x86_64'})()
if 'cpus' in settings:
    os.cpu_count = lambda: settings['cpus']
    if hasattr(os, 'process_cpu_count'):
        os.process_cpu_count = lambda: settings['cpus']
if 'affinity' in settings:
    os.sched_getaffinity = lambda _: set(range(settings['affinity']))
files = settings.get('files', {})
original_open = builtins.open
def observed_open(path, *args, **kwargs):
    name = os.fspath(path)
    if name in files:
        return io.StringIO(files[name])
    if name in ('/proc/self/cgroup', '/proc/self/mountinfo') or name.startswith('/resource-cgroup/'):
        raise FileNotFoundError(name)
    return original_open(path, *args, **kwargs)
builtins.open = observed_open
entry = sys.argv[1]
sys.argv = sys.argv[1:]
runpy.run_path(entry, run_name='__main__')
`;
const tool = String.raw`#!/usr/bin/env node
const fs=require('node:fs'),cp=require('node:child_process'),p=require('node:path');
const name=p.basename(process.argv[1]),args=process.argv.slice(2);
process.on('exit',rc=>fs.appendFileSync(process.env.RESOURCE_TRACE+'.rc',JSON.stringify({name,args,rc})+'\n'));
const keys=['CANGJIE_BUILD_JOBS','CMAKE_BUILD_PARALLEL_LEVEL','KKK2_JOBS'];
fs.appendFileSync(process.env.RESOURCE_TRACE,JSON.stringify({name,args,cwd:process.cwd(),env:Object.fromEntries(keys.map(k=>[k,process.env[k]??null]))})+'\n');
if(name==='ninja' && args.includes('--version')) {console.log('1.11.1');process.exit(process.env.RESOURCE_NINJA==='absent'?1:0);}
if(name==='ninja' && process.env.RESOURCE_NATIVE==='1' && process.cwd().includes('pcre2-build')) {
 const r=cp.spawnSync('/usr/bin/ninja',args,{stdio:'inherit'});process.exit(r.status??125);
}
if(name==='make' && process.env.RESOURCE_NATIVE_MAKE==='1') {
 fs.writeFileSync('Makefile',".PHONY: cangjie-runtime preinstall install\ncangjie-runtime preinstall install:\n\t@printf '%s\\n' '$(MAKEFLAGS)' >> \"$$RESOURCE_NATIVE_FLAGS\"\n");
 const r=cp.spawnSync('/usr/bin/make',args,{stdio:'inherit'});process.exit(r.status??125);
}
if(name==='cmake' && process.env.RESOURCE_NATIVE==='1' && !args.includes('-DTARGET=linux_x86_64_cangjie')) {
 const r=cp.spawnSync('/usr/bin/cmake',['-S',p.join(process.env.RESOURCE_ROOT,'probe'),'-B',p.join(process.env.RESOURCE_ROOT,'probe-build')],{stdio:'inherit'});
 process.exit(r.status??125);
}
if(name==='cmake' && process.env.RESOURCE_KIND==='runtime' && args.includes('-S')) {
 const script=p.join(process.env.RESOURCE_ROOT,'runtime/build/build_cjthread.sh');
 if(fs.existsSync(script)) {
  const r=cp.spawnSync('bash',[script,'-p','linux_x86_64_cangjie','Release','STATIC','','',p.join(process.env.RESOURCE_ROOT,'install'),'x86_64'],{stdio:'inherit'});
  process.exit(r.status??125);
 }
}
process.exit(0);
`;
const v2 = (leaf='100000 100000', parent='max 100000') => ({
 '/proc/self/cgroup':'0::/budget/leaf\n',
 '/proc/self/mountinfo':'29 23 0:26 / /resource-cgroup rw - cgroup2 cgroup rw\n',
 '/resource-cgroup/budget/leaf/cpu.max':leaf,
 '/resource-cgroup/budget/cpu.max':parent,
 '/resource-cgroup/cpu.max':'max 100000',
});
const v1 = {
 '/proc/self/cgroup':'5:cpu,cpuacct:/budget/leaf\n',
 '/proc/self/mountinfo':'29 23 0:26 /budget /resource-cgroup rw - cgroup cgroup rw,cpu,cpuacct\n',
 '/resource-cgroup/leaf/cpu.cfs_quota_us':'250000',
 '/resource-cgroup/leaf/cpu.cfs_period_us':'100000',
 '/resource-cgroup/cpu.cfs_quota_us':'-1',
 '/resource-cgroup/cpu.cfs_period_us':'100000',
};
const cases=[];
for (const kind of ['runtime','stdlib']) {
 const add=(name,expect,options={})=>cases.push({name:kind+'-'+name,kind,expect,...options});
 add('explicit',1,{jobs:'1',env:{CANGJIE_BUILD_JOBS:'bad',CMAKE_BUILD_PARALLEL_LEVEL:'9',KKK2_JOBS:'8'}});
 add('parent-cangjie',3,{env:{CANGJIE_BUILD_JOBS:'3',CMAKE_BUILD_PARALLEL_LEVEL:'2',KKK2_JOBS:'1'}});
 add('parent-cmake',2,{env:{CMAKE_BUILD_PARALLEL_LEVEL:'2',KKK2_JOBS:'1'}});
 add('parent-kkk2',2,{env:{KKK2_JOBS:'2'}});
 add('affinity',1,{observation:{cpus:192}}); // Real wf/taskset affinity, no affinity replacement.
 add('quota-v2',1,{observation:{cpus:192,affinity:4,files:v2()}});
 add('quota-ancestor',1,{observation:{cpus:192,affinity:4,files:v2('max 100000','100000 100000')}});
 add('quota-v1-mounted-subtree',2,{observation:{cpus:192,affinity:4,files:v1}});
 add('quota-fraction',1,{observation:{cpus:192,affinity:4,files:v2('50000 100000')}});
 add('cpu-fallback',4,{observation:{cpus:4,affinity:4}});
 add('zero-default',2,{jobs:'0',env:{CANGJIE_BUILD_JOBS:'2'}});
 add('negative-rejected',null,{jobs:'-1',reject:true});
 add('text-rejected',null,{jobs:'word',reject:true});
 add('invalid-budget',null,{env:{CANGJIE_BUILD_JOBS:'0'},reject:true});
}
cases.push({name:'stdlib-windows-fallback',kind:'stdlib',system:'Windows',ninja:'absent',jobs:'1',expect:1,env:{CANGJIE_BUILD_JOBS:'8'}});
cases.push({name:'stdlib-windows-ninja',kind:'stdlib',system:'Windows',jobs:'2',expect:2,env:{CANGJIE_BUILD_JOBS:'8'}});
cases.push({name:'stdlib-windows-default',kind:'stdlib',system:'Windows',ninja:'absent',expect:1,observation:{cpus:192,affinity:1,files:v2()}});
cases.push({name:'stdlib-native-pcre2',kind:'stdlib',jobs:'2',expect:2,native:true});
cases.push({name:'runtime-native-make',kind:'runtime',jobs:'2',expect:2,nativeMake:true,env:{MAKEFLAGS:'-j97'}});
cases.push({name:'stdlib-resume',kind:'stdlib',jobs:'1',expect:1,resume:true});
const results=[];
for (const c of cases.filter(c=>!values.select || values.select.split(',').includes(c.name))) {
 const dir=join(work,c.name), root=join(dir,'source'), bin=join(dir,'bin'), trace=join(dir,'argv.jsonl');
 assert.ok(!existsSync(dir),'each execution needs a fresh work directory: '+dir);
 mkdirSync(bin,{recursive:true});
 for(const relative of ['runtime/build.py','stdlib/build.py','runtime/build/build_cjthread.sh','runtime/build/build_cjthread_windows.bat','stdlib/third_party/cmake/Pcre2.cmake']) {
  const from=join(source,relative), to=join(root,relative);
  if(existsSync(from)){mkdirSync(dirname(to),{recursive:true});copyFileSync(from,to);}
 }
 for(const name of ['cmake','make','ninja','mingw32-make','clang','clang++','gcc','g++','x86_64-w64-mingw32-gcc.exe','x86_64-w64-mingw32-g++.exe'])writeFileSync(join(bin,name),tool,{mode:0o755});
 const env={...process.env,PATH:bin+':'+process.env.PATH,RESOURCE_ROOT:root,RESOURCE_KIND:c.kind,RESOURCE_TRACE:trace,RESOURCE_NINJA:c.ninja??'present',PYTHONDONTWRITEBYTECODE:'1'};
 for(const k of ['CANGJIE_BUILD_JOBS','CMAKE_BUILD_PARALLEL_LEVEL','KKK2_JOBS','MAKEFLAGS','MFLAGS'])delete env[k];
 Object.assign(env,c.env??{});
 env.RESOURCE_OBSERVATIONS=JSON.stringify({system:c.system??'Linux',...c.observation});
 if(c.nativeMake){env.RESOURCE_NATIVE_MAKE='1';env.RESOURCE_NATIVE_FLAGS=join(dir,'native-make-flags.txt');}
 if(c.native) {
  env.RESOURCE_NATIVE='1';
  mkdirSync(join(root,'probe'),{recursive:true});
  mkdirSync(join(root,'pcre2/pcre2'),{recursive:true});
  writeFileSync(join(root,'probe/CMakeLists.txt'),[
   'cmake_minimum_required(VERSION 3.20)', 'project(ResourceProbe NONE)',
   'set(CANGJIE_PCRE2_SOURCE_DIR "'+root+'/pcre2")',
   'include("'+root+'/stdlib/third_party/cmake/Pcre2.cmake")', ''
  ].join('\n'));
  writeFileSync(join(root,'pcre2/pcre2/CMakeLists.txt'),[
   'cmake_minimum_required(VERSION 3.20)', 'project(Pcre2ResourceProbe NONE)',
   'file(WRITE "'+root+'/pcre2/pcre2/pcre2.h" "/* resource probe */")',
   'add_custom_target(probe ALL COMMAND ${CMAKE_COMMAND} -E echo RESOURCE_NATIVE_BUILD)',
   'install(FILES pcre2.h DESTINATION include)', ''
  ].join('\n'));
 }
 const entry=join(root,c.kind,'build.py');
 const args=['-c',launcher,entry,'build',...(c.kind==='stdlib'?['-t','release']:[]),...(c.jobs!==undefined?['--jobs',c.jobs]:[])];
 const runs=[];
 const run=()=>{
  const r=spawnSync('python3',args,{cwd:dirname(entry),env,encoding:'utf8',timeout:20000});
  runs.push({command:['python3',...args],rc:r.status,signal:r.signal,error:r.error?.message??null,stdout:r.stdout,stderr:r.stderr});
  return r;
 };
 const r=run();
 if(c.resume && r.status===0)run();
 const events=existsSync(trace)?readFileSync(trace,'utf8').trim().split('\n').filter(Boolean).map(x=>JSON.parse(x)):[];
 let error=null;
 try {
  assert.equal(r.error,undefined,'executor completed');
  assert.equal(r.signal,null,'CLI was not killed');
  if(c.reject) {
   assert.ok(Number.isInteger(r.status)&&r.status>0,'invalid jobs rejected');
   assert.equal(events.length,0,'invalid budget rejected before child work');
  } else {
   assert.equal(r.status,0,'CLI completed');
   const builds=events.filter(e=>['make','ninja','mingw32-make'].includes(e.name)&&!e.args.includes('--version')&&!e.args.includes('-t'));
   assert.ok(builds.length>=1,'TARGET_REACHED: build child observed');
   for(const e of builds){
    const j=e.args.find(a=>/^-j\d+$/.test(a))?.slice(2)??e.args[e.args.indexOf('-j')+1];
    assert.equal(j,String(c.expect),'TARGET_JOBS: '+e.name+' '+e.args.join(' '));
   }
   const configured=events.filter(e=>e.name==='cmake');
   assert.ok(configured.length>=1,'configure observed');
   for(const e of events.filter(e=>!e.args.includes('--version')))
    for(const key of ['CANGJIE_BUILD_JOBS','CMAKE_BUILD_PARALLEL_LEVEL','KKK2_JOBS'])
     assert.equal(e.env[key],String(c.expect),'TARGET_BUDGET: '+e.name+' '+key);
   if(c.kind==='runtime' && existsSync(join(root,'runtime/build/build_cjthread.sh'))){
    assert.ok(builds.some(e=>e.cwd.includes('cjthread_build')),'real CJThread script reached');
    assert.ok(builds.some(e=>e.args.includes('preinstall')),'preinstall reached');
   }
   if(c.resume){assert.equal(runs[1].rc,0);assert.equal(configured.length,1,'resume skips configure');assert.equal(builds.length,2,'resume still forwards jobs');}
   if(c.nativeMake){
    const flags=readFileSync(env.RESOURCE_NATIVE_FLAGS,'utf8').trim().split('\n');
    assert.equal(flags.length,4,'CJThread build/install and runtime build/preinstall executed');
    for(const flag of flags)assert.match(flag,/(?:^| )-j2(?: |$)/,'actual GNU make receives selected jobs despite inherited MAKEFLAGS');
   }
  }
 } catch(e){error=e.message;}
 const childResults=existsSync(trace+'.rc')?readFileSync(trace+'.rc','utf8').trim().split('\n').filter(Boolean).map(x=>JSON.parse(x)):[];
 const record={name:c.name,expect:c.expect,assertionReached:!c.reject&&events.some(e=>['make','ninja','mingw32-make'].includes(e.name)&&!e.args.includes('--version')),verdict:error?'FAIL':'PASS',error,runs,events,childResults};
 writeFileSync(join(dir,'result.json'),JSON.stringify(record,null,2)+'\n');
 results.push({...record,runs:record.runs.map(r=>({rc:r.rc,signal:r.signal,error:r.error})),events:undefined});
 console.log(JSON.stringify(results.at(-1)));
}
writeFileSync(join(work,'results.json'),JSON.stringify(results,null,2)+'\n');
process.exitCode=results.some(r=>r.verdict==='FAIL')?1:0;
