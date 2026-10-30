#!/usr/bin/env zx
// Historical replay only. Never invokes measure_r6 run, a compiler or a workload.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {CELLS, compare} from './measure_r6.mjs';
const here = path.dirname(fileURLToPath(import.meta.url));
const [history, r6json, r5json, out] = process.argv.slice(2);
assert(history && r6json && r5json && out, 'HISTORICAL_MEASURE R6_JSON R5_JSON NEW_OUTPUT required');
fs.mkdirSync(out); // Refuse to reuse any earlier evidence directory.
const records = [];
function cli(name, args, rc=0, error=null) {
    const started = new Date().toISOString();
    const p = spawnSync(process.execPath, [path.join(here,'measure_r6.mjs'), ...args], {encoding:'utf8', timeout:30000, maxBuffer:1024*1024});
    fs.writeFileSync(path.join(out,name+'.stdout'),p.stdout || ''); fs.writeFileSync(path.join(out,name+'.stderr'),p.stderr || '');
    const record = {name, argv:[process.execPath,path.join(here,'measure_r6.mjs'),...args], started, ended:new Date().toISOString(), rc:p.status, signal:p.signal, error:p.error?.message??null, expectedRc:rc, target:error?.source??null};
    records.push(record); fs.writeFileSync(path.join(out,'commands.json'),JSON.stringify(records,null,2)+'\n');
    assert.equal(p.error, undefined, name); assert.equal(p.signal,null,name); assert.equal(p.status,rc,p.stderr);
    if (error) assert.match(p.stderr,error,name);
    return rc===0 ? JSON.parse(p.stdout) : null;
}
const expected6 = JSON.parse(fs.readFileSync(r6json));
for (const cell of CELLS) for (let i=1;i<=5;i++) {
    const row = cli(`history-${cell}-${i}`,['sample','official',cell,path.join(history,`official_${cell}`,`r${i}`)]);
    const old = expected6.find(r=>r.arm==='official'&&r.cell===cell&&r.i===i);
    assert.equal(row.stw_ns,Math.round(old.stw_s*1e9)); assert.equal(row.wall,old.wall); assert.equal(row.checksum,old.checksum);
    cli(`old-loader-failure-${cell}-${i}`,['sample','ours',cell,path.join(history,`ours_${cell}`,`r${i}`)],1,/product rc=127/);
}
// r5 raw measure directory is absent. Only replay its retained measured summary
// through the noise formula; do not present these scalars as raw-log validation.
const r5 = JSON.parse(fs.readFileSync(r5json)), scalar = {};
for (const cell of CELLS) {
    const off=r5.filter(r=>r.arm==='official'&&r.cell===cell), sub=r5.filter(r=>r.arm==='ours'&&r.cell===cell);
    scalar[cell]={wall:compare(off.map(r=>r.wall),sub.map(r=>r.wall)), stw:compare(off.map(r=>r.stw_s),sub.map(r=>r.stw_s))};
    assert.equal(scalar[cell].wall.verdict,cell.startsWith('NW')?'better':'noise-tie');
    assert.equal(scalar[cell].stw.verdict,'better');
}
assert.equal(compare([1,1,1,1,1],[1,1,1,1,1]).verdict,'noise-tie');
assert.equal(compare([1,1,1,1,1],[2,2,2,2,2]).verdict,'worse');
assert.equal(compare([1,1,1,1,1],[0,0,0,0,1]).verdict,'noise-tie');
assert.throws(()=>compare([1],[1]),/N>=5/);
fs.writeFileSync(path.join(out,'r5-scalar-replay.json'),JSON.stringify(scalar,null,2)+'\n');

const source = path.join(history,'official_NW256','r1');
function copy(name, edits={}) {
    const d=path.join(out,name); fs.mkdirSync(d);
    for(const f of ['stdout.txt','stderr.txt','mrt_report.txt','time.txt','rc','uptime-before.txt','uptime-after.txt']) fs.copyFileSync(path.join(source,f),path.join(d,f));
    for(const [file,edit] of Object.entries(edits)) { const p=path.join(d,file); fs.writeFileSync(p,edit(fs.readFileSync(p,'utf8'))); }
    return d;
}
const mutations = [
    ['timer-missing',{'mrt_report.txt':s=>s.replace(/^.*Preforward time:.*\n/m,'')},/incomplete four-timer/],
    ['timer-truncated',{'mrt_report.txt':s=>s.slice(0,s.lastIndexOf('2026-09-29'))},/missing\/truncated cycle/],
    ['timer-bad-unit',{'mrt_report.txt':s=>s.replace('5,553us','5,553ms')},/malformed microseconds/],
    ['timer-bad-comma',{'mrt_report.txt':s=>s.replace('5,553us','55,53us')},/malformed microseconds/],
    ['timer-duplicate',{'mrt_report.txt':s=>s.replace(/^(.*Preforward time:.*)$/m,'$1\n$1')},/unexpected\/duplicate timer/],
    ['timer-orphan',{'mrt_report.txt':s=>s.replace(/^.*Begin GC log\..*\n/m,'')},/missing cycle begin/],
    ['wave-missing',{'stdout.txt':s=>s.replace(/^WAVE_DONE wave=4 .*\n/m,'')},/exactly 12/],
    ['wave-wrong',{'stdout.txt':s=>s.replace('wave=8 checksum=352488429869400','wave=8 checksum=352488429869401')},/first_div=8/],
    ['marker-wrong',{'stdout.txt':s=>s.replace('NATURAL_WAVE_OK checksum=635925223159200','NATURAL_WAVE_OK checksum=0')},/final mismatch/],
    ['time-wrong',{'time.txt':s=>s.replace('time_exit=0','time_exit=124')},/time fields or time_exit/],
    ['signal',{'rc':()=> '139\n'},/product rc=139/],
    ['no-work',{'stdout.txt':()=> 'NATURAL_WAVE_OK checksum=635925223159200\n'},/exactly 12/],
    ['load',{'uptime-after.txt':s=>s.replace(/load average: [0-9.]+/,'load average: 51.00')},/load1>50/]
];
for(const [name,edits,why] of mutations) {
    cli(name,['sample','official','NW256',copy(name,edits)],1,why);
    cli(name+'-independent-SD',['sample','official','SD512',path.join(history,'official_SD512','r1')]);
    cli(name+'-restore',['sample','official','NW256',source]);
}
// Deliberately synthetic current-schema control based on existing
// test_gclog_schema.py complete_rows, not an observed subject performance sample.
const fixture = [
    '[GCLOG] v=6 rec=cycle seq=1 gc_tag=- name=Minor_Collection cause=young event=start',
    '[GCLOG] v=6 rec=generation seq=1 gc_tag=y name=Young_Generation event=start',
    ...[['Pause_Mark_Start','pause'],['Concurrent_Mark','conc'],['Pause_Mark_End','pause'],['Concurrent_Mark_Free','conc'],
        ['Concurrent_Reset_Relocation_Set','conc'],['Concurrent_Select_Relocation_Set','conc'],['Pause_Relocate_Start','pause'],['Concurrent_Relocate','conc']]
        .map(([name,kind],i)=>`[GCLOG] v=5 rec=phase seq=1 gc_tag=y name=${name} kind=${kind} start_ns=${i+2} ns=1`),
    '[GCLOG] v=5 rec=stw seq=1 gc_tag=y reason=Pause_Mark_Start start_ns=2 wait_ns=8 held_ns=99',
    '[GCLOG] v=6 rec=generation seq=1 gc_tag=y name=Young_Generation event=end start_ns=1 dur_ns=99 used_at_collection_start=9 used_at_collection_end=8',
    '[GCLOG] v=6 rec=cycle seq=1 gc_tag=- name=Minor_Collection cause=young event=end start_ns=1 dur_ns=100 used_at_start=9 used_at_end=8'
].join('\n')+'\n';
const sub = copy('synthetic-subject',{'stderr.txt':()=>fixture});
const parsed = cli('subject-phase-not-held',['sample','ours','NW256',sub]);
assert.equal(parsed.stw_ns,3); assert.equal(parsed.npause,3); // Not 99 or 107 or cycle wall100.
const cut = copy('synthetic-subject-cut',{'stderr.txt':()=>fixture.replace(/^.*name=Pause_Relocate_Start .*\n/m,'')});
cli('subject-missing-phase',['sample','ours','NW256',cut],1,/missing phase Pause_Relocate_Start/);
const tail = copy('synthetic-subject-tail',{'stderr.txt':()=>fixture.replace(/^.*rec=cycle.*event=end.*\n/m,'')});
cli('subject-truncated',['sample','ours','NW256',tail],1,/aborted\/truncated ledger/);
cli('subject-restored',['sample','ours','NW256',sub]);

const c = {n:5,output:'/not-executed/output with spaces',cwd:'/not-executed/cwd',cores:{256:'144-159',512:'160-175'},
    official:{NW:'/not-executed/official NW',SD:'/not-executed/official SD',library:'/not-executed/official lib'},
    ours:{NW:'/not-executed/ours NW',SD:'/not-executed/ours SD',library:'/not-executed/ours lib'}};
const cp=path.join(out,'plan-input.json');fs.writeFileSync(cp,JSON.stringify(c));
const schedule=cli('plan',['plan',cp]);assert.equal(schedule.length,40);
assert.deepEqual(schedule.map(r=>`${r.cell}/${r.i}/${r.arm}`),CELLS.flatMap(cell=>Array.from({length:5},(_,i)=>['official','ours'].map(a=>`${cell}/${i+1}/${a}`)).flat()));
for(const row of schedule) {
    assert.equal(row.argv.filter(x=>x==='cjConcGCThreads=4').length,row.arm==='ours'?1:0);
    assert.equal(row.argv.filter(x=>x==='MRT_GC_LOG=1').length,row.arm==='ours'?1:0);
    assert.equal(row.argv.filter(x=>x==='MRT_LOG_LEVEL=v').length,row.arm==='official'?1:0);
    assert(row.argv.includes('-i')); assert(row.argv.includes('300')); assert(row.argv.includes(row.cores));
    assert.equal(row.argv.at(-1),c[row.arm][row.cell.slice(0,2)]);
}
const badcp=path.join(out,'bad-plan-input.json');fs.writeFileSync(badcp,JSON.stringify({...c,n:4}));
cli('plan-reject-N4',['plan',badcp],1,/N>=5/);
cli('historical-whole-matrix-reject',['summarize',history,'5'],1,/product rc=127/);
fs.writeFileSync(path.join(out,'result.json'),JSON.stringify({rc:0,cliCases:records.length,historicalOfficial:20,historicalLoaderReject:20,
    targetCuts:mutations.length+2,scope:'parser/plan only; subject positive is synthetic; no run or compiler invoked',r5RawDirectory:'NOT_FOUND; scalar replay only',realPerformance:'NOT_RUN',minorC3:'UNMET'},null,2)+'\n');
console.log('HISTORICAL_REPLAY_AND_REJECTION_PASS '+records.length);
