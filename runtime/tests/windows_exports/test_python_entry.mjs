#!/usr/bin/env zx
// Finite public build.py CLI contract. Child launches and host observations are
// recorded, not executed: this test does not qualify a Windows compiler or DLL.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {parseArgs} from 'node:util';

const {values} = parseArgs({options: {
  source: {type: 'string'}, work: {type: 'string'}, case: {type: 'string'},
}});
assert.ok(values.source && values.work && values.case, '--source --work --case are required');
assert.ok(['windows-path', 'windows-python', 'native-path', 'native-python', 'invalid'].includes(values.case));
const work = path.resolve(values.work), trace = path.join(work, 'children.json');
assert.ok(!fs.existsSync(work), 'use a new evidence directory');
fs.mkdirSync(work, {recursive: true});
const entry = path.join(work, 'build.py');
fs.copyFileSync(path.join(path.resolve(values.source), 'runtime/build.py'), entry);
const windows = values.case.startsWith('windows');
const launcher = String.raw`
import json, os, platform, runpy, subprocess, sys
entry, trace, mode = sys.argv[1:4]
windows = mode.startswith('windows')
initial = 'C:/Existing Tools/bin;C:/Windows/System32' if windows else '/existing/tools:/usr/bin'
os.environ['PATH'] = initial
if windows:
    os.pathsep = ';'
    platform.system = lambda: 'Windows'
    platform.machine = lambda: 'AMD64'
events = []
def observe(argv, **kwargs):
    events.append({'argv': argv, 'path': kwargs.get('env', os.environ)['PATH']})
    return subprocess.CompletedProcess(argv, 0)
subprocess.run = observe
sys.argv = [entry, 'build', '--target', 'windows-x86_64' if windows else 'native', '--jobs', '4']
if windows:
    sys.argv += ['--target-toolchain', 'C:/Tool Chain']
if mode == 'invalid':
    sys.argv += ['--unknown-option']
try:
    runpy.run_path(entry, run_name='__main__')
finally:
    with open(trace, 'w') as stream:
        json.dump({'events': events, 'initial': initial, 'python': sys.executable.replace(chr(92), '/')}, stream, indent=2)
`;
const args = ['-c', launcher, entry, trace, values.case];
const result = spawnSync('python3', args, {
  cwd: work, encoding: 'utf8', env: {...process.env, PYTHONDONTWRITEBYTECODE: '1'},
});
fs.writeFileSync(path.join(work, 'command.json'), JSON.stringify({
  command: ['python3', ...args], rc: result.status, signal: result.signal, error: result.error?.message ?? null,
}, null, 2));
fs.writeFileSync(path.join(work, 'stdout.log'), result.stdout ?? '');
fs.writeFileSync(path.join(work, 'stderr.log'), result.stderr ?? '');
assert.equal(result.error, undefined);
assert.equal(result.signal, null);
const observed = JSON.parse(fs.readFileSync(trace, 'utf8'));
if (values.case === 'invalid') {
  assert.equal(result.status, 2);
  assert.equal(observed.events.length, 0);
  console.log('INVALID_CLI_REJECTED before child launches');
} else {
  assert.equal(result.status, 0, 'public CLI completes within the finite child-launch apparatus');
  assert.equal(observed.events.length, 3, 'configure, build and preinstall reached');
  const configure = observed.events[0];
  assert.equal(configure.argv[0], 'cmake');
  assert.ok(observed.events[1].argv.includes('cangjie-runtime'));
  assert.ok(observed.events[2].argv.includes('preinstall'));
  console.log('PYTHON_ENTRY_TARGET_REACHED case=' + values.case);
  if (values.case.endsWith('path')) {
    const expected = windows ? 'C:/Tool Chain/bin;' + observed.initial : observed.initial;
    for (const event of observed.events) assert.equal(event.path, expected, 'HOST_PATH_SEPARATOR');
  } else {
    const pins = configure.argv.filter(arg => arg.startsWith('-DPython3_EXECUTABLE'));
    assert.deepEqual(pins, ['-DPython3_EXECUTABLE:FILEPATH=' + observed.python], 'ENTRY_INTERPRETER_FORWARDED');
  }
  console.log('PYTHON_ENTRY_PASS case=' + values.case);
}
