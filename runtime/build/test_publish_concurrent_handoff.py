#!/usr/bin/env python3
"""Concurrent publication acceptance: two independent configured build trees.

Each arm drives the real publish_runtime_output.py, so every gate invocation is
reached through the production consumer that reads the handoff, checks the
product identity and mirrors the status receipt. The arms run at the same time
and each owns a private staging tree, therefore a private handoff.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def inventory(path):
    return {str(item.relative_to(path)): hashlib.sha256(item.read_bytes()).hexdigest()
            for item in sorted(path.rglob('*')) if item.is_file()}


def read_status(path):
    return dict(line.split('=', 1) for line in path.read_text().splitlines() if '=' in line)


def published_lib(log):
    """The publication directory this arm's own publisher run reported."""
    for line in Path(log).read_text().splitlines():
        if line.startswith('RUNTIME_OUTPUT_PUBLISHED '):
            return Path(line.split('lib_dir=', 1)[1].split()[0])
    return None


def publisher_command(arm):
    command = ['python3', arm['publisher']]
    for name, value in sorted(arm['options'].items()):
        command.extend(['--' + name, value])
    return command


def run_arm(arm, round_index, work):
    name = arm['name']
    directory = work / (str(round_index) + '-' + name)
    directory.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment.pop('GC_UNIT_GATE_RESULT', None)
    environment.update(arm.get('env', {}))
    if round_index:
        environment.update(arm.get('round_env', {}))
    log = directory / 'publisher.stdout.log'
    start = time.monotonic()
    with log.open('w') as stdout:
        result = subprocess.run(publisher_command(arm), env=environment, stdout=stdout,
                                stderr=subprocess.STDOUT, cwd=arm.get('cwd') or None)
    staging = Path(arm['options']['staging'])
    handoffs = sorted(str(path) for path in staging.glob('gate-handoff.*/result.json'))
    return {'name': name, 'round': round_index, 'rc': result.returncode,
            'wall': time.monotonic() - start, 'handoffs': handoffs, 'log': str(log)}


def arm_state(arm, result, errors):
    """Everything the production consumer of this arm's handoff published."""
    name = result['name']
    round_index = result['round']
    if len(result['handoffs']) != 1:
        errors.append('HANDOFF_COUNT ' + name + ' round=' + str(round_index) +
                      ' handoffs=' + str(result['handoffs']))
    root = published_lib(result['log'])
    if root is None:
        errors.append('PUBLICATION_UNREADABLE ' + name + ' round=' + str(round_index))
        return None, None
    published_status = root / 'gc_unit_gate.status'
    runtime_so = Path(arm['options']['runtime'])
    staged_status = runtime_so.parent / 'gc_unit_gate.status'
    if not published_status.is_file() or not staged_status.is_file():
        errors.append('STATUS_MIRROR_MISSING ' + name + ' round=' + str(round_index) +
                      ' published=' + str(published_status.is_file()) +
                      ' staged=' + str(staged_status.is_file()))
        return None, None
    published = read_status(published_status)
    staged = read_status(staged_status)
    if published != staged:
        errors.append('STATUS_MIRROR_DIVERGED ' + name + ' round=' + str(round_index))
    evidence = Path(published['EVIDENCE_DIR'])
    receipt = json.loads((evidence / 'invocation.json').read_text())
    config = published['RUNTIME_CONFIG_ID']
    if receipt['run_id'] != evidence.name or published['RUN_ID'] != evidence.name:
        errors.append('RUN_OWNERSHIP ' + name + ' round=' + str(round_index))
    requested = receipt['requested_inputs']['GCV2_RUNTIME_CONFIG']
    if requested != config or receipt['gate_rc'] != result['rc']:
        errors.append('INVOCATION_BINDING ' + name + ' round=' + str(round_index) +
                      ' requested=' + str(requested) + ' config=' + config +
                      ' rc=' + str(result['rc']))
    identity = receipt['verified_identity']
    if result['rc'] != 0:
        # A failed round has no product verdict; the producer itself only binds
        # the identity when the gate succeeded.
        if identity is not None:
            errors.append('PRODUCT_IDENTITY_UNEXPECTED ' + name + ' round=' + str(round_index))
    elif identity is None:
        errors.append('PRODUCT_IDENTITY_ABSENT ' + name + ' round=' + str(round_index))
    else:
        runtime_sha = published['RUNTIME_SHA256']
        boundscheck_sha = published['BOUNDSCHECK_SHA256']
        if (identity['RUNTIME_CONFIG_ID'] != config or
                identity['RUNTIME_SHA256'] != runtime_sha or
                identity['RUNTIME_SHA256'] != arm['expected_runtime_sha256'] or
                identity['BOUNDSCHECK_SHA256'] != boundscheck_sha or
                identity['BOUNDSCHECK_SHA256'] != arm['expected_boundscheck_sha256']):
            errors.append('PRODUCT_IDENTITY ' + name + ' round=' + str(round_index))
    print('PUBLISH_ATTRIBUTION name=' + name + ' round=' + str(round_index) +
          ' rc=' + str(result['rc']) + ' run=' + evidence.name + ' config=' + config +
          ' runtime_sha256=' + published['RUNTIME_SHA256'] +
          ' handoff=' + str(result['handoffs']), flush=True)
    return evidence, published


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    arms = json.loads(args.plan.read_text())['arms']
    work = args.work
    work.mkdir(parents=True, exist_ok=True)
    errors = []
    records = []
    history = {}
    rounds = max(arm.get('rounds', 1) for arm in arms)
    for round_index in range(rounds):
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(arms)) as executor:
            futures = [executor.submit(run_arm, arm, round_index, work) for arm in arms]
            results = [future.result() for future in futures]
        records.extend(results)
        handoffs = [handoff for result in results for handoff in result['handoffs']]
        for handoff in sorted(set(handoffs)):
            owners = sorted(result['name'] for result in results if handoff in result['handoffs'])
            if len(owners) != 1:
                errors.append('HANDOFF_SHARED ' + handoff + ' owners=' + str(owners))
        current = {}
        for arm, result in zip(arms, results):
            expected = arm.get('expected_rc', [0] * rounds)[round_index]
            if result['rc'] != expected:
                errors.append('PUBLISH_RC ' + result['name'] + ' round=' + str(round_index) +
                              ' actual=' + str(result['rc']) + ' expected=' + str(expected))
            evidence, published = arm_state(arm, result, errors)
            if evidence is not None:
                current[evidence] = inventory(evidence)
        for evidence, before in history.items():
            changed = sorted(name for name, digest in before.items()
                             if current.get(evidence, {}).get(name) != digest)
            print('PUBLISH_HISTORY_ASSERT after_round=' + str(round_index) +
                  ' run=' + evidence.name + ' changed=' + str(changed), flush=True)
            if changed:
                errors.append('PUBLISH_HISTORY_ASSERT after_round=' + str(round_index) +
                              ' run=' + evidence.name + ' changed=' + str(changed))
        for evidence, snapshot in current.items():
            history.setdefault(evidence, snapshot)
    if len({record['name'] for record in records}) != len(arms):
        errors.append('ARM_IDENTITY_COLLISION')
    (work / 'concurrent-publish-result.json').write_text(json.dumps(
        {'records': records, 'errors': errors}, indent=2) + '\n')
    for error in errors:
        print(error, flush=True)
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
