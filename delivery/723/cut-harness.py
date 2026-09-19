from pathlib import Path
import difflib
import os
import shutil
import subprocess

root = Path('/root/sym_cangjie_runtime_723_implement_r5741705087')
source = root / 'harness'
evidence = root / 'evidence'
cuts = {
    'compiler': ('kkk2_managed.sh', '                build_failed = True',
                 '                build_failed = False'),
    'missing': ('kkk2_managed.sh', '                build_failed = bool(missing)',
                 '                build_failed = False'),
    'consumer': ('kkk2_diff.sh', "if any(res['arms'][a]['build_fail'].values()) or ca['rc'] in ('','NA') or ba['rc'] in ('','NA'):",
                 "if ca['rc'] in ('','NA') or ba['rc'] in ('','NA'):")}
processes = []
for arm in ('compiler', 'missing', 'consumer', 'restored'):
    tree = root / f'harness-{arm}'
    shutil.copytree(source, tree, dirs_exist_ok=True)
    if arm in cuts:
        filename, before, after = cuts[arm]
        path = tree / 'runtime/tests/gc_unit' / filename
        original = path.read_text()
        assert original.count(before) == 1
        modified = original.replace(before, after)
        path.write_text(modified)
        diff = difflib.unified_diff(original.splitlines(True), modified.splitlines(True),
                                   fromfile=f'a/runtime/tests/gc_unit/{filename}',
                                   tofile=f'b/runtime/tests/gc_unit/{filename}')
        (evidence / f'cut-{arm}.diff').write_text(''.join(diff))
    log = (evidence / f'harness-{arm}.log').open('w')
    env = dict(os.environ, TMPDIR=str(evidence), PYTHONDONTWRITEBYTECODE='1')
    process = subprocess.Popen(['python3', str(tree / 'runtime/tests/gc_unit/test_managed_build_status.py')],
                               env=env, stdout=log, stderr=subprocess.STDOUT)
    processes.append((arm, process, log))
for arm, process, log in processes:
    rc = process.wait()
    log.close()
    (evidence / f'harness-{arm}.rc').write_text(f'{rc}\n')
    print(f'{arm} rc={rc}')
    assert (rc == 0) == (arm == 'restored')
