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
    if os.name == 'nt':
        # Keep a successfully linked DLL when a later POST_BUILD check fails.
        # This changes Make's cleanup only; all product commands still run.
        precious = evidence / 'preserve-products.make'
        # CMake emits explicit rules: a .PRECIOUS pattern alone does not
        # protect them from .DELETE_ON_ERROR. These are the two native x86_64
        # target paths emitted with/without CMAKE_SYSTEM_PROCESSOR populated.
        precious.write_text('.PRECIOUS: % runtime-staging/bin/_Release/libcangjie-runtime.dll '
                            'runtime-staging/bin/x86_64_Release/libcangjie-runtime.dll\n')
        env['MAKEFILES'] = precious.as_posix()
        env['PATH'] = 'C:/msys64/mingw64/bin;C:/msys64/usr/bin;' + env['PATH']
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
    entry_rc = rc
    entry_log = (evidence / 'production-entry.log').read_text(errors='replace')
    for generated in (build / 'windows_x86_64_exports.raw.def', build / 'runtime-link-inputs.txt'):
        if generated.is_file():
            shutil.copy2(generated, evidence / generated.name)
    record['production_entry_assertion_rc'] = int(entry_rc != 0)
    record['publication_assertion_rc'] = int('RUNTIME_OUTPUT_PUBLISHED config=' not in entry_log)
    print(f'POST_LINK_PUBLICATION_ASSERT arm={args.arm} rc={record["publication_assertion_rc"]}', flush=True)
    save()
    if args.arm.startswith('cut-'):
        log = (evidence / 'production-entry.log').read_text(errors='replace')
        products = list((build / 'runtime-staging').rglob('libcangjie-runtime.dll'))
        record['linked_products'] = {str(p): sha(p) for p in products}
        keep = evidence / 'keep'
        keep.mkdir(exist_ok=True)
        for product in products:
            shutil.copy2(product, keep / product.name)
        exact = rc != 0 and 'RUNTIME_OUTPUT_PUBLISH_FAIL:' in log and 'link.txt' in log and bool(products)
        record['exact_cut'] = exact
        save()
        print(f'MISSING_NATIVE_LINK_INPUT_ASSERT arm={args.arm} exact={exact}', flush=True)
        assert exact, 'cut did not reach the post-link missing-link-input assertion'
        return
    assert record['publication_assertion_rc'] == 0, 'production post-link publication failed'

    def publication(name):
        cache = (build / 'CMakeCache.txt').read_text()
        identity = re.search(r'^CANGJIE_RUNTIME_CONFIG_ID:INTERNAL=(.*)$', cache, re.M)[1]
        root = source / 'output/temp' / identity
        inputs = json.loads((root / 'runtime-build-inputs.txt').read_text())
        for filename in ('runtime-build-config.txt', 'runtime-build-inputs.txt', 'runtime-product-hashes.json'):
            shutil.copy2(root / filename, evidence / (name + '-' + filename))
        products = {p.name: sha(p) for p in root.rglob('*') if p.name in inputs['products']}
        assert products == inputs['products'], 'published product bytes differ from recorded identity'
        keep = evidence / 'keep' / name
        keep.mkdir(parents=True, exist_ok=True)
        for product in root.rglob('*'):
            if product.name in products:
                shutil.copy2(product, keep / product.name)
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
    publisher_command = ['python3', consumer, '@' + str(build / 'runtime-publish-args.txt')]
    # Change a genuine linker flag, force the real linker/post-build entry, and
    # inspect the product's recorded input rather than calling a model helper.
    option = '-Wl,--nxcompat' if os.name == 'nt' else '-Wl,--as-needed'
    cache = (build / 'CMakeCache.txt').read_text()
    cmake = re.search(r'^CMAKE_COMMAND:INTERNAL=(.*)$', cache, re.M)[1]
    old_flags = re.search(r'^CMAKE_SHARED_LINKER_FLAGS:STRING=(.*)$', cache, re.M)[1]
    try:
        # Configure the real flag: editing a generated recipe directly is not
        # durable across CMake's automatic regeneration after cache updates.
        assert run('changed-link-configure', [cmake, '-S', source, '-B', build,
                   '-DCMAKE_SHARED_LINKER_FLAGS=' + old_flags + ' ' + option]) == 0
        changed_build_rc = run('changed-link-build', [cmake, '--build', build, '--target', 'cangjie-runtime', '--parallel', os.cpu_count()])
        assert 'RUNTIME_OUTPUT_PUBLISHED config=' in (evidence / 'changed-link-build.log').read_text(errors='replace'), 'changed product did not publish'
        changed_id, changed_inputs = publication('changed')
        print(f'LINK_IDENTITY_ASSERT before={original_id} after={changed_id}', flush=True)
        assert original_id != changed_id, 'actual linker input change did not change identity'
        assert any(option in value for value in changed_inputs['links'].values()), 'changed link command was not recorded'
    finally:
        assert run('restored-link-configure', [cmake, '-S', source, '-B', build,
                   '-DCMAKE_SHARED_LINKER_FLAGS=' + old_flags]) == 0
    restored_build_rc = run('restored-link-build', [cmake, '--build', build, '--target', 'cangjie-runtime', '--parallel', os.cpu_count()])
    assert 'RUNTIME_OUTPUT_PUBLISHED config=' in (evidence / 'restored-link-build.log').read_text(errors='replace'), 'restored product did not publish'
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
    restored_id, restored_inputs = publication('restored')
    assert restored_id == original_id and restored_inputs['products'] == original_inputs['products'], 'restoring link inputs did not restore product identity'
    record['controls_passed'] = True
    save()
    print('PUBLISHER_CONTROLS_PASSED', flush=True)
    # Preserve the complete entry assertion independently of publication. In
    # particular, a later export-check failure must not be presented as a
    # successful build, even when publisher controls have completed.
    assert entry_rc == changed_build_rc == restored_build_rc == 0, 'complete production entry failed after publication; see recorded build rc'


if __name__ == '__main__':
    main()
