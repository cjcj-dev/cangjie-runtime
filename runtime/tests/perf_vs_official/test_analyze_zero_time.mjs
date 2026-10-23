#!/usr/bin/env zx
// Exercise the public analyzer CLI using run_one.sh's on-disk attempt format.
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const { $ } = await import(process.env.ZX_MODULE || 'zx');
$.verbose = false;
const analyzer = path.resolve(process.argv[2] || fileURLToPath(new URL('./analyze.py', import.meta.url)));
const evidence = path.resolve(process.argv[3]);
await fs.mkdir(evidence, { recursive: true });

async function attempt(root, round, arm, wall, rc = 0) {
  const dir = path.join(root, 'runs', 'fixture', '64m', `r${round}-${arm}`);
  await fs.mkdir(dir, { recursive: true });
  const files = {
    'meta.txt': `arm=${arm}\nheap=64m\nenv.cjHeapSize=64m\nobservation=fair\nenv.MRT_LOG_LEVEL=UNSET\nenv.MRT_LOG_PATH=UNSET\nenv.MRT_REPORT=UNSET\nenv.MRT_GC_LOG=UNSET\n`,
    rc: `${rc}\n`,
    'time.tsv': `wall_s=${wall.toFixed(2)}\tmaxrss_kb=1024\ttime_exit=${rc}\n`,
    stdout: rc === 0 ? 'CHECKSUM=42\n' : '',
    classification: rc === 0 ? 'COMPLETE\n' : 'FAILED\n',
  };
  for (const [name, data] of Object.entries(files)) await fs.writeFile(path.join(dir, name), data);
}
async function tsv(file) {
  const [header, ...rows] = (await fs.readFile(file, 'utf8')).trimEnd().split('\n');
  const keys = header.split('\t');
  return rows.map(row => Object.fromEntries(row.split('\t').map((value, i) => [keys[i], value])));
}
const cases = [
  { name: 'normal', subject: [1, 1, 1], official: [2, 2, 2] },
  { name: 'failed-zero', subject: [0], official: [2], rc: 1, unavailable: true },
  { name: 'success-zero-subject', subject: [0], official: [2], unavailable: true },
  { name: 'success-zero-official', subject: [1], official: [0], unavailable: true },
  { name: 'success-zero-both', subject: [0], official: [0], unavailable: true },
  { name: 'mixed-zero-middle', subject: [1, 0, 1], official: [2, 2, 2], unavailable: true },
  { name: 'mixed-zero-last', subject: [1, 1, 0], official: [2, 2, 2], unavailable: true },
  { name: 'zero-work-units', subject: [1], official: [2], units: 0 },
];
let failures = 0;
for (const test of cases) {
  try {
    const root = path.join(evidence, test.name);
    await fs.mkdir(root); // Never overwrite an earlier run's evidence.
    await fs.writeFile(path.join(root, 'manifest.tsv'), `workload\tmarker\twork_units\nfixture\tCHECKSUM=42\t${test.units ?? 100}\n`);
    for (let i = 0; i < test.subject.length; ++i) {
      await attempt(root, i + 1, 'subject', test.subject[i], test.rc ?? 0);
      await attempt(root, i + 1, 'official', test.official[i]);
    }
    const output = path.join(root, 'analysis');
    const args = [analyzer, root, output, '--min-pairs', '1', '--bootstrap-samples', '100', '--seed', '42'];
    await fs.writeFile(path.join(root, 'command.json'), JSON.stringify(['python3', ...args]) + '\n');
    const result = await $({ nothrow: true })`python3 ${args}`;
    await fs.writeFile(path.join(root, 'stdout.log'), result.stdout);
    await fs.writeFile(path.join(root, 'stderr.log'), result.stderr);
    await fs.writeFile(path.join(root, 'rc'), String(result.exitCode) + '\n');
    assert.equal(result.exitCode, 0, `${test.name}: analyzer CLI must complete: ${result.stderr}`);
    const attempts = await tsv(path.join(output, 'attempts.tsv'));
    const metrics = await tsv(path.join(output, 'metrics.tsv'));
    const matrix = await tsv(path.join(output, 'matrix.tsv'));
    const summary = JSON.parse(await fs.readFile(path.join(output, 'summary.json'), 'utf8'));
    assert.equal(attempts.length, test.subject.length * 2, 'all attempts retained');
    assert.equal(summary.attempts, attempts.length);
    for (const row of attempts.filter(row => Number(row.wall_s) === 0)) {
      assert.equal(row.throughput, 'NA', 'zero-time attempt throughput must be unavailable');
      assert.equal(row.correct, test.rc && row.arm === 'subject' ? 'False' : 'True');
      assert.equal(row.rc, test.rc && row.arm === 'subject' ? '1' : '0');
    }
    const throughput = matrix.find(row => row.metric === 'throughput');
    const wall = matrix.find(row => row.metric === 'wall');
    if (test.unavailable) {
      for (const row of [throughput, wall]) {
        for (const field of ['ratio_subject_official', 'ci95_low', 'ci95_high'])
          assert.equal(row[field], 'NA', `${row.metric} ${field}: unavailable timing must not yield a performance claim`);
        assert.equal(row.status, test.rc ? 'INVALID' : 'INSUFFICIENT');
      }
      for (const row of metrics) {
        if (test[row.arm].includes(0)) assert.equal(row.throughput_median, 'NA');
      }
    } else if (test.units === 0) {
      assert.equal(throughput.subject, '0.000000');
      assert.equal(throughput.official, '0.000000');
      assert.equal(throughput.ratio_subject_official, 'NA');
      assert.equal(throughput.status, 'INSUFFICIENT');
    } else {
      assert.equal(throughput.subject, '100.000000');
      assert.equal(throughput.official, '50.000000');
      for (const key of ['ratio_subject_official', 'ci95_low', 'ci95_high']) assert.equal(throughput[key], '2.000000');
      assert.equal(throughput.status, 'SUPERIOR');
      assert.equal(wall.ratio_subject_official, '0.500000');
      assert.equal(wall.status, 'SUPERIOR');
    }
    assert.equal(matrix.find(row => row.metric === 'peak_memory').ratio_subject_official, test.rc ? 'NA' : '1.000000');
    console.log(`PASS ${test.name}`);
  } catch (error) {
    ++failures;
    console.error(`FAIL ${test.name}: ${error.stack}`);
  }
}
console.log(`RESULT cases=${cases.length} failures=${failures}`);
process.exitCode = failures ? 1 : 0;
