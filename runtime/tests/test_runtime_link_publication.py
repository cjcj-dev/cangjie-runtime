#!/usr/bin/env python3
"""Check native generated link inputs through the real runtime build/publisher.

Windows starts cjcj#589's unmodified production entry. Linux can reuse a real
runtime build (--build). Evidence includes raw entry/target assertion rc, input
and product hashes, publication metadata, and controlled missing-input failures.
"""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path, help='runtime directory')
    parser.add_argument('--evidence', required=True, type=Path)
    parser.add_argument('--build', type=Path)
    parser.add_argument('--production-entry', type=Path, help='cjcj#589 checkout')
    parser.add_argument('--arm', choices=['candidate', 'cut-producer', 'cut-consumer', 'restored'], default='candidate')
    args = parser.parse_args()
    if args.production_entry:
        args.production_entry = args.production_entry.resolve()
    source = args.source.resolve()
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    build = args.build.resolve() if args.build else source / 'CMakebuild'
    producer = source / 'build/cmake/RuntimeOutputLayout.cmake'
    consumer = source / 'build/publish_runtime_output.py'
    env = dict(os.environ, GC_UNIT_GATE_SKIP='1')
    record = {'arm': args.arm, 'jobs': os.cpu_count(), 'steps': {},
              'test_sha256': sha(Path(__file__)),
              'source_sha256': {str(p.relative_to(source)): sha(p) for p in (producer, consumer)}}

    def save():
        (evidence / 'result.json').write_text(json.dumps(record, indent=2) + '\n')

    def run(name, command, cwd=source):
        start = time.monotonic()
        with (evidence / (name + '.log')).open('w') as log:
            log.write(repr([str(x) for x in command]) + '\n')
            log.flush()
            rc = subprocess.run([str(x) for x in command], cwd=cwd, env=env,
                                stdout=log, stderr=subprocess.STDOUT).returncode
        record['steps'][name] = {'rc': rc, 'wall': time.monotonic() - start}
        save()
        print(f'LINK_PUBLICATION_STEP {name} rc={rc} wall={time.monotonic() - start:.2f}', flush=True)
        return rc

    # Product-side cuts restore the old link.txt assumption. Neither replaces
    # the product entry nor adds an unconditional failure to the publisher.
    if args.arm in ('cut-producer', 'restored'):
        before = producer.read_text()
        after = before.replace('set(_link_input build.make)', 'set(_link_input link.txt)')
        assert before != after
        producer.write_text(after)
        (evidence / 'cut.diff').write_text(''.join(difflib.unified_diff(
            before.splitlines(True), after.splitlines(True),
            fromfile='a/runtime/build/cmake/RuntimeOutputLayout.cmake',
            tofile='b/runtime/build/cmake/RuntimeOutputLayout.cmake')))
        if args.arm == 'restored':
            producer.write_text(before)
    elif args.arm == 'cut-consumer':
        before = consumer.read_text()
        after = before.replace('paths = [Path(line) for line in', 'paths = [Path(line).with_name("link.txt") for line in')
        assert before != after
        consumer.write_text(after)
        (evidence / 'cut.diff').write_text(''.join(difflib.unified_diff(
            before.splitlines(True), after.splitlines(True),
            fromfile='a/runtime/build/publish_runtime_output.py',
            tofile='b/runtime/build/publish_runtime_output.py')))
    record['tested_source_sha256'] = {str(p.relative_to(source)): sha(p) for p in (producer, consumer)}
    save()
    if args.production_entry:
        assert os.name == 'nt', 'production entry acceptance requires native Windows'
        env['RUNTIME_SOURCE'] = str(source.parent)
        env['RUNTIME_REF'] = subprocess.check_output(['git', '-C', source.parent, 'rev-parse', 'HEAD'], text=True).strip()
        env['RUNTIME_TARGET'] = 'windows-x86_64'
        record['runtime_ref'] = env['RUNTIME_REF']
        record['entry_ref'] = subprocess.check_output(['git', '-C', args.production_entry, 'rev-parse', 'HEAD'], text=True).strip()
        entry = args.production_entry / 'ci/platform_matrix/build_runtime.mjs'
        record['entry_sha256'] = sha(entry)
        rc = run('production-entry', ['cmd', '/c', 'npx', '--yes', 'zx@8', entry], args.production_entry)
    else:
        assert args.build and args.arm == 'candidate'
        rc = run('production-entry', ['cmake', '--build', build, '--target', 'publish_runtime_output', '--parallel', os.cpu_count()])
    record['publication_assertion_rc'] = int(rc != 0)
    print(f'POST_LINK_PUBLICATION_ASSERT arm={args.arm} rc={record["publication_assertion_rc"]}', flush=True)
    save()
    if args.arm.startswith('cut-'):
        log = (evidence / 'production-entry.log').read_text(errors='replace')
        products = list((build / 'runtime-staging').rglob('libcangjie-runtime.dll'))
        record['linked_products'] = {str(p): sha(p) for p in products}
        exact = rc != 0 and 'RUNTIME_OUTPUT_PUBLISH_FAIL:' in log and 'link.txt' in log and bool(products)
        record['exact_cut'] = exact
        save()
        assert exact, 'cut did not reach the post-link missing-link-input assertion'
        return
    assert rc == 0, 'production post-link publication failed'

    def publication(name):
        cache = (build / 'CMakeCache.txt').read_text()
        identity = re.search(r'^CANGJIE_RUNTIME_CONFIG_ID:INTERNAL=(.*)$', cache, re.M)[1]
        root = source / 'output/temp' / identity
        inputs = json.loads((root / 'runtime-build-inputs.txt').read_text())
        for filename in ('runtime-build-config.txt', 'runtime-build-inputs.txt', 'runtime-product-hashes.json'):
            shutil.copy2(root / filename, evidence / (name + '-' + filename))
        record[name] = {'id': identity, 'products': inputs['products'], 'links': list(inputs['links'])}
        save()
        return identity, inputs

    original_id, original_inputs = publication('candidate')
    listed = [Path(line) for line in (build / 'runtime-link-inputs.txt').read_text().splitlines() if line]
    expected_name = 'build.make' if os.name == 'nt' else 'link.txt'
    assert listed and all(p.name == expected_name and p.is_file() for p in listed)
    selected = next(p for p in listed if p.parent.name == 'cangjie-runtime.dir')
    content = selected.read_text()
    assert 'clang++' in content, 'actual runtime linker command is absent'
    if os.name == 'nt':
        assert not selected.with_name('link.txt').exists(), 'Windows test must exercise the no-link-script generator'
    print(f'NATIVE_LINK_INPUT_ASSERT file={selected}', flush=True)
    publish_args = (build / 'runtime-publish-args.txt').read_text().splitlines()
    runtime = Path(publish_args[publish_args.index('--runtime') + 1])
    publisher_command = ['python3', consumer, '@' + str(build / 'runtime-publish-args.txt')]
    # Change a genuine linker flag, force the real linker/post-build entry, and
    # inspect the product's recorded input rather than calling a model helper.
    option = '-Wl,--nxcompat' if os.name == 'nt' else '-Wl,--as-needed'
    changed = re.sub(r'(?m)^(.*clang\+\+[^\n]*)$', lambda m: m[1] + ' ' + option, content)
    assert changed != content
    try:
        selected.write_text(changed)
        runtime.unlink()
        assert run('changed-link-build', ['cmake', '--build', build, '--target', 'cangjie-runtime', '--parallel', os.cpu_count()]) == 0
        changed_id, changed_inputs = publication('changed')
        print(f'LINK_IDENTITY_ASSERT before={original_id} after={changed_id}', flush=True)
        assert original_id != changed_id, 'actual linker input change did not change identity'
        assert any(option in value for value in changed_inputs['links'].values()), 'changed link command was not recorded'
    finally:
        selected.write_text(content)
    # Missing genuine inputs must fail even with the linked DLL/SO still present.
    response_files = []
    for key in original_inputs['links']:
        if key.endswith('.rsp'):
            response_files.append(Path(key.replace('<BUILD>', str(build))))
    for index, missing in enumerate([selected] + response_files[:1]):
        saved = missing.read_bytes()
        try:
            missing.unlink()
            missing_rc = run(f'missing-input-{index}', publisher_command)
            text = (evidence / f'missing-input-{index}.log').read_text(errors='replace')
            print(f'MISSING_LINK_INPUT_ASSERT file={missing} rc={missing_rc}', flush=True)
            assert missing_rc == 2 and 'RUNTIME_OUTPUT_PUBLISH_FAIL:' in text and missing.name in text
        finally:
            missing.write_bytes(saved)
    assert run('restored-publish', publisher_command) == 0
    publication('restored')
    record['controls_passed'] = True
    save()


if __name__ == '__main__':
    main()
