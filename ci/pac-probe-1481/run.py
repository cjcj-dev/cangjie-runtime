"""One admitted compile-only batch. Never execute generated code."""
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import subprocess
import time

LANE = 'sym_cangjie_runtime_1481_implement_r5946918200'
DEV = '/Applications/Xcode_16.4.app/Contents/Developer'
OUT = Path(os.environ['RUNNER_TEMP']) / LANE
OUT.mkdir(parents=True, exist_ok=False)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
records = []


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(name, argv):
    start = time.monotonic()
    try:
        result = subprocess.run(argv, capture_output=True, timeout=120, check=False)
        rc, stdout, stderr = result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired as exc:
        rc, stdout, stderr = 124, exc.stdout or b'', exc.stderr or b''
    except OSError as exc:
        rc, stdout, stderr = 127, b'', str(exc).encode()
    (OUT / (name + '.stdout')).write_bytes(stdout)
    (OUT / (name + '.stderr')).write_bytes(stderr)
    record = dict(id=name, argv=argv, rc=rc, wall=time.monotonic()-start)
    records.append(record)
    (OUT / 'commands.json').write_text(json.dumps(records, indent=2))
    return rc, stdout.decode(errors='replace').strip()


def stop(reason):
    (OUT / 'result.json').write_text(json.dumps(dict(status='NOT_RUN', reason=reason,
        behavior='NOT_RUN', candidate=os.environ.get('GITHUB_SHA')), indent=2))
    raise SystemExit(0)


# The default active identity is separate; never use it to select the batch compiler.
run('active-before', ['xcode-select', '-p'])
if not Path(DEV).is_dir():
    stop('fixed Xcode_16.4 developer directory missing')
os.environ['DEVELOPER_DIR'] = DEV
identity = {'DEVELOPER_DIR': DEV, 'candidate': os.environ['GITHUB_SHA'],
            'ImageOS': os.environ.get('ImageOS'), 'ImageVersion': os.environ.get('ImageVersion')}
for name, argv in [
    ('sw-vers', ['sw_vers']), ('uname', ['uname', '-a']),
    ('xcodebuild', ['xcodebuild', '-version']),
    ('clang-path', ['xcrun', '--find', 'clang']),
    ('clang-version', ['xcrun', 'clang', '--version']),
    ('resource-dir', ['xcrun', 'clang', '-print-resource-dir']),
    ('sdk-path', ['xcrun', '--sdk', 'macosx', '--show-sdk-path']),
    ('sdk-version', ['xcrun', '--sdk', 'macosx', '--show-sdk-version']),
    ('sdk-build', ['xcrun', '--sdk', 'macosx', '--show-sdk-build-version'])]:
    rc, value = run(name, argv)
    identity[name] = dict(rc=rc, value=value)
(OUT / 'identity.json').write_text(json.dumps(identity, indent=2))
if any(identity[x]['rc'] for x in ['xcodebuild', 'clang-path', 'resource-dir', 'sdk-path']):
    stop('required fixed-toolchain identity command failed')
clang = str(Path(identity['clang-path']['value']).resolve())
sdk = identity['sdk-path']['value']
header = Path(identity['resource-dir']['value']) / 'include/ptrauth.h'
if not header.is_file() or not Path(clang).is_file() or not Path(sdk).is_dir():
    stop('required clang/header/SDK entity missing')
identity['clang-realpath'] = clang
identity['clang-sha256'] = sha(clang)
identity['ptrauth-header'] = str(header)
identity['ptrauth-sha256'] = sha(header)
shutil.copyfile(header, OUT / 'ptrauth.h')
for relative in ['SDKSettings.json', 'SDKSettings.plist', 'System/Library/CoreServices/SystemVersion.plist']:
    source = Path(sdk) / relative
    identity[relative] = {'exists': source.is_file()}
    if source.is_file():
        identity[relative]['sha256'] = sha(source)
        shutil.copyfile(source, OUT / source.name)
identity['source-sha256'] = {str(p): sha(p) for p in Path('ci/pac-probe-1481').iterdir() if p.is_file()}
identity['workflow-sha256'] = sha('.github/workflows/pac-probe-1481.yml')
(OUT / 'identity.json').write_text(json.dumps(identity, indent=2))
# Configure sccache but explicitly disable caching for every compiler invocation.
# No cache restore/save action; the wrapper receives the real absolute clang path.
os.environ['SCCACHE_DISABLE'] = '1'
os.environ['SCCACHE_DIR'] = str(OUT / 'sccache')
run('sccache-version', ['sccache', '--version'])
matrix = json.loads(Path('ci/pac-probe-1481/argv.json').read_text())
if len(matrix) != 15:
    stop('frozen matrix does not contain exactly 15 commands')
for item in matrix:
    argv = [arg.replace('${CLANG}', clang).replace('${SDK}', sdk).replace('${OUT}', str(OUT)) for arg in item['argv']]
    run(item['id'], argv)
run('sccache-stats', ['sccache', '--show-stats'])
manifest = {str(p.relative_to(OUT)): sha(p) for p in OUT.rglob('*') if p.is_file()}
(OUT / 'sha256.json').write_text(json.dumps(manifest, indent=2))
(OUT / 'result.json').write_text(json.dumps(dict(status='compile-batch-collected',
    behavior='NOT_RUN', candidate=os.environ['GITHUB_SHA'], invocations=15,
    interpretation='Read API expansion and assembly individually; compile success grants no ABI or execution admission.'), indent=2))
