#!/usr/bin/env python3
"""Exercise the real BSD script under Apple /bin/bash, including its old launch line.

Only the launcher is cut. Each arm compiles the same inputs; equal Mach-O hashes
are required before their outcomes may be compared. No product SO claim is made.
"""
import concurrent.futures
import difflib
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import time

BASE = '81618ec3150dcda33a61ba4a171ddf755f5eb1cf'
ROOT = Path(__file__).resolve().parents[3]
SCRIPT = Path('runtime/tests/bsd/run_backing_fail.sh')
OUT = Path(os.environ['BSD_BACKING_REGRESSION_DIR']).resolve()
ARMS = ('baseline', 'candidate', 'cut', 'restored')
OLD_DECL = '''run_prefix=()
if [[ -n "${BSD_BACKING_SIM_UDID:-}" ]]; then
  run_prefix=(xcrun simctl spawn "$BSD_BACKING_SIM_UDID")
fi
'''
OLD_CALL = '  "${run_prefix[@]}" "$work/backing_fail" "$name" >"$work/${name}.out" 2>"$err"\n'
NEW_CALL = '''  if [[ -n "${BSD_BACKING_SIM_UDID:-}" ]]; then
    xcrun simctl spawn "$BSD_BACKING_SIM_UDID" "$work/backing_fail" "$name" >"$work/${name}.out" 2>"$err"
  else
    "$work/backing_fail" "$name" >"$work/${name}.out" 2>"$err"
  fi
'''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    OUT.mkdir(parents=True, exist_ok=True)
    os.chdir(ROOT)
    candidate = SCRIPT.read_text()
    baseline = subprocess.check_output(['git', 'show', f'{BASE}:{SCRIPT}'], text=True)
    assert candidate.count(NEW_CALL) == baseline.count(OLD_CALL) == 1
    assert baseline.count(OLD_DECL) == 1
    # Give the frozen script exactly the candidate's evidence-only EXIT trap.
    observer = candidate[candidate.index('# Optional CI evidence:'):candidate.index('\ncj=')]
    baseline = baseline.replace('trap \'rm -rf "$work"\' EXIT\n', observer)
    cut = candidate.replace(NEW_CALL, OLD_CALL).replace('bc="runtime/', OLD_DECL + 'bc="runtime/', 1)
    (OUT / 'cut.diff').write_text(''.join(difflib.unified_diff(
        candidate.splitlines(True), cut.splitlines(True),
        fromfile=f'a/{SCRIPT}', tofile=f'b/{SCRIPT}')))
    sources = dict(zip(ARMS, (baseline, candidate, cut, candidate)))
    simulator = bool(os.environ.get('BSD_BACKING_SIM_UDID'))
    cases = ['ctor_ok', 'map_ok'] if simulator else [
        'ctor_ok', 'ctor_fail', 'map_ok', 'map_fail', 'unmap_ok', 'unmap_fail']
    metadata = subprocess.check_output([
        '/bin/bash', '-c', 'echo shell=/bin/bash version=$BASH_VERSION; uname -a; '
        'xcrun --sdk "${BSD_BACKING_SDK:-macosx}" --show-sdk-path; git rev-parse HEAD; uptime'
    ], text=True)
    (OUT / 'identity.txt').write_text(metadata)
    print(metadata, end='')
    assert 'version=3.2.' in metadata, 'regression requires Apple system Bash 3.2'

    # Fetch the unchanged dependency once before arms share the source tree.
    bc = ROOT / 'runtime/third_party/third_party_bounds_checking_function'
    if not (bc / 'include/securec.h').exists():
        subprocess.run(['git', 'clone', '--depth', '1', '--branch',
                        'OpenHarmony-v6.0-Release',
                        'https://gitcode.com/openharmony/third_party_bounds_checking_function',
                        str(bc)], check=True)

    def run(arm):
        evidence = OUT / arm
        evidence.mkdir()
        # Keep the original relative root calculation and real compiler/syscalls.
        path = SCRIPT.parent / f'.backing-launcher-{arm}.sh'
        path.write_text(sources[arm])
        (evidence / 'script.sh').write_text(sources[arm])
        env = dict(os.environ, BSD_BACKING_EVIDENCE_DIR=str(evidence),
                   BSD_BACKING_CASES=' '.join(cases))
        start = time.monotonic()
        try:
            with (evidence / 'run.log').open('w') as log:
                result = subprocess.run(['/bin/bash', str(path)], env=env,
                                        stdout=log, stderr=subprocess.STDOUT, timeout=600)
            rc = result.returncode
        finally:
            path.unlink()
        log = (evidence / 'run.log').read_text()
        binary = evidence / 'backing_fail'
        record = {'rc': rc, 'wall': round(time.monotonic() - start, 3),
                  'script_sha256': sha(evidence / 'script.sh'),
                  'macho_sha256': sha(binary) if binary.exists() else None,
                  'cases': {}}
        for name in cases:
            lines = [line for line in log.splitlines()
                     if line.startswith(f'BSD_BACKING_FAIL case={name} rc=')]
            record['cases'][name] = lines or ['NOT_RUN']
        red = not simulator and arm in ('baseline', 'cut')
        if red:
            record['valid'] = (rc != 0 and 'run_prefix[@]: unbound variable' in log
                               and 'BSD_BACKING_FAIL case=' not in log
                               and 'BSD_BACKING_FAIL pass' not in log and binary.exists())
        else:
            expected = {name: 134 if name in ('map_fail', 'unmap_fail') else 0 for name in cases}
            record['valid'] = (rc == 0 and 'BSD_BACKING_FAIL pass' in log and all(
                record['cases'][name] == [f'BSD_BACKING_FAIL case={name} rc={code}']
                for name, code in expected.items()))
        (evidence / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
        # Evidence logs and the executable are retained; intermediates are not.
        for obj in evidence.glob('*.o'):
            obj.unlink()
        return record

    if simulator:
        # One booted UDID is shared; serialize spawn to isolate simulator state.
        results = {arm: run(arm) for arm in ARMS}
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = dict(zip(ARMS, pool.map(run, ARMS)))
    hashes = {result['macho_sha256'] for result in results.values()}
    valid = all(result['valid'] for result in results.values()) and len(hashes) == 1 and None not in hashes
    (OUT / 'results.json').write_text(json.dumps({
        'simulator': simulator, 'parallel_arms': 1 if simulator else 4,
        'same_binary': len(hashes) == 1 and None not in hashes,
        'valid': valid, 'arms': results}, indent=2) + '\n')
    for arm, result in results.items():
        print(f'LAUNCHER arm={arm} rc={result["rc"]} valid={result["valid"]} '
              f'wall={result["wall"]} macho={result["macho_sha256"]}')
    subprocess.run(['uptime'], check=True)
    return 0 if valid else 1


if __name__ == '__main__':
    raise SystemExit(main())
