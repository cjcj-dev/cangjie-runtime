#!/usr/bin/env python3
"""Build isolated P1 controls on kkk2; preserve and hash the loaded artifacts."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

TESTS = [
    'YoungConc.ForwardingArenaProductInstall',
    'YoungConc.ForwardingPublishedTargetInitialized',
    'ReceiptLifeRegistry.ExistingKeyReturnsFirstWinner',
    'ReceiptLifeRegistry.ConcurrentSameKeyReturnsLockedWinner',
    'ZForwardingEntries.CapacityArithmeticDoesNotWrap',
    'ZForwardingEntries.ArenaRetainsLastCarrier',
    'ZForwardingEntries.ArenaBudgetFailureDoesNotConsumeStorage',
    'ZForwardingEntries.ConcurrentSameKeyReturnsInitializedWinner',
    'ZForwardingEntries.CollisionPreservesIdentityAndOtherKey',
    'ColourAddress.UncolorRoundTripAllRemapOneHot',
]
SECONDARY = 'FindToPublicState.NotManagedIsObservable'
PREFIX = 'runtime/src/'
CUTS = {
    'entry': (PREFIX+'Heap/WCollector/WCollector.cpp',
              '        DoYoungGarbageCollection();', '        (void)0;'),
    'arena_begin': (PREFIX+'Heap/Allocator/RegionManager.h',
        '        CHECK_DETAIL(ForwardingTable::BeginForwardingArena(fromRegionList),\n'
        '                     "forwarding arena budget allocation failed");',
        '        (void)fromRegionList;'),
    'arena_storage': (PREFIX+'Heap/Collector/ZForwarding.h',
        'void* const addr = arena ? arena->allocate(size) : AttachedArray::alloc(n);',
        'void* const addr = AttachedArray::alloc(n);'),
    'copy': (PREFIX+'Heap/Collector/Relocate.cpp',
        '    CopyObject(*obj, *toObj, size);',
        '    CopyObject(*obj, *toObj, TYPEINFO_PTR_SIZE);'),
    'existing_winner': (PREFIX+'Heap/Allocator/ForwardingTable.cpp',
        'return Receipt{ existingBeforeLock, false, Receipt::Status::EXISTING };',
        'return Receipt{ to, false, Receipt::Status::EXISTING };'),
    'locked_winner': (PREFIX+'Heap/Allocator/ForwardingTable.cpp',
        '    const MAddress existing = find(from);\n    if (existing != 0) {\n'
        '        return Receipt{ existing, false, Receipt::Status::EXISTING };',
        '    const MAddress existing = find(from);\n    if (existing != 0) {\n'
        '        return Receipt{ to, false, Receipt::Status::EXISTING };'),
    'header_capacity': (PREFIX+'Heap/Collector/ZForwarding.h',
        '        return capacity;\n    }', '        return static_cast<uint32_t>(capacity);\n    }'),
    'header_winner': (PREFIX+'Heap/Collector/ZForwarding.h',
        '                    return entry.to_offset();', '                    return toOffset;'),
    'header_retention': (PREFIX+'Heap/Collector/ZForwarding.h',
        '        auto arena = _arena;',
        '        static auto retainedArena = _arena;\n        auto arena = _arena;'),
}
EXPECTED = {
    'normal': [], 'restored': [],
    'entry': TESTS[:2], 'arena_begin': TESTS[:1], 'arena_storage': TESTS[:1],
    'copy': TESTS[1:2], 'existing_winner': TESTS[2:3], 'locked_winner': TESTS[3:4],
    'header_capacity': TESTS[4:5], 'header_winner': TESTS[7:8], 'header_retention': TESTS[5:6],
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def call(cmd, cwd, env, log):
    with log.open('w') as f:
        return subprocess.run(cmd, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT).returncode


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cores', default='128-143,144-159')
    p.add_argument('--samples', type=int, default=3)
    p.add_argument('--arms', default=','.join(EXPECTED))
    p.add_argument('--baseline', type=Path)
    args = p.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    head = subprocess.check_output(['git', '-C', str(args.repo), 'rev-parse', 'HEAD'], text=True).strip()
    arms = args.arms.split(',')
    baseline = args.baseline.resolve() if args.baseline else out/'normal/retained'
    canonical = baseline.parent.parent
    cores = args.cores.split(',')
    env = dict(os.environ, GC_UNIT_GATE_SKIP='1', CCACHE_DISABLE='1')
    for key in ('CCACHE_BASEDIR', 'CCACHE_NOHASHDIR', 'CCACHE_SLOPPINESS'):
        env.pop(key, None)
    manifest = {'head': head, 'samples': args.samples, 'arms': {}, 'tests': TESTS + [SECONDARY]}

    def build(item):
        index, arm = item
        d = out / arm
        d.mkdir()
        src, build_dir = d/'src', d/'build'
        subprocess.run(['git', 'clone', '-q', '--shared', str(args.repo), str(src)], check=True)
        subprocess.run(['git', '-C', str(src), 'checkout', '-q', head], check=True)
        original = None
        rec = {'head': head, 'cores': cores[index % len(cores)], 'build_started': time.time()}
        rec['uptime_before_build'] = subprocess.check_output(['uptime'], text=True)
        if arm in CUTS:
            file, before, after = CUTS[arm]
            path = src/file
            original = path.read_text()
            if original.count(before) != 1:
                raise RuntimeError(f'{arm}: anchor count {original.count(before)}')
            patched = original.replace(before, after, 1)
            path.write_text(patched)
            rec['source_before_sha256'] = hashlib.sha256(original.encode()).hexdigest()
            rec['source_cut_sha256'] = sha(path)
            rec['source_mtime'] = path.stat().st_mtime
            (d/'cut.diff').write_text(''.join(difflib.unified_diff(original.splitlines(True),
                patched.splitlines(True), fromfile='a/'+file, tofile='b/'+file)))
        else:
            (d/'cut.diff').write_text('')
        # Canonicalize both source and generated-file paths. Independent build
        # directories must not turn path strings into a third experimental input.
        flags = f'-ffile-prefix-map={src}={canonical}/canonical-src -ffile-prefix-map={build_dir}={canonical}/canonical-build'
        cmd = ['cmake', '-S', '.', '-B', str(build_dir), '-DCMAKE_BUILD_TYPE=Release',
               '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++', '-DDISABLE_VERSION_CHECK=1',
               '-DCJ_SDK_VERSION=0.0.2', '-DMRT_GC_UNIT_TESTS=ON', '-DMRT_TESTABLE_INTERNALS=ON',
               '-DCMAKE_C_COMPILER_LAUNCHER=', '-DCMAKE_CXX_COMPILER_LAUNCHER=',
               '-DCMAKE_C_FLAGS='+flags, '-DCMAKE_CXX_FLAGS='+flags]
        rec['configure_command'] = cmd
        affinity = ['taskset', '-c', rec['cores']]
        rec['configure_rc'] = call(affinity+cmd, src/'runtime', env, d/'configure.log')
        if rec['configure_rc']:
            raise RuntimeError(f'{arm}: configure failed')
        target = ['--target', 'cangjie-runtime'] if arm in CUTS and not arm.startswith('header_') else []
        rec['build_rc'] = call(affinity+['cmake', '--build', str(build_dir), '-j64']+target,
                               src/'runtime', env, d/'build.log')
        rec['build_finished'] = time.time()
        rec['uptime_after_build'] = subprocess.check_output(['uptime'], text=True)
        (d/'build-record.json').write_text(json.dumps(rec, indent=2))
        if rec['build_rc']:
            raise RuntimeError(f'{arm}: build failed')
        retained = d/'retained'
        retained.mkdir()
        lib = build_dir/'runtime-staging/lib/x86_64_Release'
        for name in ('libcangjie-runtime.so', 'libboundscheck.so'):
            shutil.copy2(lib/name, retained/name)
        if not target:
            for name in ('cj_gc_unit', 'cj_gc_clear_entries_unit'):
                shutil.copy2(build_dir/'runtime-staging/bin/x86_64_Release'/name, retained/name)
        rec['built_sha256'] = {f.name: sha(f) for f in retained.iterdir()}
        if original is not None:
            path.write_text(original)
            rec['source_restored_sha256'] = sha(path)
            rec['restored_status'] = subprocess.check_output(
                ['git', '-C', str(src), 'status', '--porcelain'], text=True)
        (d/'build-record.json').write_text(json.dumps(rec, indent=2))
        print('BUILT', arm, rec['built_sha256'], flush=True)
        return arm, rec

    # Every build has its own source/objects; no concurrent source mutation.
    with ThreadPoolExecutor(max_workers=len(cores)) as pool:
        for arm, rec in pool.map(build, enumerate(arms)):
            manifest['arms'][arm] = rec
            (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
    if 'restored' in arms:
        for name in ('libcangjie-runtime.so', 'libboundscheck.so'):
            if sha(baseline/name) != sha(out/'restored/retained'/name):
                raise RuntimeError(f'normal/restored path noise: {name}')
    for arm in arms:
        rec = manifest['arms'][arm]
        retained = out/arm/'retained'
        if arm.startswith('header_'):
            # The cut is the one header truth instantiated in the test ELF.
            for name in ('libcangjie-runtime.so', 'libboundscheck.so'):
                shutil.copy2(baseline/name, retained/name)
        elif arm != 'normal':
            shutil.copy2(baseline/'cj_gc_unit', retained/'cj_gc_unit')
        shutil.copy2(baseline/'cj_gc_clear_entries_unit', retained/'cj_gc_clear_entries_unit') if arm != 'normal' else None
        rec['loaded_sha256'] = {f.name: sha(f) for f in retained.iterdir()}
        rec['lineage'] = [line for line in subprocess.check_output(
            ['strings', str(retained/'libcangjie-runtime.so')], text=True).splitlines()
            if line.startswith('CJRT-COMMIT:') or line.startswith('CJRT-DECLARED:')]
        run_env = dict(os.environ, LD_LIBRARY_PATH=str(retained), LD_DEBUG='libs')
        rec['runs'] = []
        rec['uptime_before_runs'] = subprocess.check_output(['uptime'], text=True)
        for sample in range(args.samples):
            for test in TESTS + [SECONDARY]:
                elf = 'cj_gc_clear_entries_unit' if test == SECONDARY else 'cj_gc_unit'
                log = out/arm/f'{sample}-{test}.log'
                command = ['taskset', '-c', cores[0], str(retained/elf), '--gtest_filter='+test]
                start = time.time()
                rc = call(command, out, run_env, log)
                txt = log.read_text()
                entered = '[  RUN   ] '+test in txt
                loaded = all('calling init: '+str(retained/name) in txt for name in
                             ('libcangjie-runtime.so', 'libboundscheck.so'))
                row = {'test': test, 'sample': sample, 'rc': rc, 'entered': entered, 'loaded_paths_observed': loaded,
                       'command': command, 'log': str(log), 'wall': time.time()-start}
                rec['runs'].append(row)
                (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
                if not entered or not loaded or ((rc != 0) != (test in EXPECTED[arm])):
                    raise RuntimeError(f'{arm}: unexpected result {row}')
        rec['uptime_after_runs'] = subprocess.check_output(['uptime'], text=True)
        print('RUNS', arm, 'expected_failures', EXPECTED[arm], flush=True)
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
