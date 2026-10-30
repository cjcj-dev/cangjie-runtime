#!/usr/bin/env zx
// TASK0226: actual public run entry, short fixtures only, never NW/SD programs.
// Required real scope: CPUs144-159, quota400%, RAM4GiB, Swap0/core0, <=120s.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

const here=path.dirname(fileURLToPath(import.meta.url));
const [output]=process.argv.slice(2);
assert(output && path.isAbsolute(output), 'new absolute fixture output required');
fs.mkdirSync(output); // Keep all earlier runs and fail if reused.
const records=[];
const save=(p,v)=>fs.writeFileSync(p,JSON.stringify(v,null,2)+'\n',{flag:'wx'});

// Written as separate executable .mjs files with the actual Node interpreter.
// These fixtures label their synthetic checksums/timers and use sleep, not a
// GC workload. Library-name files are identity-reader fixtures, not ELF DSOs.
function fixture(p) {
    const cg='/sys/fs/cgroup'+fs.readFileSync('/proc/self/cgroup','utf8').trim().split('::')[1];
    const cpus=fs.readFileSync('/proc/self/status','utf8').match(/^Cpus_allowed_list:\s*(.*)$/m)[1];
    const envKeys=['PATH','HOME','cjHeapSize','LD_LIBRARY_PATH','cjConcGCThreads','MRT_GC_LOG','MRT_LOG_LEVEL','MRT_REPORT'];
    const env=Object.fromEntries(envKeys.map(k=>[k,process.env[k]??null]));
    const observed={kind:'SYNTHETIC_RUN_ENTRY_FIXTURE_NOT_NW_SD',pid:process.pid,ppid:process.ppid,
        executable:fs.readlinkSync('/proc/self/exe'),argv:process.argv,cwd:process.cwd(),cpus,
        cgroup:cg,quota:fs.readFileSync(cg+'/cpu.max','utf8').trim(),
        memory:fs.readFileSync(cg+'/memory.max','utf8').trim(),swap:fs.readFileSync(cg+'/memory.swap.max','utf8').trim(),
        limits:fs.readFileSync('/proc/self/limits','utf8'),env,arm:p.arm,family:p.family,
        parentPoisonPresent:process.env.R6_ENTRY_POISON!==undefined,hostHomePresent:process.env.CANGJIE_HOME!==undefined};
    fs.appendFileSync(p.events,JSON.stringify({event:'start',...observed})+'\n');
    assert.equal(cpus,'144-159'); assert.equal(observed.quota,'400000 100000');
    assert.equal(observed.memory,'4294967296'); assert.equal(observed.swap,'0');
    assert.match(observed.limits,/Max core file size\s+0\s+0/);
    assert.equal(process.cwd(),p.cwd); assert.equal(env.PATH,'/usr/bin:/bin');assert.equal(env.HOME,'/root');
    assert(['256MB','512MB'].includes(env.cjHeapSize));assert.equal(env.LD_LIBRARY_PATH,p.library);
    assert(!observed.parentPoisonPresent && !observed.hostHomePresent);
    if(p.arm==='official') {
        assert.equal(env.MRT_LOG_LEVEL,'v');assert.equal(env.cjConcGCThreads,null);assert.equal(env.MRT_GC_LOG,null);
        assert.equal(env.MRT_REPORT,path.join(p.runOutput,'measure',`official_${p.family}${env.cjHeapSize.slice(0,-2)}`,`r${p.sampleFromReport(env.MRT_REPORT)}`,'mrt_report.txt'));
    } else {
        assert.equal(env.cjConcGCThreads,'4');assert.equal(env.MRT_GC_LOG,'1');assert.equal(env.MRT_LOG_LEVEL,null);assert.equal(env.MRT_REPORT,null);
    }
    fs.writeSync(1,'R6_ENTRY_FIXTURE synthetic=true not_a_workload=true\n');
    if(p.arm==='ours' && p.mode==='exit7') {
        fs.appendFileSync(p.events,JSON.stringify({event:'normal-exit-request',pid:process.pid,code:7})+'\n');
        process.exit(7);
    }
    if(p.arm==='ours' && p.mode==='signal') {
        fs.appendFileSync(p.events,JSON.stringify({event:'self-signal-request',pid:process.pid,signal:'SIGTERM'})+'\n');
        process.kill(process.pid,'SIGTERM');
        throw Error('self SIGTERM unexpectedly returned to JS');
    }
    // Deliberate latency separation is test data, NOT a runtime speed comparison.
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)),0,0,p.arm==='official'?180:20);
    if(p.family==='NW') {
        const gold=['2289681096600','13798287193200','34525818289800','64472274386400','103637655483000','152021961579600',
            '209625192676200','276447348772800','352488429869400','437748435966000','532227367062600','635925223159200'];
        for(let i=0;i<gold.length;i++)fs.writeSync(1,`WAVE_DONE wave=${i} checksum=${gold[i]}\n`);
        fs.writeSync(1,'NATURAL_WAVE_OK checksum=635925223159200 fixture=true\n');
    } else fs.writeSync(1,'SURVIVAL_DENSE_OK checksum=368685940367600 fixture=true\n');
    if(p.arm==='official') {
        const prefix='2026-10-31 00:00:00.000000 '+process.pid+' ';
        fs.writeFileSync(env.MRT_REPORT,[
            'Begin GC log. GCReason: fixture, synthetic=true',
            'enum roots & update old pointers within time: 200,000us',
            'MarkSatbBuffer time: 200,000us','Preforward time: 200,000us','ForwardFromRegions time: 200,000us',
            'total gc time: 800,000 us, collection rate synthetic'
        ].map(l=>prefix+l).join('\n')+'\n');
    } else {
        const rows=[
            '[GCLOG] v=6 rec=cycle seq=1 gc_tag=- name=Minor_Collection cause=fixture event=start',
            '[GCLOG] v=6 rec=generation seq=1 gc_tag=y name=Young_Generation event=start',
            ...[['Pause_Mark_Start','pause'],['Concurrent_Mark','conc'],['Pause_Mark_End','pause'],['Concurrent_Mark_Free','conc'],
                ['Concurrent_Reset_Relocation_Set','conc'],['Concurrent_Select_Relocation_Set','conc'],['Pause_Relocate_Start','pause'],['Concurrent_Relocate','conc']]
                .map(([name,kind],i)=>`[GCLOG] v=5 rec=phase seq=1 gc_tag=y name=${name} kind=${kind} start_ns=${i+2} ns=1`),
            '[GCLOG] v=6 rec=generation seq=1 gc_tag=y name=Young_Generation event=end start_ns=1 dur_ns=99 used_at_collection_start=9 used_at_collection_end=8',
            '[GCLOG] v=6 rec=cycle seq=1 gc_tag=- name=Minor_Collection cause=fixture event=end start_ns=1 dur_ns=100 used_at_start=9 used_at_end=8'
        ];
        fs.writeSync(2,rows.join('\n')+'\n');
    }
    fs.appendFileSync(p.events,JSON.stringify({event:'normal-exit-request',pid:process.pid,code:0})+'\n');
}

function scenario(name,mode,cores='144-159') {
    const base=path.join(output,name),cwd=path.join(base,'cwd with spaces'),runOutput=path.join(base,'run output');
    fs.mkdirSync(cwd,{recursive:true});const events=path.join(base,'fixture-events.jsonl');
    const c={n:5,output:runOutput,cwd,cores:{256:cores,512:cores}};
    for(const arm of ['official','ours']) {
        const library=path.join(base,arm+' identity fixtures');fs.mkdirSync(library);
        for(const f of ['libcangjie-runtime.so','libboundscheck.so'])fs.writeFileSync(path.join(library,f),'IDENTITY FIXTURE; NOT ELF; NOT LOADED\n');
        c[arm]={library};
        for(const family of ['NW','SD']) {
            const file=path.join(base,`${arm}-${family}-fixture.mjs`);
            const p={events,arm,family,mode,cwd,runOutput,library};
            const code=`#!${process.execPath}\nimport fs from 'node:fs';\nimport path from 'node:path';\nimport assert from 'node:assert/strict';\nconst p=${JSON.stringify(p)};\np.sampleFromReport=s=>{const m=s.match(/\\/r([1-5])\\/mrt_report\\.txt$/);assert(m);return Number(m[1]);};\n(${fixture.toString()})(p);\n`;
            fs.writeFileSync(file,code,{mode:0o755});c[arm][family]=file;
        }
    }
    const input=path.join(base,'config.json');save(input,c);return {base,c,input,events};
}
function invoke(s,expectedRc,error) {
    const argv=[path.join(here,'measure_r6.mjs'),'run',s.input],started=new Date().toISOString();
    const p=spawnSync(process.execPath,argv,{cwd:s.base,encoding:'utf8',timeout:90000,maxBuffer:4*1024**2,
        env:{...process.env,R6_ENTRY_POISON:'must-not-reach-fixture',CANGJIE_HOME:'/fixture-host-not-target',cjHeapSize:'bad',cjConcGCThreads:'99',MRT_GC_LOG:'bad',MRT_LOG_LEVEL:'bad'}});
    fs.writeFileSync(path.join(s.base,'driver.stdout'),p.stdout||'');fs.writeFileSync(path.join(s.base,'driver.stderr'),p.stderr||'');
    const record={argv:[process.execPath,...argv],cwd:s.base,started,ended:new Date().toISOString(),rc:p.status,signal:p.signal,error:p.error?.message??null,expectedRc};
    records.push(record);save(path.join(s.base,'driver.json'),record);
    assert.equal(p.error,undefined,p.stderr);assert.equal(p.signal,null);assert.equal(p.status,expectedRc,p.stderr);
    if(error)assert.match(p.stderr,error);
    const events=fs.existsSync(s.events)?fs.readFileSync(s.events,'utf8').trim().split('\n').map(l=>JSON.parse(l)):[];
    const starts=events.filter(e=>e.event==='start');
    const plan=JSON.parse(fs.readFileSync(path.join(s.c.output,'plan.json')));
    assert.equal(plan.length,40); // N/checks remain unchanged, even on failure.
    for(let i=0;i<starts.length;i++) {
        const row=plan[i],e=starts[i];
        assert.equal(e.arm,row.arm);assert.equal(e.family,row.cell.slice(0,2));assert.equal(e.env.cjHeapSize,row.cell.slice(2)+'MB');
        assert.equal(e.cpus,'144-159');assert.equal(e.cwd,s.c.cwd);assert.equal(e.argv[1],row.argv.at(-1));
        const r=JSON.parse(fs.readFileSync(path.join(row.dir,'command.json')));
        assert.deepEqual(r.argv,row.argv);assert.equal(r.cwd,s.c.cwd);assert.equal(r.command,'/usr/bin/time');
        assert.equal(r.signal,null);assert.equal(r.error,null);assert(Date.parse(r.ended)>=Date.parse(r.started));
        assert.equal(fs.readFileSync(path.join(row.dir,'rc'),'utf8'),r.rc+'\n');
        assert(fs.readFileSync(path.join(row.dir,'stdout.txt'),'utf8').includes('R6_ENTRY_FIXTURE'));
        assert(fs.existsSync(path.join(row.dir,'time.txt')));assert(fs.existsSync(path.join(row.dir,'load-after.txt')));
    }
    if(expectedRc===0) {
        assert.equal(starts.length,40);assert.equal(events.filter(e=>e.event==='normal-exit-request'&&e.code===0).length,40);
        const sum=JSON.parse(fs.readFileSync(path.join(s.c.output,'summary.json')));assert.equal(sum.numericCriteriaMet,true);assert.equal(sum.rows.length,40);
        for(const row of plan) assert.equal(JSON.parse(fs.readFileSync(path.join(row.dir,'command.json'))).rc,0);
    } else {
        assert(!fs.existsSync(path.join(s.c.output,'summary.json')));
        if(starts.length) {
            assert.equal(starts.length,2);assert(!fs.existsSync(plan[2].dir),'third sample started after first failure');
            assert.equal(JSON.parse(fs.readFileSync(path.join(plan[0].dir,'command.json'))).rc,0);
            const failed=JSON.parse(fs.readFileSync(path.join(plan[1].dir,'command.json')));
            if(error.source.includes('143')) {
                assert.equal(failed.rc,143);
                assert.equal(events.filter(e=>e.event==='self-signal-request'&&e.signal==='SIGTERM').length,1);
                assert.match(fs.readFileSync(path.join(plan[1].dir,'time.txt'),'utf8'),/Command terminated by signal 15/);
            } else assert.equal(failed.rc,7);
        } else {
            assert(!fs.existsSync(path.join(plan[0].dir,'command.json')));
            assert(!fs.existsSync(plan[1].dir));
        }
    }
    return {name:path.basename(s.base),driver:record,fixtureStarts:starts.length};
}

const results=[];
results.push(invoke(scenario('success','ok'),0));
results.push(invoke(scenario('failure-exit7','exit7'),1,/product stopped: rc=7 signal=null/));
results.push(invoke(scenario('failure-signal15','signal'),1,/product stopped: rc=143 signal=null/));
// A real out-of-scope range, rejected by the unmodified actual scope reader;
// it is never applied to a child. No fake /proc/cgroup, taskset or reduced N.
results.push(invoke(scenario('affinity-reject','ok','128-143'),1,/affinity: requested range outside scope/));
save(path.join(output,'result.json'),{rc:0,results,fixtureProcesses:44,realWorkloads:0,compilers:0,
    timeout:'NOT_EXERCISED; entire service bounded to120s',C3:'UNMET',qualification:'PUBLIC RUN ENTRY FIXTURES ONLY; NOT NW/SD OR PERFORMANCE PASS'});
console.log('PUBLIC_RUN_ENTRY_FIXTURES_PASS');
