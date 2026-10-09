#!/usr/bin/env node
// Controlled integration of the real workflow shell and its real filtered runner.
// Native builds are recorded at the subprocess boundary, never performed here.
import { readFileSync, writeFileSync, mkdirSync, cpSync, chmodSync, existsSync, readdirSync } from 'node:fs';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../..');
const evidence = resolve(process.argv[2] ?? '');
if (!process.argv[2]) throw new Error('usage: node test_arm_unit_entry.mjs EVIDENCE_DIR');
mkdirSync(evidence, {recursive: true});
const records = [], checks = [];
function run(name, cmd, args, cwd, extra = {}) {
  const env = {...process.env, ...extra};
  const p = spawnSync(cmd, args, {cwd, env, encoding: 'utf8', timeout: 20000});
  writeFileSync(join(evidence, name + '.log'), (p.stdout ?? '') + (p.stderr ?? ''));
  writeFileSync(join(evidence, name + '.rc'), String(p.status) + '\n');
  records.push({name, cmd, args, cwd, rc: p.status, error: p.error?.message});
  writeFileSync(join(evidence, 'commands.json'), JSON.stringify(records, null, 2));
  return p;
}
function check(name, value, detail = '') {
  checks.push({name, pass: !!value, detail});
  console.log(`ASSERT ${name} ${value ? 'PASS' : 'FAIL'} ${detail}`);
}
function text(path) {return existsSync(path) ? readFileSync(path, 'utf8') : '';}
const yaml = run('yaml', 'python3', ['-c', 'import json,sys,yaml; print(json.dumps(yaml.safe_load(open(sys.argv[1]))))', join(root, '.github/workflows/arm-unit-ref.yml')], root);
if (yaml.status !== 0) throw new Error('workflow parsing failed');
const workflow = JSON.parse(yaml.stdout);
const body = workflow.jobs.unit.steps.find(s => s.name === 'Build and execute real standalone entry').run;
const work = join(evidence, 'workflow');
mkdirSync(work, {recursive: true});
for (const path of ['bin', 'elf-source/runtime/tests/gc_unit', 'product-source/runtime', 'runtime/tests/gc_unit']) mkdirSync(join(work, path), {recursive:true});
cpSync(join(root, 'runtime/tests/gc_unit/run_parallel_tests.sh'), join(work, 'runtime/tests/gc_unit/run_parallel_tests.sh'));
const fixture = `#!/usr/bin/env node
const fs = require('node:fs');
const a = process.argv[2];
const names = process.argv[1].endsWith('cj_gc_unit') ? ['Main.One','Main.Fail','Main.Hang','Main.Incomplete'] : ['Publication.One','Publication.Two'];
if (a === '--gtest_list_tests') { for(const n of names) {const [s,t]=n.split('.'); console.log(s+'.\\n  '+t);} process.exit(0); }
const n = a?.replace('--gtest_filter=', '');
if(!names.includes(n)) process.exit(3);
fs.appendFileSync(process.env.CALL_LOG, n+'\\n');
console.log('[  RUN   ] '+n);
if(n === 'Main.Hang') {setTimeout(()=>{},20000);} else {
 const fail = n === 'Main.Fail';
 console.log(fail ? '[  FAIL  ] '+n : '[  PASS  ] '+n);
 if(n !== 'Main.Incomplete') fs.writeFileSync(process.env.GC_UNIT_TALLY_FILE, fail ? '[========] 1 tests: 0 passed, 1 failed\\n' : '[========] 1 tests: 1 passed, 0 failed\\n');
 process.exit(fail ? 7 : 0);
}
`;
writeFileSync(join(work, 'fixture'), fixture);
// A subprocess recorder refuses an accidental build-and-run call before any
// suite can execute. The shell under test is extracted unchanged from YAML.
const shim = `#!/usr/bin/env node
const fs=require('node:fs'), path=require('node:path'), cp=require('node:child_process');
const cmd=path.basename(process.argv[1]), a=process.argv.slice(2);
if(cmd==='cmake') {
 if(a.includes('-B')) {const b=a[a.indexOf('-B')+1], r=path.resolve(b+'/publication'); fs.mkdirSync(r+'/lib/native',{recursive:true}); for(const n of ['libcangjie-runtime.so','libboundscheck.so']) fs.writeFileSync(r+'/lib/native/'+n,'controlled input'); fs.writeFileSync(b+'/CMakeCache.txt','OUTPUT_TEMP_PATH:INTERNAL='+r+'\\n');}
 process.exit(0);
}
if(cmd==='git') {console.log('0d6e0888216481c55d1e8b2a0fc2685672289b6a'); process.exit(0);}
if(cmd==='nm') {console.log('controlled-symbol'); process.exit(0);}
if(cmd==='bash' && a[0].endsWith('/run_standalone.sh')) {
 fs.appendFileSync(process.env.BUILD_CALL_LOG, JSON.stringify({command:a,buildOnly:process.env.GC_UNIT_BUILD_ONLY??null})+'\\n');
 if(process.env.GC_UNIT_BUILD_ONLY!=='1') {console.error('UNEXPECTED_FULL_SUITE_CALL refused before execution'); process.exit(86);}
 const out=process.env.GC_UNIT_OUT; fs.mkdirSync(out,{recursive:true});
 for(const n of ['cj_gc_unit','cj_gc_forwarding_publication_unit']) {fs.copyFileSync('fixture',out+'/'+n); fs.chmodSync(out+'/'+n,0o755);}
 console.log('GC_UNIT_BUILD_ONLY_DONE tests_executed=0'); process.exit(0);
}
const p=cp.spawnSync('/bin/bash',a,{stdio:'inherit'}); process.exit(p.status??125);
`;
for (const command of ['cmake', 'git', 'nm', 'bash']) {writeFileSync(join(work,'bin',command), shim); chmodSync(join(work,'bin',command),0o755);}
const calls = join(work,'calls.log'), buildCalls = join(work,'build-calls.log');
const entry = run('entry', '/bin/bash', ['-e', '-o', 'pipefail', '-c', body], work,
 {PATH:join(work,'bin')+':'+process.env.PATH, TEST_FILTER:'Publication.One:Main.One', CALL_LOG:calls, BUILD_CALL_LOG:buildCalls, GC_UNIT_TEST_TIMEOUT:'1'});
const dispatch = text(buildCalls).trim().split('\n').filter(Boolean).map(JSON.parse);
check('entry.build_only_dispatch', dispatch.length===1 && dispatch[0].buildOnly==='1', JSON.stringify(dispatch));
check('entry.exact_ordered_selection', text(calls)==='Publication.One\nMain.One\n', text(calls).trim());
check('entry.completion', entry.status===0 && text(join(work,'evidence/standalone/parallel_tally.txt'))==='[========] 2 tests: 2 passed, 0 failed\n', `rc=${entry.status}`);
// Directly invoke the same production consumer with real subprocess fixtures.
// Every invariant is evaluated even when an earlier invariant turns red.
for (const [name, selected, rc, states, expectedCalls] of [
 ['selected','Publication.Two:Main.One',0,['PASS','PASS'],'Publication.Two\nMain.One\n'],
 ['failure','Main.Fail:Publication.One',1,['FAIL','NOT_RUN'],'Main.Fail\n'],
 ['timeout','Main.Hang:Publication.One',1,['TIMEOUT','NOT_RUN'],'Main.Hang\n'],
 ['incomplete','Main.Incomplete:Publication.One',1,['INCOMPLETE','NOT_RUN'],'Main.Incomplete\n'],
 ['unknown','Main.One:Missing.Name',2,[],''],
 ['duplicate','Main.One:Main.One',2,[],''],
 ['wildcard','Main.*',2,[],''],
 ['empty-name','Main.One:',2,[],''],
 ]) {
 const out=join(evidence,name), call=join(evidence,name+'.calls');
 const main=join(work,'cj_gc_unit'), pub=join(work,'cj_gc_forwarding_publication_unit');
 for(const elf of [main,pub]) {cpSync(join(work,'fixture'),elf); chmodSync(elf,0o755);}
 const p=run(name,'/bin/bash',[join(root,'runtime/tests/gc_unit/run_parallel_tests.sh'),main,pub,out,work],root,
 {GC_UNIT_TEST_FILTER:selected,GC_UNIT_TEST_TIMEOUT:'1',GC_UNIT_JOBS:'3',CALL_LOG:call});
 const actualStates=existsSync(join(out,'test-status')) ? readdirSync(join(out,'test-status')).sort().map(n=>text(join(out,'test-status',n)).trim()) : [];
 check('runner.'+name+'.selection', text(call)===expectedCalls, text(call).trim());
 check('runner.'+name+'.status', p.status===rc && JSON.stringify(actualStates)===JSON.stringify(states), `rc=${p.status} states=${JSON.stringify(actualStates)}`);
 if(name==='failure') check('runner.failure.own_rc',text(join(out,'test-rc/000000-main.rc'))==='7\n' && !existsSync(join(out,'test-rc/000001-publication.rc')));
 if(name==='timeout') check('runner.timeout.own_rc',text(join(out,'test-rc/000000-main.rc'))==='124\n' && !existsSync(join(out,'test-rc/000001-publication.rc')));
}
const sha='0d6e0888216481c55d1e8b2a0fc2685672289b6a';
const rootFilter='ZRootTask.YoungCarrierPublishesOnlyYoung:ZRootTask.YoungCarrierParallelDispatch:ZRootTask.OldCarrierPublishesBothGenerations:ZRootTask.OldCarrierParallelDispatch';
for(const [script, valid] of [['arm_root_qualification.py',rootFilter],['teardown_qualification.py','RuntimeWorkers.ActivePoolBeforeHarnessShutdown']]) {
 const args=['--candidate',sha,'--validate-only','--filter'];
 const entry=script==='teardown_qualification.py' ? ['node',['--input-type=module','-e',`import {validate,PRODUCT,OBSERVER} from ${JSON.stringify(join(root,'runtime/tests/teardown_qualification.mjs'))}; validate({candidate:${JSON.stringify(sha)},productRef:PRODUCT,elfRef:PRODUCT,observerRef:OBSERVER,host:'ubuntu-26.04',filter:process.argv[1]}); console.log('TEARDOWN_FILTER_VALIDATED '+process.argv[1]);`]] : ['python3',[join(root,'runtime/tests',script),...args]];
 const good=run(script+'.valid',entry[0],[...entry[1],valid],root);
 const bad=run(script+'.invalid',entry[0],[...entry[1],'Publication.One:Main.One'],root);
 check('specialized.'+script,good.status===0 && bad.status===1,`valid=${good.status} unrelated=${bad.status}`);
 if(script==='teardown_qualification.py') check('specialized.teardown.validate-target',good.stdout.includes('TEARDOWN_FILTER_VALIDATED '+valid) && bad.stderr.includes('exact teardown filter required') && !bad.stderr.includes('ENOENT'),`valid=${good.status} unrelated=${bad.status} diagnostic=${bad.stderr.trim()}`);
}
for(const [name,args] of [['no-argv',[]],['missing-path',['missing-caller']],['existing-caller',[join(root,'runtime/tests/gc_unit/test_arm_unit_entry.mjs'),join(root,'runtime/tests/teardown_qualification.mjs')]]]) {
 const p=run('import.'+name,'node',['--input-type=module','-e',`await import(${JSON.stringify(join(root,'runtime/tests/teardown_qualification.mjs'))}); console.log('LIBRARY_IMPORT_ONLY');`,...args],root);
 check('import.'+name,p.status===0 && p.stdout.trim()==='LIBRARY_IMPORT_ONLY' && !p.stderr,`rc=${p.status} stderr=${p.stderr.trim()}`);
}
check('workflow.native_arm',workflow.jobs.unit['runs-on']==='ubuntu-24.04-arm');
check('workflow.sccache',workflow.jobs.unit.steps.some(s=>s.uses==='mozilla-actions/sccache-action@v0.0.10'));
writeFileSync(join(evidence,'assertions.json'),JSON.stringify(checks,null,2));
console.log(`ENTRY_ASSERTIONS total=${checks.length} passed=${checks.filter(c=>c.pass).length} failed=${checks.filter(c=>!c.pass).length}`);
process.exitCode=checks.every(c=>c.pass)?0:1;
