#!/usr/bin/env python3
"""Assert stderr from the real OHOS-host Logger and from the hilog adapter.

Pass an ohos_logging_probe ELF linked to (not containing) the runtime logger.
Reuse the same ELF with candidate, cut and restored OHOS-host SO directories.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--runtime-lib-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.pop('MRT_LOG_PATH', None)
    env['LD_LIBRARY_PATH'] = str(args.runtime_lib_dir.resolve())
    artifacts = [args.elf, args.runtime_lib_dir / 'libcangjie-runtime.so',
                 args.runtime_lib_dir / 'libboundscheck.so']
    hashes = {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts}
    cases = {
        'fatal': (-signal.SIGABRT, ['[FATAL] CANGJIE-RUNTIME: ohos-host diagnostic value=1244 text=visible']),
        'info': (0, ['[INFO] CANGJIE-RUNTIME: ohos-host diagnostic value=1244 text=visible']),
        'shim': (0, ['[DEBUG] CANGJIE-RUNTIME: plain=42', '[INFO] CANGJIE-RUNTIME: public=visible',
                     '[WARN] CANGJIE-RUNTIME: private=17', '[ERROR] CANGJIE-RUNTIME: width=  1.25',
                     '[FATAL] CANGJIE-RUNTIME: literal=%{public}s percent=% value=23']),
    }
    results = {}
    for name, (expected_rc, lines) in cases.items():
        run = subprocess.run([str(args.elf.resolve()), name], env=env, capture_output=True, timeout=30)
        (args.output / (name + '.stdout')).write_bytes(run.stdout)
        (args.output / (name + '.stderr')).write_bytes(run.stderr)
        started = name == 'shim' or b'OHOS_LOGGING_ENTRY Logger::FormatLog' in run.stdout
        # Evaluate the diagnostic assertion even when the independent exit check fails.
        observed = run.stderr.decode(errors='replace').splitlines()
        diagnostic = all(line in observed for line in lines)
        exit_ok = run.returncode == expected_rc
        passed = started and diagnostic and exit_ok
        results[name] = dict(rc=run.returncode, started=started, diagnostic=diagnostic,
                             exit_ok=exit_ok, passed=passed, stderr_bytes=len(run.stderr))
        print(f'OHOS_LOGGING_ASSERT case={name} diagnostic={int(diagnostic)} '
              f'exit_ok={int(exit_ok)} started={int(started)} rc={run.returncode} '
              f'status={"PASS" if passed else "FAIL"}', flush=True)
    (args.output / 'result.json').write_text(json.dumps(dict(hashes=hashes, cases=results), indent=2) + '\n')
    return int(not all(result['passed'] for result in results.values()))


if __name__ == '__main__':
    raise SystemExit(main())
