#!/usr/bin/env python3
"""Cut publication retention/lookup through the complete native build entry.

Run only in an isolated checkout. The real publication test and build entry
run as subprocesses; neither is replaced with a model or mocked producer.
"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--production-entry', required=True, type=Path)
    parser.add_argument('--evidence', required=True, type=Path)
    parser.add_argument('--cut', required=True, choices=['retention', 'lookup'])
    args = parser.parse_args()
    source, evidence = args.source.resolve(), args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    test = source / 'tests/test_runtime_link_publication.py'
    if args.cut == 'retention':
        target = source / 'build/build_cjthread_windows.bat'
        old = '    cd /d "%BUILD_PATH%"'
        new = '    rd /S /Q "%PROJECT_PATH%\\output" & cd /d "%BUILD_PATH%"'
        diagnostic = 'PUBLICATION_METADATA_MISSING'
    else:
        target = test
        old = "        root = source / 'output/temp' / identity"
        new = "        root = source / 'output/temp' / (identity + '-lookup-cut')"
        diagnostic = 'PUBLICATION_LOOKUP_MISMATCH'
    original = target.read_bytes()
    before = original.decode().replace('\r\n', '\n')
    assert before.count(old) == 1, 'cut anchor must identify exactly one real consumer'
    after = before.replace(old, new)
    newline = '\r\n' if b'\r\n' in original else '\n'
    relative = str(target.relative_to(source.parent)).replace('\\', '/')
    (evidence / 'cut.diff').write_text(''.join(difflib.unified_diff(
        before.splitlines(True), after.splitlines(True),
        fromfile='a/' + relative, tofile='b/' + relative)))
    record = {'cut': args.cut, 'target': relative, 'before_sha256': sha(original),
              'driver_sha256': sha(Path(__file__).read_bytes()),
              'test_before_sha256': sha(test.read_bytes())}
    try:
        target.write_bytes(after.replace('\n', newline).encode())
        record['cut_sha256'] = sha(target.read_bytes())
        with (evidence / 'control.log').open('w') as log:
            rc = subprocess.run([sys.executable, str(test), '--source', str(source),
                                 '--production-entry', str(args.production_entry.resolve()),
                                 '--evidence', str(evidence), '--arm', 'candidate'],
                                stdout=log, stderr=subprocess.STDOUT).returncode
        record['test_rc'] = rc
        result = json.loads((evidence / 'result.json').read_text())
        record['production_entry_rc'] = result['steps']['production-entry']['rc']
        lookup = result.get('candidate-lookup', {})
        record['lookup'] = lookup
        log = (evidence / 'control.log').read_text(errors='replace')
        missing = lookup.get('missing_metadata')
        expected_missing = ['runtime-build-config.txt', 'runtime-build-inputs.txt',
                            'runtime-product-hashes.json'] if args.cut == 'retention' else []
        record['exact_cut'] = (rc != 0 and record['production_entry_rc'] == 0
                               and lookup.get('lookup_assertion_rc') == int(args.cut == 'lookup')
                               and missing == expected_missing
                               and f'AssertionError: {diagnostic}:' in log)
    finally:
        target.write_bytes(original)
        record['restored_sha256'] = sha(target.read_bytes())
        (evidence / 'control-result.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'PUBLICATION_METADATA_CONTROL cut={args.cut} test_rc={rc} '
          f'exact={record["exact_cut"]}', flush=True)
    assert record['exact_cut'], 'cut did not reach the intended publication assertion; see control.log'
    assert record['before_sha256'] == record['restored_sha256'], 'cut source was not restored'


if __name__ == '__main__':
    main()
