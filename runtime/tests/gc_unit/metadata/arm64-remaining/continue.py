#!/usr/bin/env python3
"""#1478 only: consume run 37080734066's two immutable ARM64 artifacts."""
import ast
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import re
import shutil
import subprocess
import sys
import time
from types import SimpleNamespace
import urllib.request
import urllib.error

HERE = Path(__file__).resolve().parent
SOURCE_HEAD = 'e8d8e9d4fea011b8b943ca4dc696ffc1f974f82f'
SOURCE_RUN = 37080734066
CONFIG = os.environ['SOURCE_CONFIG']
FIXED = {
    'default': (11259325217, 'ea65abe394b3433032351769f06d430d4c1c43e322135847ddf4219f75acb0c8',
                '87f628ea64006eeb6c66ba1d71ddb5d2349ed6475fbe3c7368b49944def7b344'),
    'testable': (11259310233, '7533798d5d5850c29c7e2edf76e47fa0a187ec4bae4bf44549e14022fd3ade58',
                 '1b9f3202991292675b191aec00c07e32399b6f5efd60a30e82b7c7ac8c6061ab'),
}
ARTIFACT, MANIFEST, RECEIPT = FIXED[CONFIG]
INPUT = Path(os.environ['INPUT']).resolve()
out = Path(os.environ['REMAIN']).resolve()
out.mkdir(parents=True, exist_ok=False)
bundle = out / 'bundle'
bundle.mkdir()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
record = {'source_run': SOURCE_RUN, 'source_head': SOURCE_HEAD, 'source_attempt': 1,
          'source_artifact_id': ARTIFACT, 'source_config': CONFIG,
          'execution_head': os.environ['GITHUB_SHA'], 'execution_run': os.environ['GITHUB_RUN_ID'],
          'execution_attempt': os.environ['GITHUB_RUN_ATTEMPT'],
          'metadata-regression': {'status': 'NOT_RUN'}, 'a2-run': {'status': 'NOT_RUN'},
          'behavior': 'NOT_RUN'}
args = SimpleNamespace(arm='candidate')
windows = mac = False
# The original shape consumer uses these names. It is executed verbatim below.
env = dict(os.environ, LD_LIBRARY_PATH=str(bundle),
           PATH=str(bundle) + os.pathsep + os.environ['PATH'],
           LD_DEBUG='libs', LD_DEBUG_OUTPUT=str(out / 'actual-loader'))


def save():
    (out / 'execution-result.json').write_text(json.dumps(record, indent=2))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def admission():
    assert os.environ['GITHUB_EVENT_NAME'] == 'workflow_dispatch'
    assert os.environ['GITHUB_REPOSITORY'] == 'cjcj-dev/cangjie-runtime'
    assert os.environ['GITHUB_REPOSITORY_VISIBILITY'] == 'public'
    assert os.environ['GITHUB_RUN_ATTEMPT'] == '1', 'no reruns authorized'
    assert record['execution_head'] == os.environ['APPROVED_SHA']
    deadline = dt.datetime.fromisoformat(os.environ['ABSOLUTE_DEADLINE'].replace('Z', '+00:00'))
    now = dt.datetime.now(dt.timezone.utc)
    assert deadline.tzinfo is not None and 0 < (deadline - now).total_seconds() <= 2400
    return (deadline - now).total_seconds()


def api(path):
    request = urllib.request.Request('https://api.github.com/repos/cjcj-dev/cangjie-runtime/' + path,
        headers={'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
                 'Accept': 'application/vnd.github+json'})
    # API/permission errors propagate unchanged. No retry or alternate identity.
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        record['api_error'] = {'path': path, 'status': error.code,
                               'headers': str(error.headers),
                               'body': error.read().decode('utf-8', errors='replace')}
        save()
        raise


def run(command, label, cwd=None, timeout=None):
    remaining = admission()
    argv = [str(x) for x in command]
    entry = record[label] = {'status': 'STARTED', 'argv': argv, 'cwd': str(cwd or Path.cwd()),
                             'started_utc': dt.datetime.now(dt.timezone.utc).isoformat()}
    save()
    start = time.monotonic()
    with (out / (label + '.log')).open('w') as log:
        log.write(repr(argv) + '\n'); log.flush()
        try:
            process = subprocess.Popen(argv, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            # Raw maps are observations of this execution, never inferred from ldd.
            snapshots = {}
            while process.poll() is None:
                pids = [process.pid]
                for pid in pids:
                    try:
                        children = Path(f'/proc/{pid}/task/{pid}/children').read_text().split()
                        pids.extend(int(child) for child in children if int(child) not in pids)
                        maps = Path(f'/proc/{pid}/maps').read_text()
                        if 'libcangjie-runtime.so' in maps and pid not in snapshots:
                            snapshots[pid] = maps
                            (out / f'{label}-{pid}.maps').write_text(maps)
                    except (FileNotFoundError, ProcessLookupError):
                        pass
                if time.monotonic() - start >= min(timeout or 120, remaining):
                    import signal
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL); process.wait()
                    entry['failure'] = 'TIMEOUT'
                    rc = 124
                    break
                time.sleep(0.001)
            else:
                rc = process.returncode
            entry['raw_maps'] = [f'{label}-{pid}.maps' for pid in snapshots]
            entry['raw_maps_status'] = 'CAPTURED' if snapshots else 'NOT_CAPTURED'
            loaded = {}
            for maps in snapshots.values():
                for line in maps.splitlines():
                    fields = line.split(None, 5)
                    if len(fields) == 6 and fields[5].startswith('/'):
                        path = Path(fields[5])
                        if path.is_file():
                            loaded[str(path)] = digest(path)
            entry['mapped_file_sha256'] = loaded
        except OSError as error:
            rc = 127
            entry['failure'] = str(error)
            log.write(str(error) + '\n')
    entry.update(status='FINISHED', rc=rc, wall=time.monotonic() - start)
    save()
    print(label, json.dumps(entry), flush=True)
    return rc


def checked(command, label):
    rc = run(command, label)
    if rc:
        raise SystemExit(rc)


save()
try:
    record['before'] = {'utc': dt.datetime.now(dt.timezone.utc).isoformat(),
                        'uptime': Path('/proc/uptime').read_text(), 'load': os.getloadavg()}
    admission()
    assert platform.machine() == 'aarch64' and os.sysconf('SC_PAGE_SIZE') == 4096
    record['system'] = {'uname': list(platform.uname()), 'page_size': os.sysconf('SC_PAGE_SIZE'),
                        'ImageOS': os.environ.get('ImageOS'), 'ImageVersion': os.environ.get('ImageVersion')}
    source_run = api(f'actions/runs/{SOURCE_RUN}')
    artifact = api(f'actions/artifacts/{ARTIFACT}')
    record['source_run_api'] = source_run
    record['source_artifact_api'] = artifact
    save()
    assert source_run['id'] == SOURCE_RUN and source_run['head_sha'] == SOURCE_HEAD
    assert source_run['run_attempt'] == 1 and source_run['status'] == 'completed'
    assert artifact['id'] == ARTIFACT and not artifact['expired']
    assert artifact['name'] == f'a2-candidate-linux-arm64-{CONFIG}'
    assert artifact['workflow_run']['id'] == SOURCE_RUN
    assert artifact['workflow_run']['head_sha'] == SOURCE_HEAD
    manifest = HERE / (CONFIG + '.sha256')
    assert digest(manifest) == MANIFEST
    expected = {}
    for line in manifest.read_text().splitlines():
        value, name = line.split('  ', 1)
        path = INPUT / name
        assert not path.is_symlink() and path.is_file() and digest(path) == value, name
        expected[name] = value
    actual = {str(p.relative_to(INPUT)) for p in INPUT.rglob('*') if p.is_file()}
    assert actual == set(expected), 'artifact file set differs from fixed original manifest'
    receipt_path = INPUT / 'build-result.json'
    assert digest(receipt_path) == RECEIPT
    receipt = json.loads(receipt_path.read_text())
    assert (receipt['head'], receipt['machine'], receipt['config'], receipt['arm']) == (SOURCE_HEAD, 'aarch64', CONFIG, 'candidate')
    assert receipt['compiler']['rc'] == 0
    assert receipt['a2-input-cj_metadata_foreign.so']['rc'] == 127
    reader = Path(os.environ['LLVM_READOBJ']).resolve(strict=True)
    assert reader.is_file() and os.access(reader, os.X_OK)
    record['llvm_reader'] = {'path': str(reader), 'sha256': digest(reader)}
    checked([reader, '--version'], 'llvm-reader-version')
    assert 'LLVM version 18.' in (out / 'llvm-reader-version.log').read_text()
    loader = Path('/lib/ld-linux-aarch64.so.1').resolve(strict=True)
    record['loader'] = {'path': str(loader), 'sha256': digest(loader)}
    fixtures = ['cj_metadata_foreign.so', 'cj_metadata_hole.so', 'cj_metadata_owner.so', 'cj_metadata_contiguous.so']
    names = [*(f'a2-test-build/{name}' for name in fixtures), 'a2-test-build/metadata-code-shape',
             *(f'linked-product/{name}' for name in ('libcangjie-runtime.so', 'libboundscheck.so', 'libcangjie-trace.so'))]
    if CONFIG == 'testable':
        names.append('a2-test-build/metadata')
    copies = []
    for name in names:
        original = INPUT / name
        copy = bundle / original.name
        shutil.copy2(original, copy)
        if original.name in ('metadata-code-shape', 'metadata'):
            copy.chmod(copy.stat().st_mode | 0o111)
        assert digest(copy) == expected[name]
        copies.append({'source': name, 'copy': str(copy), 'sha256': digest(copy),
                       'source_mode': oct(original.stat().st_mode), 'copy_mode': oct(copy.stat().st_mode)})
    record['copies'] = copies
    # This is a new copy identity for the original consumer, never an edited source receipt.
    (bundle / 'a2-candidate.json').write_text(json.dumps({
        'test_elf_sha256': digest(bundle / 'metadata-code-shape'),
        'runtime_sha256': digest(bundle / 'libcangjie-runtime.so'),
        'fixtures': {name: digest(bundle / name) for name in fixtures}}))
    for name in fixtures:
        checked([reader, '--file-headers', '--sections', '--program-headers', bundle / name], 'a2-input-' + name)
    # ldd is static resolution evidence only. Actual loader traces/maps are separate.
    checked(['ldd', bundle / 'metadata-code-shape'], 'static-ldd-shape')
    assert 'not found' not in (out / 'static-ldd-shape.log').read_text()
    if CONFIG == 'testable':
        checked(['ldd', bundle / 'metadata'], 'static-ldd-metadata')
        assert 'not found' not in (out / 'static-ldd-metadata.log').read_text()
        checked([sys.executable, INPUT / 'runtime/tests/gc_unit/metadata/run.py',
                 bundle / 'metadata', out / 'metadata.json'], 'metadata-regression')
    # Reuse exactly the original shape consumer, with no execution of its build/parser/main.
    original_build = INPUT / 'runtime/tests/gc_unit/metadata/build.py'
    assert digest(original_build) == digest(HERE.parent / 'build.py')
    assert digest(INPUT / 'runtime/tests/gc_unit/metadata/run.py') == digest(HERE.parent / 'run.py')
    tree = ast.parse(original_build.read_text())
    function = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'a2_run_bundle')
    exec(compile(ast.Module(body=[function], type_ignores=[]), str(original_build), 'exec'), globals())
    a2_run_bundle()
except BaseException as error:
    record['first_error'] = str(error)
    save()
    raise
finally:
    record['after'] = {'utc': dt.datetime.now(dt.timezone.utc).isoformat(),
                       'uptime': Path('/proc/uptime').read_text(), 'load': os.getloadavg()}
    # Actual dynamic-loader init records are distinct from raw maps and static ldd.
    loader_objects = {}
    for trace in out.glob('actual-loader.*'):
        for name in re.findall(r'calling init:\s+(\S+)', trace.read_text(errors='replace')):
            path = Path(name)
            if path.is_file():
                loader_objects[str(path.resolve())] = digest(path)
    record['actual_loader_init_sha256'] = loader_objects
    if 'copies' in record:
        record['copies_after'] = {item['copy']: digest(Path(item['copy'])) for item in record['copies']}
        record['copies_unchanged'] = all(record['copies_after'][item['copy']] == item['sha256'] for item in record['copies'])
        if not record['copies_unchanged']:
            record['behavior'] = 'INVALID_COPY_CHANGED'
            save()
            raise RuntimeError('fixed artifact copy changed during execution')
    save()
