#!/usr/bin/env zx
// #820 r6 recipe, r5 four-timer proxy. TASK0200: preparation only;
// `run` requires a separately admitted real measurement, never run by the replay tests.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';

const here = path.dirname(fileURLToPath(import.meta.url));
export const CELLS = ['NW256', 'NW512', 'SD256', 'SD512'];
const ARMS = ['official', 'ours'];
// Independent historical oracle in tools/gate_nwdet.py (not generated from the subject).
const GOLD = ['2289681096600', '13798287193200', '34525818289800', '64472274386400',
    '103637655483000', '152021961579600', '209625192676200', '276447348772800',
    '352488429869400', '437748435966000', '532227367062600', '635925223159200'];
function need(ok, why) { if (!ok) throw Error(why); }
function text(file) { return fs.readFileSync(file, 'utf8'); }
function save(file, value) { fs.writeFileSync(file, JSON.stringify(value, null, 2) + '\n', {flag: 'wx'}); }
function integer(s, why) {
    need(/^[0-9]+$/.test(String(s)), why);
    const n = Number(s); need(Number.isSafeInteger(n), why); return n;
}
function completeText(s, what) { need(s.length > 0 && s.endsWith('\n'), `${what}: empty/truncated final line`); }

export function checksum(stdout, cell) {
    completeText(stdout, 'stdout');
    const nw = cell.startsWith('NW'), marker = nw ? 'NATURAL_WAVE_OK' : 'SURVIVAL_DENSE_OK';
    const markers = stdout.split('\n').filter(l => l.startsWith(marker));
    need(markers.length === 1, 'checksum: missing/duplicate completion marker');
    const m = markers[0].match(new RegExp(`^${marker} checksum=([0-9]+)(?: |$)`));
    need(m && m[1] === (nw ? GOLD[11] : '368685940367600'), 'checksum: final mismatch');
    if (nw) {
        const waves = stdout.split('\n').filter(l => l.startsWith('WAVE_DONE'));
        need(waves.length === 12, 'checksum: require exactly 12 WAVE_DONE');
        for (let i = 0; i < 12; ++i) {
            const w = waves[i].match(/^WAVE_DONE wave=([0-9]+) checksum=([0-9]+)$/);
            need(w && Number(w[1]) === i && w[2] === GOLD[i], `checksum: first_div=${i}`);
        }
        need(stdout.indexOf(markers[0]) > stdout.lastIndexOf('WAVE_DONE'), 'checksum: marker before final wave');
    }
    return m[1];
}

// Exact existing official report grammar; do not substitute "stw time" or wall.
// Per-thread Begin/total framing rejects omitted middle/end timers, not just EOF damage.
export function officialTimers(report) {
    completeText(report, 'official report');
    const names = ['enum roots & update old pointers within', 'MarkSatbBuffer', 'Preforward', 'ForwardFromRegions'];
    const active = new Map(), counts = Object.fromEntries(names.map(n => [n, 0]));
    let ns = 0, cycles = 0, totalGcNs = 0;
    const us = value => integer(value.replaceAll(',', ''), 'official timer: invalid integer') * 1000;
    for (const line of report.split('\n')) {
        const relevant = line.includes('Begin GC log.') || line.includes('total gc time:') || names.some(n => line.includes(`${n} time:`));
        if (!relevant) continue;
        const row = line.match(/^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+ ([0-9]+) (.*)$/);
        need(row, 'official timer: malformed timestamp/tid');
        const [, tid, body] = row;
        if (body.startsWith('Begin GC log. GCReason: ')) {
            need(!active.has(tid), 'official timer: nested/missing cycle end');
            active.set(tid, new Set()); continue;
        }
        need(active.has(tid), 'official timer: missing cycle begin');
        const seen = active.get(tid);
        if (body.startsWith('total gc time:')) {
            need(seen.size === 4, 'official timer: incomplete four-timer cycle');
            const m = body.match(/^total gc time: ([0-9]+|[0-9]{1,3}(?:,[0-9]{3})+) us, collection rate /);
            need(m, 'official timer: malformed cycle total');
            totalGcNs += us(m[1]); cycles++; active.delete(tid); continue;
        }
        const name = names.find(n => body.startsWith(`${n} time:`));
        need(name && !seen.has(name), 'official timer: unexpected/duplicate timer');
        const value = body.slice(`${name} time:`.length).trim();
        const m = value.match(/^([0-9]+|[0-9]{1,3}(?:,[0-9]{3})+)\s*us$/);
        need(m, 'official timer: malformed microseconds');
        ns += us(m[1]); counts[name]++; seen.add(name);
    }
    need(cycles > 0 && active.size === 0, 'official timer: missing/truncated cycle');
    need(Number.isSafeInteger(ns) && Number.isSafeInteger(totalGcNs), 'official timer: sum overflow');
    return {stw_ns: ns, cycles, counts, total_gc_ns: totalGcNs, ruler: 'r5-four-timer-proxy-not-total-STW'};
}

function subjectTimers(file) {
    completeText(text(file), 'subject log');
    // Reuse the shipped current lifecycle parser rather than invent another GC schema.
    const code = `import json,sys
sys.path.insert(0,sys.argv[1])
from gclog_schema import parse_gclog
r=parse_gclog(open(sys.argv[2]).read()).validate_complete()
if not r.cycles or not r.phases: raise ValueError('subject: no complete ledger')
if r.aborted or r.truncated: raise ValueError('subject: aborted/truncated ledger')
p=[x for x in r.phases if x.kind=='pause']
if not p: raise ValueError('subject: no pause samples')
print(json.dumps(dict(stw_ns=sum(x.ns for x in p), npause=len(p), cycles=sum(x.event=='end' for x in r.cycles), ruler='r5-phase-pause-ns')))
`;
    const p = spawnSync('python3', ['-B', '-c', code, here, file], {encoding: 'utf8', timeout: 30000, maxBuffer: 1024 * 1024});
    need(p.status === 0 && !p.signal && !p.error, `subject parser: ${p.stderr || p.error || p.signal}`);
    const result = JSON.parse(p.stdout);
    need(Number.isSafeInteger(result.stw_ns), 'subject pause sum overflow'); return result;
}

export function readSample(dir, arm, cell) {
    need(ARMS.includes(arm) && CELLS.includes(cell), 'unknown arm/cell');
    const rc = integer(text(path.join(dir, 'rc')).trim(), 'missing/invalid process rc');
    need(rc === 0, `product rc=${rc}`);
    const cmdPath = path.join(dir, 'command.json');
    if (fs.existsSync(cmdPath)) {
        const c = JSON.parse(text(cmdPath));
        need(c.rc === 0 && c.signal === null && c.error === null, 'process signal/error');
    }
    const timing = text(path.join(dir, 'time.txt')).replaceAll('\\t', '\t');
    const m = timing.match(/^wall_s=([0-9]+(?:\.[0-9]+)?)\tmaxrss_kb=([0-9]+)\ttime_exit=([0-9]+)\n$/);
    need(m && m[3] === '0', 'missing/invalid time fields or time_exit');
    const wall = Number(m[1]), maxrss = integer(m[2], 'invalid RSS');
    need(wall > 0 && Number.isFinite(wall) && maxrss > 0, 'invalid wall/RSS');
    const loads = ['before','after'].map(when => {
        const current = path.join(dir, `load-${when}.txt`);
        const s = fs.existsSync(current) ? text(current) : text(path.join(dir, `uptime-${when}.txt`));
        const m = fs.existsSync(current) ? s.match(/^([0-9.]+) /) : s.match(/load average[s]?:\s*([0-9.]+)/);
        need(m && Number.isFinite(Number(m[1])), 'noise: missing/invalid load1');
        need(Number(m[1]) <= 50, 'noise: load1>50'); return Number(m[1]);
    });
    const sum = checksum(text(path.join(dir, 'stdout.txt')), cell);
    const timers = arm === 'official' ? officialTimers(text(path.join(dir, 'mrt_report.txt'))) : subjectTimers(path.join(dir, 'stderr.txt'));
    return {arm, cell, rc, checksum: sum, wall, maxrss_kb: maxrss, load1: loads, ...timers};
}

export function compare(official, ours) {
    need(official.length === ours.length && official.length >= 5, 'comparison: N>=5 paired samples required');
    for (const n of [...official, ...ours]) need(Number.isFinite(n) && n >= 0, 'comparison: invalid value');
    const stats = a => { const s = [...a].sort((x,y) => x-y), n = s.length; return {median: (s[Math.floor((n-1)/2)] + s[Math.floor(n/2)]) / 2, min: s[0], max: s[n-1]}; };
    const off = stats(official), sub = stats(ours), noise = Math.max(off.max-off.min, sub.max-sub.min), advantage = off.median-sub.median;
    // Boundary ties remain ties despite IEEE subtraction rounding of decimal wall seconds.
    const epsilon = Number.EPSILON * Math.max(1, off.max, sub.max) * 4;
    return {official: off, ours: sub, noise, advantage, verdict: advantage > noise+epsilon ? 'better' : advantage < -noise-epsilon ? 'worse' : 'noise-tie'};
}

export function summarize(root, n = 5) {
    need(Number.isInteger(n) && n >= 5, 'N must be >=5');
    const cells = {}, rows = [];
    for (const cell of CELLS) {
        const arms = {};
        for (const arm of ARMS) {
            const base = path.join(root, `${arm}_${cell}`);
            const dirs = fs.readdirSync(base).filter(p => /^r[0-9]+$/.test(p));
            need(dirs.length === n, `${arm}/${cell}: exact N mismatch`);
            arms[arm] = [];
            for (let i=1; i<=n; i++) { const row = {...readSample(path.join(base, `r${i}`), arm, cell), i}; arms[arm].push(row); rows.push(row); }
        }
        const wall = compare(arms.official.map(r=>r.wall), arms.ours.map(r=>r.wall));
        const stw = compare(arms.official.map(r=>r.stw_ns), arms.ours.map(r=>r.stw_ns));
        cells[cell] = {wall, stw, pass: stw.verdict === 'better' && (cell.startsWith('NW') ? wall.verdict === 'better' : wall.verdict !== 'worse')};
    }
    return {n, cells, rows, numericCriteriaMet: Object.values(cells).every(c=>c.pass), qualification: 'requires fixed producer/loader, load/affinity and independent review; numeric result alone is not release qualification'};
}

function config(file) {
    const c = JSON.parse(text(file));
    need(Number.isInteger(c.n) && c.n >= 5, 'config: N>=5');
    for (const p of [c.output, c.cwd, ...ARMS.flatMap(a => [c[a]?.library, c[a]?.NW, c[a]?.SD])]) need(typeof p === 'string' && path.isAbsolute(p), 'config: absolute input/output/cwd required');
    for (const heap of ['256','512']) {
        const m = c.cores?.[heap]?.match(/^([0-9]+)-([0-9]+)$/);
        need(m && Number(m[2])-Number(m[1]) === 15 && Number(m[2]) < 190, 'config: explicit 16-core range per heap required');
    }
    return c;
}
export function plan(c) {
    const rows = [];
    for (const cell of CELLS) for (let i=1; i<=c.n; ++i) for (const arm of ARMS) {
        const heap = cell.slice(2), d = path.join(c.output, 'measure', `${arm}_${cell}`, `r${i}`);
        const env = ['PATH=/usr/bin:/bin', 'HOME=/root', `cjHeapSize=${heap}MB`, `LD_LIBRARY_PATH=${c[arm].library}`,
            ...(arm === 'ours' ? ['cjConcGCThreads=4', 'MRT_GC_LOG=1'] : [`MRT_REPORT=${d}/mrt_report.txt`, 'MRT_LOG_LEVEL=v'])];
        rows.push({arm, cell, i, dir: d, cwd: c.cwd, cores: c.cores[heap], command: '/usr/bin/time',
            argv: ['-f', 'wall_s=%e\tmaxrss_kb=%M\ttime_exit=%x', '-o', `${d}/time.txt`,
                'timeout', '-k', '10', '300', 'taskset', '-c', c.cores[heap], 'env', '-i', ...env, c[arm][cell.slice(0,2)]]});
    }
    return rows;
}

function run(c) {
    // No default inputs, no resume/overwrite, no compilation, no retries.
    const rows = plan(c);
    need(!fs.existsSync(c.output), 'output exists; refusing overwrite/resume');
    const limits = text('/proc/self/limits');
    need(/Max core file size\s+0\s+0/.test(limits), 'core soft/hard must be 0');
    const cg = '/sys/fs/cgroup' + text('/proc/self/cgroup').trim().split('::')[1];
    need(text(`${cg}/memory.swap.max`).trim() === '0', 'swap must be 0');
    fs.mkdirSync(c.output, {recursive: true}); save(`${c.output}/config.json`, c); save(`${c.output}/plan.json`, rows);
    const inputPaths = [...new Set(ARMS.flatMap(a=>[c[a].NW,c[a].SD,`${c[a].library}/libcangjie-runtime.so`,`${c[a].library}/libboundscheck.so`]))];
    const identities = inputPaths.map(p=>({path:p, realpath:fs.realpathSync(p), sha256:createHash('sha256').update(fs.readFileSync(p)).digest('hex')}));
    save(`${c.output}/inputs.json`, identities);
    for (const row of rows) {
        fs.mkdirSync(row.dir, {recursive:true}); fs.writeFileSync(`${row.dir}/mrt_report.txt`, '', {flag:'wx'});
        const before = text('/proc/loadavg'); fs.writeFileSync(`${row.dir}/load-before.txt`, before);
        need(Number(before.split(' ')[0]) <= 50, 'noise: load1>50, stop before next sample');
        const affinity = spawnSync('taskset', ['-pc', String(process.pid)], {encoding:'utf8'});
        need(affinity.status === 0, 'cannot read scope affinity');
        fs.writeFileSync(`${row.dir}/affinity.txt`, affinity.stdout);
        const allowed = text(`${cg}/cpuset.cpus.effective`).trim().split(',').flatMap(s=>{ const [a,b=a]=s.split('-').map(Number); return Array.from({length:b-a+1},(_,i)=>a+i); });
        const [a,b] = row.cores.split('-').map(Number);
        need(Array.from({length:b-a+1},(_,i)=>a+i).every(x=>allowed.includes(x)), 'affinity: requested range outside scope');
        const out = fs.openSync(`${row.dir}/stdout.txt`, 'wx'), err = fs.openSync(`${row.dir}/stderr.txt`, 'wx');
        const started = new Date().toISOString();
        const p = spawnSync(row.command, row.argv, {cwd:row.cwd, env:{PATH:'/usr/bin:/bin',LC_ALL:'C'}, stdio:['ignore',out,err]});
        fs.closeSync(out); fs.closeSync(err);
        const record = {...row, started, ended:new Date().toISOString(), rc:p.status, signal:p.signal, error:p.error?.message??null};
        save(`${row.dir}/command.json`, record); fs.writeFileSync(`${row.dir}/rc`, `${p.status}\n`);
        const after = text('/proc/loadavg'); fs.writeFileSync(`${row.dir}/load-after.txt`, after);
        need(p.status===0 && !p.signal && !p.error, `product stopped: rc=${p.status} signal=${p.signal}`);
        const parsed = readSample(row.dir, row.arm, row.cell); save(`${row.dir}/parsed.json`, parsed);
        need(Number(after.split(' ')[0]) <= 50, 'noise: load1>50, preserve sample and stop');
    }
    const summary = summarize(`${c.output}/measure`, c.n); save(`${c.output}/summary.json`, summary); return summary;
}

export function main(args) {
    const [mode, ...rest] = args;
    if (mode === 'sample' && rest.length === 3) return readSample(path.resolve(rest[2]), rest[0], rest[1]);
    if (mode === 'summarize' && rest.length === 2) return summarize(path.resolve(rest[0]), Number(rest[1]));
    if ((mode === 'plan' || mode === 'run') && rest.length === 1) { const c=config(rest[0]); return mode==='plan' ? plan(c) : run(c); }
    throw Error('usage: measure_r6.mjs plan|run CONFIG.json; sample official|ours CELL DIR; summarize MEASURE_ROOT N');
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    try { const result=main(process.argv.slice(2)); console.log(JSON.stringify(result,null,2)); if(result.numericCriteriaMet===false)process.exitCode=1; }
    catch(e) { console.error(String(e)); process.exitCode=1; }
}
