#!/usr/bin/env zx
// Fixed native qualification. Offline apparatus controls are not qualification.
import {mkdirSync, readFileSync, writeFileSync, cpSync, existsSync, readdirSync, statSync, realpathSync, openSync, closeSync} from 'node:fs';
import {resolve, join, dirname, basename, relative} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
import {spawnSync} from 'node:child_process';
import os from 'node:os';

export const PRODUCT='0d6e0888216481c55d1e8b2a0fc2685672289b6a';
export const OBSERVER='9c6d610e9d1b79e7b46025f62617dc6263129d7a';
export const TEST='RuntimeWorkers.ActivePoolBeforeHarnessShutdown';
export const SCRIPTS=['run_other_vm_teardown.sh','check_other_vm_teardown.py','check_teardown_exit.py','check_teardown_records.py'];
const hash=b=>createHash('sha256').update(b).digest('hex');
const json=(p,v)=>writeFileSync(p,JSON.stringify(v,null,2)+'\n');
const text=p=>readFileSync(p,'utf8');
const requireThat=(v,s)=>{if(!v) throw new Error(s);};
export function validate(a) {
  requireThat(/^[0-9a-f]{40}$/.test(a.candidate??''),'full scheduler SHA required');
  requireThat(a.productRef===PRODUCT && a.elfRef===PRODUCT && a.observerRef===OBSERVER,'identity: fixed product/unit and observer refs required');
  requireThat(a.filter===TEST,'exact teardown filter required');
  requireThat(['ubuntu-24.04-arm','ubuntu-26.04'].includes(a.host),'unsupported qualification platform');
  requireThat(!a.reuse,'retained artifact reuse forbidden: this is a new native build');
}
export class Qualification {
  constructor(a) {
    this.args=a; this.evidence=resolve(a.evidence);
    this.product=realpathSync(a.productSource); this.observer=realpathSync(a.observerSource); this.scheduler=realpathSync(a.schedulerSource);
    requireThat(new Set([this.product,this.observer,this.scheduler]).size===3,'identity: three separate checkouts required');
    requireThat(!existsSync(join(this.evidence,'summary.json')),'fresh evidence directory required');
    mkdirSync(this.evidence,{recursive:true}); this.commands=[];
    this.stages=Object.fromEntries(['identity','domain','prepare','candidate','progress-cut','configuration-cut','restored','providers'].map(k=>[k,'NOT_RUN']));
    this.env={...process.env};
    for(const key of ['GC_UNIT_LIST_TESTS','GC_UNIT_ABORT_BEFORE','GC_UNIT_TALLY_FILE']) delete this.env[key];
  }
  save(n,v){json(join(this.evidence,n),v);}
  run(name,command,env=this.env) {
    const log=join(this.evidence,name+'.log'); writeFileSync(log,'COMMAND='+JSON.stringify(command)+'\n');
    const fd=openSync(log,'a'), started=new Date().toISOString(); let p;
    try {p=spawnSync(command[0],command.slice(1).map(String),{env,stdio:['ignore',fd,fd]});} finally {closeSync(fd);}
    this.commands.push({name,command,cwd:process.cwd(),started,ended:new Date().toISOString(),rc:p.status,signal:p.signal,executorError:p.error?.message??null});
    this.save('commands.json',this.commands);
    writeFileSync(join(this.evidence,name+'.rc'),p.status===null?'NOT_RUN executor-error\n':p.status+'\n');
    console.log(name+' rc='+p.status);
    requireThat(!p.error && p.status!==null,name+': executor failure; dependents NOT_RUN'); return p.status;
  }
  need(n,c,e){const rc=this.run(n,c,e); requireThat(rc===0,n+': child rc='+rc+'; dependents NOT_RUN');}
  git(source,...a) {
    const p=spawnSync('git',['-C',source,...a],{encoding:'utf8'});
    requireThat(p.status===0,'git identity rc='+p.status+': '+p.stderr); return p.stdout.trim();
  }
  identities() {
    validate(this.args); const identities={};
    for(const [name,source,expected] of [['product',this.product,PRODUCT],['observer',this.observer,OBSERVER],['scheduler',this.scheduler,this.args.candidate]]) {
      const actual=this.git(source,'rev-parse','HEAD'),tree=this.git(source,'rev-parse','HEAD^{tree}'),dirty=this.git(source,'status','--porcelain','--untracked-files=all');
      identities[name]={source,expected,actual,tree,clean:dirty==='',dirty}; this.save('checkouts.json',identities);
      requireThat(actual===expected && !dirty,'identity: '+name+' SHA/clean mismatch');
    }
    const p=join(this.product,'runtime/tests/gc_unit'),o=join(this.observer,'runtime/tests/gc_unit');
    for(const f of ['run_other_vm_teardown.sh','check_other_vm_teardown.py']) requireThat(readFileSync(join(p,f)).equals(readFileSync(join(o,f))),'identity: shared runner/import byte mismatch '+f);
    this.scripts=join(this.evidence,'scripts'); mkdirSync(this.scripts);
    for(const f of SCRIPTS) cpSync(join(o,f),join(this.scripts,f));
    cpSync(join(p,'run_standalone.sh'),join(this.scripts,'run_standalone.sh'));
    const schedulerScripts=['runtime/tests/teardown_qualification.py','runtime/tests/teardown_qualification.mjs','.github/workflows/arm-unit-ref.yml'];
    const directory=join(this.evidence,'scheduler-scripts'); mkdirSync(directory);
    for(const f of schedulerScripts) cpSync(join(this.scheduler,f),join(directory,basename(f)));
    this.save('scheduler-scripts.json',{checkout:Object.fromEntries(schedulerScripts.map(f=>[f,hash(readFileSync(join(this.scheduler,f)))])),executed:{path:fileURLToPath(import.meta.url),sha256:hash(readFileSync(fileURLToPath(import.meta.url)))}});
    this.save('scripts.json',this.scriptIdentity(this.scripts)); this.stages.identity='PASS';
  }
  scriptIdentity(d){return Object.fromEntries(readdirSync(d).filter(f=>statSync(join(d,f)).isFile()).sort().map(f=>[f,hash(readFileSync(join(d,f)))]));}
  domain() {
    const release=Object.fromEntries(text('/etc/os-release').split('\n').filter(l=>l.includes('=')).map(l=>{const i=l.indexOf('=');return[l.slice(0,i),l.slice(i+1).replace(/^"|"$/g,'')];}));
    const domain={host:this.args.host,machine:os.machine(),kernel:os.release(),release,load:os.loadavg(),cpus:os.availableParallelism(),runner:Object.fromEntries(['RUNNER_OS','RUNNER_ARCH','ImageOS','ImageVersion','GITHUB_RUN_ID','GITHUB_RUN_ATTEMPT'].map(k=>[k,process.env[k]??null]))};
    this.save('domain.json',domain);
    requireThat(release.ID==='ubuntu' && release.VERSION_ID===(this.args.host==='ubuntu-26.04'?'26.04':'24.04'),'domain: actual OS differs from qualification host');
    requireThat(domain.machine===(this.args.host==='ubuntu-24.04-arm'?'aarch64':'x86_64'),'domain: actual native architecture differs');
    this.need('core-limits',['python3','-c','import resource; print(resource.getrlimit(resource.RLIMIT_CORE)); assert resource.getrlimit(resource.RLIMIT_CORE)==(0,0)']);
    this.need('glibc-version',['ldd','--version']); this.stages.domain='PASS';
  }
  plan() {
    const cache=spawnSync('which',['sccache'],{encoding:'utf8',env:this.env}); requireThat(cache.status===0 && cache.stdout.trim(),'sccache executable unavailable'); this.cache=realpathSync(cache.stdout.trim());
    const cxx=this.env.CXX; requireThat(cxx && existsSync(cxx),'standalone compiler-bin CXX wrapper required');
    const wrapper=text(cxx); requireThat(wrapper.includes('exec sccache /usr/bin/clang++ "$@"'),'standalone CXX must use existing sccache wrapper recipe');
    this.save('compiler-wrapper.json',{path:cxx,sha256:hash(readFileSync(cxx)),bytes:wrapper,cache:this.cache});
    const build=join(this.evidence,'build');
    this.configure=['cmake','-S',join(this.product,'runtime'),'-B',build,'-DCMAKE_BUILD_TYPE=Release','-DCOPYGC_FLAG=1','-DDOPRA_FLAG=1','-DRUNTIME_TRACE_FLAG=1','-DCJ_SDK_VERSION=0.0.1','-DDISABLE_VERSION_CHECK=1','-DCMAKE_C_COMPILER=/usr/bin/clang','-DCMAKE_CXX_COMPILER=/usr/bin/clang++','-DCMAKE_AR_PATH=ar','-DCANGJIE_COMPILER_CACHE='+this.cache,...['C','CXX','ASM'].map(l=>'-DCMAKE_'+l+'_COMPILER_LAUNCHER='+this.cache),'-DMRT_TESTABLE_INTERNALS=OFF','-DMRT_GC_UNIT_TESTS=OFF'];
    this.build=['cmake','--build',build,'--target','cangjie-runtime','-j',String(os.availableParallelism())];
    this.save('recipe.json',{product:PRODUCT,observer:OBSERVER,scheduler:this.args.candidate,kind:'NEW_NATIVE_BUILD',configure:this.configure,build:this.build,standalone:['bash',join(this.product,'runtime/tests/gc_unit/run_standalone.sh')],flags:{GC_UNIT_GATE_SKIP:'1',GC_UNIT_BUILD_ONLY:'1'},timeoutMinutes:40,budget:'One Release/default product build, standalone build-only, initial 3-process runner; 3 further exited/live pairs (existing timeout 120s each). No full GC suite.'});
  }
  publication() {
    const entries=[...text(join(this.evidence,'build/CMakeCache.txt')).matchAll(/^OUTPUT_TEMP_PATH:INTERNAL=(.+)$/gm)]; requireThat(entries.length===1,'unique CMake OUTPUT_TEMP_PATH required');
    const root=realpathSync(entries[0][1]),libs=readdirSync(join(root,'lib')).filter(d=>existsSync(join(root,'lib',d,'libcangjie-runtime.so'))); requireThat(libs.length===1,'unique publication library directory required'); return{root,subdir:join('lib',libs[0])};
  }
  prepare() {
    this.plan(); this.env={...this.env,GC_UNIT_GATE_SKIP:'1',GC_UNIT_BUILD_ONLY:'1',GC_UNIT_JOBS:String(os.availableParallelism())};
    this.need('configure',this.configure);
    const cache=text(join(this.evidence,'build/CMakeCache.txt'));
    for(const key of ['CANGJIE_COMPILER_CACHE',...['C','CXX','ASM'].map(l=>'CMAKE_'+l+'_COMPILER_LAUNCHER')]) requireThat(cache.split('\n').some(l=>l.startsWith(key+':') && l.slice(l.indexOf('=')+1)===this.cache),'effective sccache cache/launcher mismatch: '+key);
    this.need('product-build',this.build);
    const p=this.publication(),sos=['libcangjie-runtime.so','libboundscheck.so'].map(n=>join(p.root,p.subdir,n));
    for(const so of sos) requireThat(statSync(so).size>0,'missing product entity '+so);
    this.save('product-at-link.json',Object.fromEntries(sos.map(p=>[p,hash(readFileSync(p))])));
    cpSync(p.root,join(this.evidence,'publication'),{recursive:true}); this.library=join(this.evidence,'publication',p.subdir);
    this.env={...this.env,GCV2_RUNTIME_LIB_DIR:this.library,GCV2_RUNTIME_OUTPUT_ROOT:join(this.evidence,'publication'),LD_LIBRARY_PATH:this.library,GC_UNIT_OUT:join(this.evidence,'standalone')};
    this.need('standalone-build-only',['bash',join(this.product,'runtime/tests/gc_unit/run_standalone.sh')]);
    requireThat(text(join(this.evidence,'standalone-build-only.log')).split('\n').includes('GC_UNIT_BUILD_ONLY_DONE tests_executed=0'),'build-only completion not observed');
    this.elf=join(this.evidence,'standalone/cj_gc_unit'); this.entities=[this.elf,...sos.map(p=>join(this.library,basename(p)))];
    for(const path of this.entities) {const b=readFileSync(path); requireThat(b.length>=20 && b.subarray(0,4).equals(Buffer.from([127,69,76,70])) && b[4]===2 && b[5]===1 && b.readUInt16LE(18)===(this.args.host==='ubuntu-24.04-arm'?183:62),'ELF native machine mismatch: '+path);}
    this.originalEntities=this.entityIdentity(); this.save('entities-before-run.json',this.originalEntities);
    this.need('elf-headers',['readelf','-h','-l',this.elf]); this.need('elf-symbols',['nm','--defined-only','-C',this.elf]); this.need('elf-imports',['nm','-u','-C',this.elf]);
    requireThat(text(join(this.evidence,'elf-symbols.log')).split('\n').filter(l=>/^\S+\s+\S+\s+MapleRuntime::GcUnit::CompleteTestRun\(int\)$/.test(l)).length===1,'unique completion symbol required'); this.stages.prepare='PASS';
  }
  entityIdentity(){return Object.fromEntries(this.entities.map(p=>[p,hash(readFileSync(p))]));}
  unchanged(label,scripts) {const entities=this.entityIdentity(); this.save(label+'-inputs.json',{entities,scripts:this.scriptIdentity(scripts)}); requireThat(JSON.stringify(entities)===JSON.stringify(this.originalEntities),'same-product identity changed');}
  inspectChildren(out) {
    const children=Object.fromEntries(['','-exited','-live'].map(s=>{const p=join(out,'teardown'+s+'.rc'); const v=existsSync(p)?text(p).trim():''; return[s||'gdb',/^\d+$/.test(v)?Number(v):'NOT_RUN'];})); this.save(relative(this.evidence,out)+'-children.json',children);
    requireThat(Object.values(children).every(v=>Number.isInteger(v) && v!==77 && v!==124),'permission/resource/timeout or missing child: NOT_QUALIFIED '+JSON.stringify(children));
    requireThat(children.gdb===0 && children['-exited']===0 && children['-live']===1,'original child exit semantics differ '+JSON.stringify(children)); return children;
  }
  arm(label,scripts,reason=null) {
    this.unchanged(label,scripts); const out=join(this.evidence,label); mkdirSync(out); let aggregate;
    if(label==='candidate') aggregate=this.run(label,['bash',join(scripts,'run_other_vm_teardown.sh'),this.elf,this.library,out]);
    else {
      for(const f of ['teardown.log','teardown.rc']) cpSync(join(this.evidence,'candidate',f),join(out,f));
      this.save(label+'-gdb-reuse.json',{source:'candidate',files:['teardown.log','teardown.rc'],newGdbProcesses:0});
      for(const phase of ['exited','live']) {
        const rc=this.run(label+'-'+phase,['timeout','120','python3',join(scripts,'check_teardown_exit.py'),this.elf,this.library,...(phase==='live'?['--live']:[])]);
        cpSync(join(this.evidence,label+'-'+phase+'.log'),join(out,'teardown-'+phase+'.log')); writeFileSync(join(out,'teardown-'+phase+'.rc'),rc+'\n');
        requireThat(rc===(phase==='live'?1:0),'observer child rc='+rc+'; dependents NOT_RUN');
      }
      aggregate=this.run(label,['python3',join(scripts,'check_teardown_records.py'),out]); cpSync(join(this.evidence,label+'.log'),join(out,'teardown-records.log')); writeFileSync(join(out,'teardown-aggregate.rc'),aggregate+'\n');
    }
    this.providers(label);
    this.inspectChildren(out); const log=text(join(out,'teardown-records.log'));
    if(reason) {requireThat(aggregate===1 && log.includes('ASSERT_TEARDOWN_OBSERVER phase=exited FAIL reason='+reason) && log.includes('TEARDOWN_RECORDS_REJECT exited: hardware observer: '+reason),'cut must reject exactly at observer '+reason); this.stages[label]='EXPECTED_REJECT:'+reason;}
    else {requireThat(aggregate===0 && log.split('\n').some(l=>l.startsWith('TEARDOWN_RECORDS_ACCEPT ')),'normal records rejected; NOT_QUALIFIED'); this.stages[label]='PASS';}
    this.unchanged(label+'-after',scripts);
  }
  batch() {
    this.arm('candidate',this.scripts);
    for(const [label,file,before,after,reason] of [
      ['progress-cut','check_teardown_exit.py','pc=pc, after=after, si_code=code','pc=pc, after=pc, si_code=code','entry-progress'],
      ['configuration-cut','check_teardown_records.py','            configured = addresses\n','            configured = []\n','entry-configuration']]) {
      const scripts=join(this.evidence,label+'-scripts'); cpSync(this.scripts,scripts,{recursive:true}); const path=join(scripts,file),original=text(path);
      requireThat(original.split(before).length===2,'cut source identity mismatch: '+file); writeFileSync(path,original.replace(before,after));
      this.save(label+'-cut.json',{file,observer:OBSERVER,before,after,reason,sourceSha256:hash(Buffer.from(original)),cutSha256:hash(readFileSync(path))}); this.arm(label,scripts,reason);
    }
    this.arm('restored',this.scripts);
    requireThat(JSON.stringify(this.scriptIdentity(this.scripts))===JSON.stringify(JSON.parse(text(join(this.evidence,'scripts.json')))),'restored script identity changed');
  }
  providers(current=null) {
    const directory=join(this.evidence,'providers'); mkdirSync(directory,{recursive:true}); const entities=new Map();
    for(const label of current?[current]:['candidate','progress-cut','configuration-cut','restored']) for(const phase of ['exited','live']) {
      const path=join(this.evidence,label,'teardown-'+phase+'.log');
      if(current && !existsSync(path)) continue;
      const lines=text(path).split('\n'),records=k=>lines.filter(l=>l.startsWith(k+' ')).map(l=>JSON.parse(l.slice(k.length+1)));
      const providers=records('PTHREAD_PROVIDER');
      if(!current) requireThat(providers.length===2,'actual provider records missing');
      for(const p of providers) {
        if(current && !p.path) continue; // Partial records remain diagnostic; final qualification requires entities.
        requireThat(hash(readFileSync(p.path))===p.sha256,'loaded provider entity changed'); entities.set(p.path,p.sha256);
      }
      for(const r of records('PRODUCT_MAPS')) for(const line of r.maps.split('\n')) {
        const path=line.trim().split(/\s+/).slice(5).join(' ');
        if(path.startsWith('/') && /^(libc(?:-[\d.]+)?\.so(?:\.\d+)*|libpthread(?:-[\d.]+)?\.so(?:\.\d+)*|ld-linux[^/]*\.so(?:\.\d+)*|ld-[\d.]+\.so)$/.test(basename(path))) entities.set(path,hash(readFileSync(path)));
      }
    }
    if(!current) requireThat([...entities.keys()].some(p=>basename(p).startsWith('ld-')),'actual loader maps entity missing'); const saved=[];
    for(const [path,sha256] of entities) {
      const dest=join(directory,sha256+'-'+basename(path));
      if(!existsSync(dest)) {cpSync(path,dest); this.need('provider-'+sha256,['readelf','-n','-V',dest]);}
      saved.push({path,sha256,entity:dest});
    }
    this.save(current?current+'-providers.json':'providers.json',saved); if(!current) this.stages.providers='PASS';
  }
  execute() {
    try {
      this.identities();
      requireThat(readFileSync(fileURLToPath(import.meta.url)).equals(readFileSync(join(this.scheduler,'runtime/tests/teardown_qualification.mjs'))),'identity: executed scheduler script byte mismatch');
      if(this.args.validateOnly) return 0; this.domain(); this.prepare(); this.batch(); this.providers();
      for(const source of [this.product,this.observer,this.scheduler]) requireThat(!this.git(source,'status','--porcelain','--untracked-files=all'),'checkout dirtied during qualification');
      this.save('summary.json',{status:'QUALIFIED',stages:this.stages,checkouts:JSON.parse(text(join(this.evidence,'checkouts.json')))});
      writeFileSync(join(this.evidence,'QUALIFICATION_DONE'),'new native product; original child semantics; observer cuts qualified\n'); return 0;
    } catch(error) {this.save('summary.json',{status:'NOT_QUALIFIED',reason:error.message,stages:this.stages,commands:this.commands}); console.error(error.message); return 1;}
  }
}
function main(argv) {
  const args={evidence:'evidence',schedulerSource:resolve(dirname(fileURLToPath(import.meta.url)),'../..')};
  const names={'--candidate':'candidate','--product-ref':'productRef','--elf-ref':'elfRef','--observer-ref':'observerRef','--product-source':'productSource','--observer-source':'observerSource','--scheduler-source':'schedulerSource','--filter':'filter','--host':'host','--evidence':'evidence'};
  try {
    for(let i=0;i<argv.length;i++) {if(argv[i]==='--validate-only') args.validateOnly=true; else {requireThat(names[argv[i]] && argv[i+1],'unsupported/missing argument '+argv[i]); args[names[argv[i]]]=argv[++i];}}
    validate(args); requireThat(args.productSource && args.observerSource,'separate product and observer checkouts required'); return new Qualification(args).execute();
  } catch(error) {console.error(error.message); return 1;}
}
// zx imports the script and keeps its CLI in argv[1]; node puts the script there.
const scriptIndex=process.argv[1] && /[/\\]zx[/\\]build[/\\]cli\.(?:c?js|mjs)$/.test(realpathSync(process.argv[1])) ? 2 : 1;
if(process.argv[scriptIndex] && resolve(process.argv[scriptIndex])===fileURLToPath(import.meta.url)) process.exitCode=main(process.argv.slice(scriptIndex+1));
