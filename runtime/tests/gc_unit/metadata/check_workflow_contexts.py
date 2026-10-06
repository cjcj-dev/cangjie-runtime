#!/usr/bin/env python3
"""Bounded full-workflow actionlint regression for the two job-env failures.

Run on kkk2 with actionlint v1.7.7, the saved c43 workflow, and this workflow.
Optional shellcheck/pyflakes are excluded; all actionlint diagnostics are retained.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--parser', required=True, type=Path)
p.add_argument('--old', required=True, type=Path)
p.add_argument('--candidate', required=True, type=Path)
p.add_argument('--out', required=True, type=Path)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=True)
version = subprocess.run([str(a.parser), '-version'], capture_output=True, text=True)
(a.out / 'parser-version.txt').write_text(version.stdout + version.stderr)
assert version.returncode == 0 and version.stdout.splitlines()[0] == '1.7.7', version.stdout
old = a.old.read_text()
candidate = a.candidate.read_text()
cut = candidate
for variable, suffix in [('METADATA_TUPLE_ARTIFACT', 'metadata-tuple'),
                         ('TUPLE_ROOT', 'metadata-tool-sources')]:
    step = f'        env:\n          {variable}: ${{{{ runner.temp }}}}/{suffix}\n'
    assert cut.count(step) == 2, variable
    cut = cut.replace(step, '')
    job = 'fixtures' if variable == 'METADATA_TUPLE_ARTIFACT' else 'tools'
    start = cut.index(f'  {job}:\n')
    pos = cut.index('    steps:\n', start)
    cut = cut[:pos] + f'      {variable}: ${{{{ runner.temp }}}}/{suffix}\n' + cut[pos:]
(a.out / 'cut.yml').write_text(cut)
results = {}
for arm, source in [('old', old), ('candidate', candidate), ('cut', cut), ('restored', candidate)]:
    workflow = a.out / (arm + '.yml')
    workflow.write_text(source)
    command = [str(a.parser), '-shellcheck=', '-pyflakes=', '-format', '{{json .}}', str(workflow)]
    (a.out / (arm + '.command.json')).write_text(json.dumps(command))
    run = subprocess.run(command, capture_output=True, text=True)
    (a.out / (arm + '.stdout')).write_text(run.stdout)
    (a.out / (arm + '.stderr')).write_text(run.stderr)
    (a.out / (arm + '.rc')).write_text(str(run.returncode) + '\n')
    assert run.returncode in (0, 1), (arm, run.returncode, run.stderr)
    errors = json.loads(run.stdout) or []
    results[arm] = Counter((e['kind'], e['message']) for e in errors)
    print(arm, 'rc=', run.returncode, 'diagnostics=', len(errors), flush=True)

def target(key):
    return key[0] == 'expression' and 'runner' in key[1] and 'not available' in key[1]

for arm, expected in [('old', 2), ('candidate', 0), ('cut', 2), ('restored', 0)]:
    actual = sum(n for key, n in results[arm].items() if target(key))
    assert actual == expected, (arm, actual, results[arm])
controls = lambda arm: Counter({k: v for k, v in results[arm].items() if not target(k)})
assert controls('old') == controls('candidate') == controls('cut') == controls('restored')
assert results['candidate'] == results['restored']
(a.out / 'summary.json').write_text(json.dumps({arm: [{'kind': k[0], 'message': k[1], 'count': v}
    for k, v in counts.items()] for arm, counts in results.items()}, indent=2) + '\n')
print('WORKFLOW_CONTEXT_REGRESSION_PASS: runner job-env errors 2/0/2/0; controls unchanged')
