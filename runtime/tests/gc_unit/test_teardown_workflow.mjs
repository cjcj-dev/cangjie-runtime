#!/usr/bin/env zx
// Execute the existing workflow's actual validation/qualification shell context.
// A foreign host must stop before any native build, even with valid fixed refs.
import {readFileSync,writeFileSync,mkdirSync,existsSync} from 'node:fs';
import {resolve,join} from 'node:path';
import {spawnSync} from 'node:child_process';
const [rootArg,sourcesArg]=process.argv.slice(2);
if(!rootArg || !sourcesArg) throw new Error('usage: zx test_teardown_workflow.mjs EVIDENCE SOURCES');
const work=resolve(rootArg),sources=resolve(sourcesArg),scheduler=join(sources,'scheduler');mkdirSync(work,{recursive:true});
const records=[],checks=[];
function run(name,cmd,args,cwd=work,env=process.env) {
 const p=spawnSync(cmd,args,{cwd,env,encoding:'utf8',timeout:20000});
 writeFileSync(join(work,name+'.log'),(p.stdout??'')+(p.stderr??''));
 writeFileSync(join(work,name+'.rc'),p.status===null?'NOT_RUN executor-error\n':p.status+'\n');
 records.push({name,cmd,args,cwd,rc:p.status,signal:p.signal,error:p.error?.message});writeFileSync(join(work,'commands.json'),JSON.stringify(records,null,2));return p;
}
function check(name,ok,detail=''){checks.push({name,pass:!!ok,detail});console.log('ASSERT '+name+' '+(ok?'PASS':'FAIL')+' '+detail);}
const parsed=run('workflow-parse','/usr/bin/python3',['-c','import json,sys,yaml; print(json.dumps(yaml.safe_load(open(sys.argv[1]))))',join(scheduler,'.github/workflows/arm-unit-ref.yml')]);
if(parsed.status!==0) throw new Error('workflow parsing failed before execution');
const job=JSON.parse(parsed.stdout).jobs['teardown-qualification-1426'];
const fixed='0d6e0888216481c55d1e8b2a0fc2685672289b6a',observer='9c6d610e9d1b79e7b46025f62617dc6263129d7a';
const candidate=run('scheduler-ref','git',['-C',scheduler,'rev-parse','HEAD']).stdout.trim();
const preflight=job.steps.find(s=>s.name==='Validate fixed identities and teardown-only entry');
const native=job.steps.find(s=>s.name==='Build new product once and execute finite observer batch');
check('workflow.platforms',JSON.stringify(job.strategy.matrix.host)===JSON.stringify(['ubuntu-24.04-arm','ubuntu-26.04']));
check('workflow.original-budget',job['timeout-minutes']===40);
check('workflow.no-retained-download',!job.steps.some(s=>s.uses?.startsWith('actions/download-artifact')));
check('workflow.full-evidence',job.steps.some(s=>s.uses==='actions/upload-artifact@v4' && s.if==='always()' && s.with.path==='evidence/'));
for(const [name,wrong] of [['normal',false],['wrong-ref',true]]) {
 const cwd=join(work,name);mkdirSync(cwd);
 for(const [from,to] of [['scheduler','scheduler-source'],['product','product-source'],['observer','observer-source']]) {
  const p=run(name+'-clone-'+from,'git',['clone','-q','--shared',join(sources,from),join(cwd,to)]);if(p.status!==0)throw new Error('source preparation failed');
 }
 const env={...process.env,CANDIDATE:candidate,PRODUCT_REF:fixed,OBSERVER_REF:observer,TEST_FILTER:'RuntimeWorkers.ActivePoolBeforeHarnessShutdown',ELF_REF:wrong?observer:fixed,REQUEST_PRODUCT_REF:fixed};
 const body=preflight.run.replaceAll('${{ matrix.host }}','ubuntu-26.04');writeFileSync(join(work,name+'-preflight-command.txt'),body);
 const p=run(name+'-preflight','/bin/bash',['-e','-o','pipefail','-c',body],cwd,env);
 const identity=join(cwd,'evidence/preflight/checkouts.json');
 const entered=existsSync(identity) && Object.keys(JSON.parse(readFileSync(identity))).length===3;
 check('workflow.'+name+'.identity-entry',p.status===(wrong?1:0) && (wrong || entered) && !existsSync(join(cwd,'evidence/preflight/QUALIFICATION_DONE')),`rc=${p.status} entered=${entered}`);
 if(!wrong) {
   const body=native.run.replaceAll('${{ matrix.host }}','ubuntu-26.04');writeFileSync(join(work,'native-command.txt'),body);
   const p=run('native-foreign-domain','/bin/bash',['-e','-o','pipefail','-c',body],cwd,env);
   const summary=JSON.parse(readFileSync(join(cwd,'evidence/native/summary.json')));
   check('workflow.native-domain-rejected-before-build',p.status===1 && summary.status==='NOT_QUALIFIED' && summary.reason.startsWith('domain:') && summary.stages.prepare==='NOT_RUN' && readFileSync(join(cwd,'evidence/qualification.rc'),'utf8')==='1\n' && !existsSync(join(cwd,'evidence/native/QUALIFICATION_DONE')),JSON.stringify(summary));
 }
}
writeFileSync(join(work,'assertions.json'),JSON.stringify({scope:'workflow context/identity/foreign-domain only; native qualification NOT_RUN',checks},null,2));
console.log('WORKFLOW_ASSERTIONS total='+checks.length+' passed='+checks.filter(c=>c.pass).length+' failed='+checks.filter(c=>!c.pass).length);process.exitCode=checks.every(c=>c.pass)?0:1;
