#!/usr/bin/env python3
"""Exercise the real cut runner with one built product and one unchanged test ELF.

Run only in an isolated checkout on the build host. CMakebuild must already be
configured with Unix Makefiles, MRT_GC_UNIT_TESTS and MRT_TESTABLE_INTERNALS.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    root, evidence = args.runtime.resolve(), args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=False)
    (evidence / 'T2').mkdir()
    elf = evidence / 'T2/cj_gc_unit'
    shutil.copy2(args.elf, elf)
    runner = root / 'tests/gc_unit/run_young_weak_product_cut.sh'
    tests = re.findall(r'^  (YoungWeakClosure\.\w+)$', runner.read_text(), re.M)
    target = 'YoungWeakClosure.SerialDiscoversWithoutStrongReferentClosure'
    assert target in tests and len(tests) == len(set(tests))
    publish_args = (root / 'CMakebuild/runtime-publish-args.txt').read_text().splitlines()
    sources = [Path(publish_args[publish_args.index(k) + 1]).resolve()
               for k in ('--runtime', '--boundscheck')]
    baseline = evidence / 'baseline'
    baseline.mkdir()
    for source in sources:
        shutil.copy2(source, baseline / source.name)
    product_source = root / 'src/Heap/Collector/Mark.cpp'
    record = dict(elf_sha256=sha(elf), source_before=sha(product_source),
                  baseline_sha256=[sha(baseline / s.name) for s in sources])
    import os
    with (evidence / 'baseline.grid.log').open('w') as log:
        record['baseline_rc'] = {}
        for test in tests:
            rc = subprocess.run([str(elf), '--gtest_filter=' + test],
                                env=dict(os.environ, LD_LIBRARY_PATH=str(baseline)),
                                stdout=log, stderr=subprocess.STDOUT).returncode
            record['baseline_rc'][test] = rc
    with (evidence / 'runner.log').open('w') as log:
        record['runner_rc'] = subprocess.run(
            ['bash', str(runner), str(root), str(evidence), 'serial', 'WCollector',
             str(root / 'tests/gc_unit/young_weak_serial_negative.patch')],
            cwd=root, stdout=log, stderr=subprocess.STDOUT).returncode
    record['source_after'] = sha(product_source)
    record['elf_after'] = sha(elf)
    assert record['runner_rc'] == 0, 'cut runner did not complete; see runner.log'
    cut_dir = evidence / 'serial'
    for arm, filename in [('cut', 'grid.log'), ('restored', 'restored.grid.log')]:
        text = (cut_dir / filename).read_text()
        record[arm + '_rc'] = {name: int(rc) for name, rc in
                              re.findall(r'^GRID_RC (\S+) (\d+)$', text, re.M)}
    record['cut_sha256'] = [sha(cut_dir / 'product' / s.name) for s in sources]
    record['restored_sha256'] = [sha(cut_dir / 'restored_product' / s.name) for s in sources]
    (evidence / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    assert record['baseline_rc'] == {test: 0 for test in tests}, 'baseline must execute successfully'
    assert record['source_before'] == record['source_after'], 'source restoration failed'
    assert record['elf_sha256'] == record['elf_after'], 'the test ELF changed between arms'
    assert record['restored_rc'] == record['baseline_rc'], 'restoration changed the passing set'
    print('RESTORED_CONTROL_PASS', flush=True)
    print('ASSERT_CUT_RESULT target=' + target, flush=True)
    assert record['cut_rc'] == {test: int(test == target) for test in tests}, \
        'cut must change exactly the serial target result; stale SO selection leaves it passing'
    assert record['cut_sha256'][0] != record['baseline_sha256'][0], 'cut runtime identity did not change'
    assert record['restored_sha256'] == record['baseline_sha256'], 'restored product bytes differ'
    assert record['cut_sha256'][1] == record['baseline_sha256'][1], 'boundscheck changed'
    for arm in ('product', 'restored_product'):
        identities = json.loads((cut_dir / arm / 'linked-product.json').read_text())
        assert [Path(item['source']) for item in identities] == sources, 'not the generated linker outputs'
        assert [item['sha256'] for item in identities] == [sha(cut_dir / arm / s.name) for s in sources]
    print('CUT_IDENTITY_PASS: exact serial failure; restored results and identities match', flush=True)


if __name__ == '__main__':
    main()
