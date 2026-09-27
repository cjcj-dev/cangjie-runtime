#!/usr/bin/env python3
"""Cut actual OHOS configure edges and observe their generated compilation routes.

Artifact identity protocol (B1 rework):
Every arm of one recipe group builds in the SAME source/build path, sequentially,
so a byte difference can never come from the arm's own directory. Diagnosis on
the same tree showed libcangjie-thread.a embeds the source/build path and the
SDK path through DWARF (same path rebuild is byte-identical; a different source,
build or SDK path is not). Route cuts only re-point the toolchain to a
byte-identical copy of the SDK, so the cuts must NOT change any binary: arms are
grouped into identity classes by (recipe, effective SDK path) and every artifact
inside a class must be byte-identical, otherwise the arm is invalid.

Classes per recipe group (identity is compared byte-for-byte, with the
CJRT-COMMIT source digest zeroed in .so files — a cut mutates runtime source,
so the digest must change while every other byte must not):
  real SDK : candidate, restored, strip-cut, rename-cut (plus all cuts on the
             non-target architecture, where the mutation is inert)
  alternate SDK : sdkpath-control, producer-cut, consumer-cut
             (the control arm routes the unmutated tree to the alternate SDK;
             the cut arms must reproduce its bytes exactly)
build.py recipe: buildpy-restored (real), buildpy-control / buildpy-cut (alternate).

The route observer always asserts against the REAL SDK path (the product
invariant), so any arm routed at the alternate SDK turns red on exactly the
rerouted assertions; control arms expect the full reroute set.
"""
import concurrent.futures
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

repo = Path.cwd()
arch = sys.argv[1]
sdk = Path(os.environ['PUBLIC_NATIVE'])
root = Path(os.environ['RUNNER_TEMP']) / ('ohos-controls-' + arch)
out = repo / 'ohos-evidence' / 'controls'
root.mkdir(parents=True, exist_ok=True)
out.mkdir(parents=True, exist_ok=True)
alternative = root / 'alternate-native'
if not alternative.exists():
    shutil.copytree(sdk, alternative, symlinks=False)
base = '10965c352184f366d9e46fc75341905724bf40d5'
with (out / 'baseline-fetch.log').open('w') as log:
    subprocess.run(['git', 'fetch', '--no-tags', '--depth=1', 'origin', base],
                   cwd=repo, stdout=log, stderr=subprocess.STDOUT, check=True)
observer = repo / 'runtime/tests/test_ohos_public_sdk.py'
(out / 'observer.sha256').write_text(hashlib.sha256(observer.read_bytes()).hexdigest() + '\n')

reroute_cuts = ('producer-cut', 'consumer-cut', 'buildpy-cut')
control_arms = ('sdkpath-control', 'buildpy-control')


def sdk_for(arm):
    """Effective SDK feeding the products (identity class axis)."""
    if arm in control_arms:
        return alternative
    if arch == 'x86_64' and arm in reroute_cuts:
        return alternative
    return sdk


def entry_sdk_for(arm):
    """SDK handed to the entry. Cut arms get the REAL SDK: the reroute must
    come from the mutation alone, otherwise the cut proves nothing."""
    return alternative if arm in control_arms else sdk


def normalized_identity(path):
    """Byte identity modulo the runtime provenance stamp.

    libcangjie-runtime.so embeds CJRT-COMMIT:src-<sha256>, a digest of the
    runtime source content (runtime/build/cmake/GenerateRuntimeProvenance.cmake).
    A cut mutates runtime source, so the stamp MUST differ between a cut arm
    and its unmutated control; everything else MUST NOT. Normalizing the stamp
    keeps the comparison live instead of dropping the .so from identity.
    """
    data = path.read_bytes()
    return re.sub(b'CJRT-COMMIT:src-[0-9a-f]{64}', b'CJRT-COMMIT:src-' + b'0' * 64, data)


def mutate(source, arm):
    if arm == 'producer-cut':
        path = source / 'runtime/CMakeLists.txt'
        before = path.read_text()
        old = 'execute_process(COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/build/build_cjthread.sh" -p ohos_x86_64_cangjie'
        new = 'execute_process(COMMAND "${CMAKE_COMMAND}" -E env "OHOS_PUBLIC_SDK=$ENV{OHOS_ROUTE_CONTROL_SDK}" bash "${CMAKE_CURRENT_SOURCE_DIR}/build/build_cjthread.sh" -p ohos_x86_64_cangjie'
    elif arm == 'consumer-cut':
        path = source / 'runtime/build/cmake/toolchain/ohos_x86_64_cangjie.cmake'
        before = path.read_text()
        old = 'set(CMAKE_CXX_COMPILER "$ENV{OHOS_PUBLIC_SDK}/llvm/bin/clang++${EXECUTABLE_EXTENSION}")'
        new = 'set(CMAKE_CXX_COMPILER "$ENV{OHOS_ROUTE_CONTROL_SDK}/llvm/bin/clang++${EXECUTABLE_EXTENSION}")'
    elif arm == 'buildpy-cut':
        path = source / 'runtime/build.py'
        before = path.read_text()
        old = 'os.path.abspath(args.ohos_public_sdk) if args.ohos_public_sdk else ""'
        new = 'os.environ["OHOS_ROUTE_CONTROL_SDK"] if args.target == "ohos-x86_64" else (' + old + ')'
    elif arm == 'strip-cut':
        path = source / 'runtime/CMakeLists.txt'
        before = path.read_text()
        old = 'set(STRIP_PROGRAM "${OHOS_PUBLIC_SDK}/llvm/bin/llvm-strip")'
        new = old + '\n            if (OHOS_FLAG MATCHES 2)\n                set(STRIP_PROGRAM "$ENV{OHOS_ROUTE_CONTROL_SDK}/llvm/bin/llvm-strip")\n            endif()'
    elif arm == 'rename-cut':
        path = source / 'runtime/config.cmake'
        before = path.read_text()
        old = '    set(OHOS_LIB "${_ohos_ndk_lib}")'
        new = old + '\n    if (OHOS_FLAG MATCHES 2)\n        configure_file("${_ohos_llvm_lib}/libc++.so" "${CMAKE_BINARY_DIR}/libstdc++.so" COPYONLY)\n    endif()'
    else:
        return
    if before.count(old) != 1:
        raise RuntimeError('Cut anchor changed: ' + arm)
    after = before.replace(old, new)
    path.write_text(after)
    relative = path.relative_to(source).as_posix()
    (out / (arm + '.diff')).write_text(''.join(difflib.unified_diff(
        before.splitlines(True), after.splitlines(True), 'a/' + relative, 'b/' + relative)))


def expected_for(arm):
    if arm == 'baseline' and arch == 'aarch64':
        # Base already routes aarch64 cjthread through OHOS_PUBLIC_SDK
        # (runtime/CMakeLists.txt OHOS_FLAG==1 exports it; ohos_aarch64_cangjie.cmake
        # consumes it), so only the strip route and the library rename remain as
        # baseline failures.
        return ['runtime.strip_route', 'no_runtime_library_rename']
    if arch == 'x86_64' and arm in ('producer-cut', 'consumer-cut'):
        return ['cjthread.sdk_route']
    if arch == 'x86_64' and arm == 'buildpy-cut':
        return ['runtime.sdk_route', 'cjthread.sdk_route', 'runtime.strip_route']
    if arch == 'x86_64' and arm == 'strip-cut':
        return ['runtime.strip_route']
    if arch == 'x86_64' and arm == 'rename-cut':
        return ['no_runtime_library_rename']
    if arm in ('sdkpath-control', 'buildpy-control'):
        # Unmutated tree routed wholesale at the alternate SDK: every route
        # assertion fires. This arm is the identity reference for the cut arms,
        # not a route-green arm.
        return ['runtime.sdk_route', 'cjthread.sdk_route', 'runtime.strip_route']
    return []


def materialize(arm, source):
    if source.exists():
        shutil.rmtree(source)
    source.mkdir()
    if arm == 'baseline':
        archive = subprocess.Popen(['git', 'archive', base], cwd=repo, stdout=subprocess.PIPE)
        unpacked = subprocess.run(['tar', '-x', '-C', str(source)], stdin=archive.stdout)
        archive.stdout.close()
        if archive.wait() or unpacked.returncode:
            raise RuntimeError('Baseline archive failed')
    else:
        tracked = subprocess.check_output(['git', 'ls-files', '-z'], cwd=repo).decode().split('\0')
        for relative in filter(None, tracked):
            dest = source / relative
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(repo / relative, dest)
    if arm in ('restored', 'buildpy-restored'):
        path = source / ('runtime/build.py' if arm == 'buildpy-restored' else 'runtime/CMakeLists.txt')
        original = path.read_bytes()
        mutate(source, 'buildpy-cut' if arm == 'buildpy-restored' else 'producer-cut')
        path.write_bytes(original)
        assert path.read_bytes() == (repo / path.relative_to(source)).read_bytes()
    elif arm not in ('sdkpath-control', 'buildpy-control'):
        mutate(source, arm)


def run(arm, source):
    started = time.monotonic()
    materialize(arm, source)
    identities = {}
    for relative in ('runtime/CMakeLists.txt', 'runtime/config.cmake', 'runtime/build.py',
                     'runtime/build/cmake/toolchain/ohos_x86_64_cangjie.cmake',
                     'runtime/build/cmake/toolchain/ohos_aarch64_cangjie.cmake',
                     'runtime/src/CJThread/CMakeLists.txt'):
        identities[relative] = hashlib.sha256((source / relative).read_bytes()).hexdigest()
    (out / (arm + '-source.json')).write_text(json.dumps(identities, indent=2) + '\n')
    env = os.environ.copy()
    env.pop('OHOS_ROOT', None)
    env.pop('OHOS_PUBLIC_SDK', None)
    env['OHOS_ROUTE_CONTROL_SDK'] = str(alternative)
    env['CMAKE_EXPORT_COMPILE_COMMANDS'] = 'ON'
    build = source / 'configured'
    entry_sdk = entry_sdk_for(arm)
    command = ['cmake', '-S', str(source / 'runtime'), '-B', str(build),
               '-DCMAKE_BUILD_TYPE=Release', '-DOHOS_FLAG=' + ('2' if arch == 'x86_64' else '1'),
               '-DOHOS_PUBLIC_SDK=' + str(entry_sdk), '-DCMAKE_INSTALL_PREFIX=' + str(source / 'install'),
               '-DCOPYGC_FLAG=1', '-DDOPRA_FLAG=1', '-DRUNTIME_TRACE_FLAG=0',
               '-DCJ_SDK_VERSION=0.0.1', '-DDISABLE_VERSION_CHECK=1']
    if arch == 'aarch64':
        command += ['-DRUNTIME_FORWARD_PTRAUTH_CFI=1', '-DRUNTIME_BACKWARD_PTRAUTH_CFI=1']
    if arm.startswith('buildpy-'):
        command = [sys.executable, str(source / 'runtime/build.py'), 'build', '-t', 'release',
                   '--target', 'ohos-' + arch, '--ohos-public-sdk', str(entry_sdk), '-v', '0.0.1']
        build = source / 'runtime/CMakebuild'
    with (out / (arm + '.log')).open('w') as log:
        log.write(json.dumps(command) + '\n'); log.flush()
        configured = subprocess.run(command, cwd=source / 'runtime', env=env, stdout=log, stderr=subprocess.STDOUT)
        observed = None
        if configured.returncode == 0:
            observed = subprocess.run([sys.executable, str(observer), '--build', str(build),
                                       '--sdk', str(sdk), '--arch', arch,
                                       '--output', str(out / (arm + '.json'))]
                                      + ([] if arm.startswith('buildpy-') else ['--configured-only']),
                                      stdout=log, stderr=subprocess.STDOUT).returncode
    expected = expected_for(arm)
    failures = None
    artifacts = {}
    identity_hashes = {}
    stamps = {}
    observation = out / (arm + '.json')
    if observation.exists():
        parsed = json.loads(observation.read_text())
        failures = [c['name'] for c in parsed['checks'] if not c['pass']]
        for entry in parsed['artifacts']:
            artifacts[Path(entry['path']).name] = entry['sha256']
        keep = out / 'artifacts' / arm
        keep.mkdir(parents=True, exist_ok=True)
        for entry in parsed['artifacts']:
            target = keep / Path(entry['path']).name
            shutil.copy2(entry['path'], target)
            identity_hashes[target.name] = hashlib.sha256(normalized_identity(target)).hexdigest()
            match = re.search(b'CJRT-COMMIT:[^\\\\]+', target.read_bytes())
            if match:
                stamps[target.name] = match.group(0).decode()
    record = {'arm': arm, 'arch': arch, 'configure_rc': configured.returncode,
              'observer_rc': observed, 'failures': failures, 'expected_failures': expected,
              'sdk_path': str(sdk_for(arm)), 'artifacts': artifacts,
              'identity_hashes': identity_hashes, 'stamps': stamps,
              'wall': round(time.monotonic() - started, 2)}
    record['routes_valid'] = (configured.returncode == 0 and failures == expected
                              and observed == (1 if expected else 0))
    if arm == 'baseline' and arch == 'x86_64':
        record['routes_valid'] = configured.returncode != 0
        record['note'] = 'Original configure failure reproduced; not counted as assertion-level red evidence.'
    (out / (arm + '-result.json')).write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record), flush=True)
    return record


def identity_class(record):
    if record['arm'] == 'baseline':
        return None
    recipe = 'buildpy' if record['arm'].startswith('buildpy-') else 'cmake'
    return recipe + '|' + record['sdk_path']


def run_group(arms, source):
    records = [run(arm, source) for arm in arms]
    classes = {}
    for record in records:
        name = identity_class(record)
        if name:
            classes.setdefault(name, []).append(record)
    identity = []
    for name, members in sorted(classes.items()):
        reference = members[0]['identity_hashes']
        table = {'class': name, 'reference_arm': members[0]['arm'],
                 'arms': {m['arm']: m['artifacts'] for m in members},
                 'identity_basis': 'normalized (CJRT-COMMIT source digest zeroed for .so)',
                 'stamps': {m['arm']: m['stamps'] for m in members}}
        diverged = [m['arm'] for m in members if m['identity_hashes'] != reference]
        # Positive control: in a class pairing an unmutated control with cut arms,
        # the .so provenance stamp MUST differ (the mutation changes the runtime
        # source digest); an identical stamp means the stamp is dead or the
        # mutation did not land in the product source.
        controls = [m for m in members if m['arm'] in control_arms]
        cuts = [m for m in members if m['arm'] in reroute_cuts]
        stamp_dead = []
        for control in controls:
            for cut in cuts:
                shared = set(control['stamps']) & set(cut['stamps'])
                if shared and all(control['stamps'][s] == cut['stamps'][s] for s in shared):
                    stamp_dead.append(cut['arm'])
        table['identical'] = not diverged
        table['diverged_arms'] = diverged
        table['stamp_dead_arms'] = stamp_dead
        identity.append(table)
        for m in members:
            m['identity_class'] = name
            m['identity_valid'] = not diverged and m['arm'] not in stamp_dead
            m['valid'] = m['routes_valid'] and m['identity_valid']
            (out / (m['arm'] + '-result.json')).write_text(json.dumps(m, indent=2) + '\n')
    for record in records:
        record.setdefault('valid', record['routes_valid'])
        (out / (record['arm'] + '-result.json')).write_text(json.dumps(record, indent=2) + '\n')
    return records, identity


subprocess.run(['uptime'], check=True)
groups = [(['baseline', 'candidate', 'sdkpath-control', 'producer-cut', 'consumer-cut',
            'rename-cut', 'strip-cut', 'restored'], root / 'tree-cmake'),
          (['buildpy-restored', 'buildpy-control', 'buildpy-cut'], root / 'tree-buildpy')]
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
    grouped = list(pool.map(lambda g: run_group(*g), groups))
subprocess.run(['uptime'], check=True)
results = [r for records, _ in grouped for r in records]
identity = [t for _, tables in grouped for t in tables]
# Cross-class sample: where the first byte diverges between SDK-path classes of the
# cmake recipe (documentation of the path-bearing artifact class, not a gate).
kept = out / 'artifacts'
sample = {}
real_a = kept / 'candidate' / 'libcangjie-thread.a'
alt_a = kept / 'sdkpath-control' / 'libcangjie-thread.a'
if real_a.exists() and alt_a.exists():
    probe = subprocess.run(['cmp', str(real_a), str(alt_a)], capture_output=True, text=True)
    sample = {'candidate_vs_sdkpath_control_cmp': probe.stdout.strip(),
              'note': 'Identical toolchain content at different SDK paths; '
                      'divergence is the path-bearing bytes (DWARF) documented in the report.'}
summary = {'arch': arch, 'results': results, 'identity': identity, 'cross_class_sample': sample}
(out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
raise SystemExit(0 if all(r['valid'] for r in results) else 1)
