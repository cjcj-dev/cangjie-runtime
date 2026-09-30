#!/usr/bin/env python3
"""Invocation-owned GC gate evidence and atomic compatibility receipts."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile


def atomic_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + '.', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'w') as stream:
            stream.write(text)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def write_json(path, value):
    atomic_text(path, json.dumps(value, sort_keys=True, indent=2) + '\n')


def read_status(path):
    return dict(line.split('=', 1) for line in path.read_text().splitlines() if '=' in line)


def begin(root, run):
    requested = {name: os.environ.get(name) for name in (
        'GCV2_RUNTIME_CONFIG', 'GCV2_RUNTIME_LIB_DIR', 'GC_UNIT_GATE_LANGUAGE_TESTS',
        'GC_UNIT_GATE_STATUS', 'GC_UNIT_TALLY_FILE', 'GC_UNIT_OHOS_HOST_RECEIPT')}
    write_json(run / 'invocation.json', {
        'schema_version': 1, 'run_id': run.name, 'evidence_dir': str(run),
        'requested_inputs': requested, 'state': 'STARTED', 'phase': 'entry',
        'gate_rc': None, 'verified_identity': None, 'cache_source': None,
    })
    for name in ('gate_run.log', 'gate_tally.txt', 'ohos_host.receipt'):
        (root / name).unlink(missing_ok=True)
    cache = root / 'gate-cache.json'
    if cache.is_file():
        try:
            record = json.loads(cache.read_text())
            source = Path(record['evidence_dir'])
            status = read_status(source / 'gate.status')
            if (source.parent != root / 'gate-runs' or status['GATE'] != 'PASS'
                    or json.loads((source / 'invocation.json').read_text())['gate_rc'] != 0):
                return
            for name in ('.gate_stamp', '.gate_language_identity'):
                if (source / name).is_file():
                    shutil.copy2(source / name, run / name)
            write_json(run / 'cache-source.json', record)
        except (OSError, ValueError, KeyError):
            for name in ('.gate_stamp', '.gate_language_identity', 'cache-source.json'):
                (run / name).unlink(missing_ok=True)
            return


def finish(root, run, rc):
    receipt = json.loads((run / 'invocation.json').read_text())
    status = read_status(run / 'gate.status')
    source_file = run / 'cache-source.json'
    source = json.loads(source_file.read_text()) if source_file.is_file() else None
    receipt.update(state=status['GATE'], phase=status['PHASE'], reason=status['REASON'], gate_rc=rc,
                   status=status, cache_candidate=source,
                   cache_source=source if any(status[name] == 'CACHE' for name in (
                       'CPP_SUITE_SOURCE', 'FINALIZER_TRIGGER_SOURCE')) else None,
                   stage_rc={name: None if value == 'NOT_RUN' else int(value)
                             for name, value in status.items() if name.endswith('_RUNNER_RC')},
                   verified_identity={name: status[name] for name in (
                       'RUNTIME_CONFIG_ID', 'RUNTIME_CONFIG_SIGNATURE', 'RUNTIME_SHA256',
                       'BOUNDSCHECK_SHA256')} if status['RUNTIME_SHA256'] != 'unrecorded' else None)
    receipt['outputs'] = {
        str(path.relative_to(run)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(run.rglob('*')) if path.is_file() and path.name != 'invocation.json'
    }
    write_json(run / 'invocation.json', receipt)
    mirrors = {name: root / name for name in ('gate_run.log', 'gate_tally.txt', 'ohos_host.receipt')}
    for variable, name in (('GC_UNIT_EVIDENCE_TALLY_MIRROR', 'gate_tally.txt'),
                           ('GC_UNIT_EVIDENCE_OHOS_MIRROR', 'ohos_host.receipt')):
        if os.environ.get(variable):
            mirrors[name] = Path(os.environ[variable])
    for name, destination in mirrors.items():
        if (run / name).is_file():
            atomic_text(destination, (run / name).read_text())
        else:
            destination.unlink(missing_ok=True)
    handoff = os.environ.get('GC_UNIT_GATE_RESULT')
    if handoff:
        write_json(Path(handoff), {'schema_version': 1, 'run_id': run.name,
                                 'evidence_dir': str(run), 'invocation': str(run / 'invocation.json'),
                                 'status': str(run / 'gate.status')})
    if rc == 0 and status['GATE'] == 'PASS' and (run / '.gate_stamp').is_file():
        write_json(root / 'gate-cache.json', {
            'evidence_dir': str(run), 'run_id': run.name,
            'cpp_source': source['cpp_source'] if status['CPP_SUITE_SOURCE'] == 'CACHE' else str(run),
            'language_source': source['language_source'] if source and status['FINALIZER_TRIGGER_SOURCE'] != 'FRESH' else str(run),
        })


if __name__ == '__main__':
    operation, root_path, run_path, *arguments = sys.argv[1:]
    if operation == 'begin':
        begin(Path(root_path), Path(run_path))
    else:
        finish(Path(root_path), Path(run_path), int(arguments[0]))
