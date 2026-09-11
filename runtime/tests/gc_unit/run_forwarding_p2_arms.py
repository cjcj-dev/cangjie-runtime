#!/usr/bin/env python3
"""P2 product page ownership cuts, with a fixed dynamically linked test ELF."""
import argparse
import concurrent.futures
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

PRIMARY = [
    'YoungConc.ForwardingPageClaimedByProductTask',
    'YoungConc.ForwardingReaderExitsBeforeInPlaceReuse',
    'YoungConc.ReleasedForwardingFindReturnsProductCopy',
    'YoungConc.ForwardingDoneFollowsPageWork',
    'YoungConc.ForwardingPartialReaderExitsBeforeInPlaceReuse',
]
CONTROLS = ['ColourAddress.UncolorRoundTripAllRemapOneHot',
            'RelocationPageQueue.TwoObjectsShareOnePageClaim',
            'ForwardingPublicationProduct.PartialCompactFirstDestinationKeepsReceipt']
CUTS = {
    'entry': ('runtime/src/Heap/WCollector/WCollector.cpp',
              '        DoYoungGarbageCollection();', '        (void)0;', 1),
    'claim': ('runtime/src/Heap/Allocator/RegionManager.cpp',
              '    if (!owner || (!claimed && !owner->claim())) return;',
              '    if (!owner) return;', 1),
    'reader': ('runtime/src/Heap/Allocator/RegionManager.cpp',
               '        owner->in_place_relocation_claim_page();',
               '        (void)owner;', 2),
    'find': ('runtime/src/Heap/Collector/Relocate.cpp',
             '    return reinterpret_cast<BaseObject*>(owner->resolve_life(owner->find(from)));',
             '    return obj;', 1),
    'done': ('runtime/src/Heap/Allocator/RegionManager.cpp',
             '    ForwardRegion<G>(region);\n#if defined(MRT_TESTABLE_INTERNALS)',
             '    ForwardRegion<G>(region);\n    owner->mark_done();\n#if defined(MRT_TESTABLE_INTERNALS)', 1),
}
CUTS['reader_partial'] = ('runtime/src/Heap/Allocator/RegionManager.cpp',
    'void RegionManager::CompactRegion(RegionInfo* region, RegionInfo* toRegion1)\n{\n    auto owner = ForwardingTable::RetainPageOwner(region);\n    ZForwardingLife::PageWorkScope work(owner.get(),\n        owner && ZForwardingLife::CurrentPageWork() != owner.get());\n    if (owner && owner->ref_count().load(std::memory_order_acquire) > 0) {\n        owner->in_place_relocation_claim_page();',
    'void RegionManager::CompactRegion(RegionInfo* region, RegionInfo* toRegion1)\n{\n    auto owner = ForwardingTable::RetainPageOwner(region);\n    ZForwardingLife::PageWorkScope work(owner.get(),\n        owner && ZForwardingLife::CurrentPageWork() != owner.get());\n    if (owner && owner->ref_count().load(std::memory_order_acquire) > 0) {\n        (void)owner;', 1)
CUTS['derived'] = (
    'runtime/src/Mutator/Mutator.cpp',
    '            Collector::FailClosedLoad(\n'
    '                "Mutator::MakePreForwardDerivedVisitor.base-not-remapped", oldBase,\n'
    '                reinterpret_cast<uintptr_t>(&derivedPtr),\n'
    '                ForwardingProvenance{ ForwardingHolderKind::Derived,\n'
    '                                      reinterpret_cast<const void*>(raw(basePtr)), &derivedPtr });',
    '            return;',
    1)
DERIVED = 'ForwardingPublicationProduct.PreForwardDerivedTaggedUnresolvedGhostFailsClosed'
EXPECTED = {'normal': [], 'restored': [], 'entry': PRIMARY,
            'claim': PRIMARY[:1], 'reader': PRIMARY[1:2], 'find': PRIMARY[2:3], 'done': PRIMARY[3:4],
            'reader_partial': PRIMARY[4:5], 'derived': []}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(cmd, cwd, env, log):
    with log.open('w') as f:
        return subprocess.run(cmd, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT).returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--cores', default='32-63')
    ap.add_argument('--samples', type=int, default=3)
    ap.add_argument('--build-jobs', type=int, default=3)
    ap.add_argument('--arms', default='normal,entry,claim,reader,reader_partial,find,done,derived,restored,default')
    args = ap.parse_args()
    repo, out = args.repo.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    head = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
    env = dict(os.environ, GC_UNIT_GATE_SKIP='1', CCACHE_DISABLE='1')
    for key in ('CCACHE_BASEDIR', 'CCACHE_NOHASHDIR', 'CCACHE_SLOPPINESS'):
        env.pop(key, None)
    manifest = dict(head=head, cores=args.cores, samples=args.samples, arms={})
    affinity = ['taskset', '-c', args.cores]

    def build(arm):
        d = out/arm
        d.mkdir()
        src, bld = d/'src', d/'build'
        subprocess.run(['git', 'clone', '-q', '--shared', str(repo), str(src)], check=True)
        subprocess.run(['git', '-C', str(src), 'checkout', '-q', head], check=True)
        # Reuse pinned dependency source, never a shared installation or object cache.
        dependency = repo/'runtime/third_party/third_party_bounds_checking_function'
        if dependency.exists():
            shutil.copytree(dependency, src/'runtime/third_party/third_party_bounds_checking_function', dirs_exist_ok=True)
        rec = dict(head=head, arm=arm, source_head=head, cores=args.cores)
        rec['uptime_before'] = subprocess.check_output(['uptime'], text=True)
        original = None
        if arm in CUTS:
            file, before, after, occurrences = CUTS[arm]
            path = src/file
            original = path.read_text()
            if original.count(before) != occurrences:
                raise RuntimeError(f'{arm}: anchor count {original.count(before)} != {occurrences}')
            changed = original.replace(before, after, 1)
            path.write_text(changed)
            rec.update(cut_file=file, source_before_sha256=hashlib.sha256(original.encode()).hexdigest(),
                       source_cut_sha256=sha(path), source_cut_mtime=path.stat().st_mtime)
            (d/'cut.diff').write_text(''.join(difflib.unified_diff(original.splitlines(True), changed.splitlines(True),
                                      fromfile='a/'+file, tofile='b/'+file)))
        else:
            (d/'cut.diff').write_text('')
        flags = f'-ffile-prefix-map={src}={out}/canonical-src -ffile-prefix-map={bld}={out}/canonical-build'
        macro = 'OFF' if arm == 'default' else 'ON'
        configure = ['cmake', '-S', '.', '-B', str(bld), '-DCMAKE_BUILD_TYPE=Release',
                     '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++', '-DDISABLE_VERSION_CHECK=1',
                     '-DCJ_SDK_VERSION=0.0.2', '-DMRT_GC_UNIT_TESTS='+macro, '-DMRT_TESTABLE_INTERNALS='+macro,
                     '-DCANGJIE_DISABLE_COMPILER_CACHE=ON', '-DCMAKE_C_COMPILER_LAUNCHER=',
                     '-DCMAKE_CXX_COMPILER_LAUNCHER=', '-DCMAKE_C_FLAGS='+flags,
                     '-DCMAKE_CXX_FLAGS='+flags, '-DCMAKE_ASM_FLAGS='+flags]
        rec['configure_command'] = configure
        rec['configure_rc'] = run(affinity+configure, src/'runtime', env, d/'configure.log')
        if rec['configure_rc']:
            (d/'build-record.json').write_text(json.dumps(rec, indent=2))
            return rec
        files = subprocess.check_output(['git', '-C', str(src), 'ls-files'], text=True).splitlines()
        rec['last_source_mtime'] = max((src/f).stat().st_mtime for f in files if (src/f).is_file())
        rec['build_started'] = time.time()
        target = ['--target', 'cangjie-runtime'] if arm in CUTS or arm == 'default' else []
        cmd = affinity+['cmake', '--build', str(bld), '-j64']+target
        rec['build_command'] = cmd
        rec['build_rc'] = run(cmd, src/'runtime', env, d/'build.log')
        rec['build_finished'] = time.time()
        if not rec['build_rc']:
            ret = d/'retained'; ret.mkdir()
            lib = bld/'runtime-staging/lib/x86_64_Release'
            for name in ('libcangjie-runtime.so', 'libboundscheck.so'):
                shutil.copy2(lib/name, ret/name)
            if not target:
                for name in ('cj_gc_unit', 'cj_gc_clear_entries_unit'):
                    shutil.copy2(bld/'runtime-staging/bin/x86_64_Release'/name, ret/name)
            rec['sha256'] = {p.name: sha(p) for p in ret.iterdir()}
            with (d/'runtime-defined-symbols.txt').open('w') as f:
                subprocess.run(['nm', '-C', '--defined-only', str(ret/'libcangjie-runtime.so')], stdout=f, check=True)
        if original is not None:
            path.write_text(original)
            rec['source_restored_sha256'] = sha(path)
            rec['source_final_status'] = subprocess.check_output(['git', '-C', str(src), 'status', '--porcelain', '--', file], text=True)
        rec['uptime_after'] = subprocess.check_output(['uptime'], text=True)
        (d/'build-record.json').write_text(json.dumps(rec, indent=2))
        return rec

    arms = args.arms.split(',')
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.build_jobs) as pool:
        for rec in pool.map(build, arms):
            manifest['arms'][rec['arm']] = rec
            (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
    if any(rec.get('build_rc', 1) for rec in manifest['arms'].values()):
        raise SystemExit(2)
    normal = out/'normal/retained'
    expected_red = {arm: set(EXPECTED.get(arm, [])) for arm in arms}
    results = []
    errors = []
    for arm in arms:
        if arm == 'default': continue
        ret = out/arm/'retained'
        for elf in ('cj_gc_unit', 'cj_gc_clear_entries_unit'):
            if not (ret/elf).exists(): shutil.copy2(normal/elf, ret/elf)
        if sha(ret/'libboundscheck.so') != sha(normal/'libboundscheck.so'):
            errors.append(f'{arm}: boundscheck changed')
        if any(sha(ret/e) != sha(normal/e) for e in ('cj_gc_unit', 'cj_gc_clear_entries_unit')):
            errors.append(f'{arm}: test ELF changed')
        logs = out/arm/'runs'; logs.mkdir()
        arm_env = dict(env, LD_LIBRARY_PATH=str(ret), CJ_GC_UNIT_REMAP_WINDOW='1')
        for sample in range(args.samples):
            red = set()
            for test in PRIMARY+CONTROLS:
                elf = 'cj_gc_clear_entries_unit' if test.startswith('ForwardingPublicationProduct.') else 'cj_gc_unit'
                log = logs/f'{sample}-{test}.log'
                cmd = affinity+['timeout', '35', str(ret/elf), '--gtest_filter='+test]
                rc = run(cmd, repo, arm_env, log)
                output = log.read_text(errors='replace')
                if rc: red.add(test)
                target_seen = test not in PRIMARY or 'P2_PAGE target_assertion executed=1' in output
                result = dict(arm=arm, sample=sample, test=test, rc=rc, target_seen=target_seen,
                              command=cmd, log=str(log), elf_sha256=sha(ret/elf),
                              runtime_sha256=sha(ret/'libcangjie-runtime.so'), boundscheck_sha256=sha(ret/'libboundscheck.so'))
                results.append(result)
                if not target_seen or rc not in (0, 1): errors.append(f'{arm}/{sample}/{test}: rc={rc} target={target_seen}')
            if red != expected_red[arm]: errors.append(f'{arm}/{sample}: red={sorted(red)} expected={sorted(expected_red[arm])}')
    # The multi-worker reader scenario is its own equal-sized three-arm group.
    parallel = 'YoungConc.ForwardingReaderExitsBeforeInPlaceReuseParallel'
    for arm in ('normal', 'reader', 'restored'):
        if arm not in arms: continue
        ret = out/arm/'retained'
        arm_env = dict(env, LD_LIBRARY_PATH=str(ret), CJ_GC_UNIT_REMAP_WINDOW='1')
        for sample in range(args.samples):
            for test in (parallel, CONTROLS[0]):
                log = out/arm/'runs'/f'parallel-{sample}-{test}.log'
                cmd = affinity+['timeout', '35', str(ret/'cj_gc_unit'), '--gtest_filter='+test]
                rc = run(cmd, repo, arm_env, log)
                target_seen = test != parallel or 'P2_PAGE target_assertion executed=1' in log.read_text(errors='replace')
                expected_rc = 1 if arm == 'reader' and test == parallel else 0
                results.append(dict(group='parallel-reader', arm=arm, sample=sample, test=test, rc=rc,
                    target_seen=target_seen, command=cmd, log=str(log),
                    elf_sha256=sha(ret/'cj_gc_unit'), runtime_sha256=sha(ret/'libcangjie-runtime.so'),
                    boundscheck_sha256=sha(ret/'libboundscheck.so')))
                if rc != expected_rc or not target_seen:
                    errors.append(f'parallel/{arm}/{sample}/{test}: rc={rc} expected={expected_rc} target={target_seen}')
    derived_arms = [a for a in ('normal', 'derived', 'restored') if a in arms]
    if len(derived_arms) == 3:
        for arm in derived_arms:
            ret = out/arm/'retained'
            arm_env = dict(env, LD_LIBRARY_PATH=str(ret), CJ_GC_UNIT_REMAP_WINDOW='1')
            for sample in range(args.samples):
                log = out/arm/'runs'/f'derived-{sample}-{DERIVED}.log'
                elf = ret/'cj_gc_clear_entries_unit'
                cmd = affinity+['timeout', '35', str(elf), '--gtest_filter='+DERIVED]
                rc = run(cmd, repo, arm_env, log)
                text = log.read_text(errors='replace')
                target_seen = 'DERIVED_BASE_TARGET target_assertion executed=1' in text
                matched = 'matched=1' in text
                expected_rc = 1 if arm == 'derived' else 0
                expected_matched = 0 if arm == 'derived' else 1
                results.append(dict(group='derived', arm=arm, sample=sample, test=DERIVED, rc=rc,
                    target_seen=target_seen, matched=int(matched), command=cmd, log=str(log),
                    elf_sha256=sha(elf), runtime_sha256=sha(ret/'libcangjie-runtime.so'),
                    boundscheck_sha256=sha(ret/'libboundscheck.so')))
                if rc != expected_rc or not target_seen or int(matched) != expected_matched:
                    errors.append(f'derived/{arm}/{sample}: rc={rc} expected={expected_rc} target={target_seen} matched={int(matched)}')
        if sha(out/'derived/retained/libcangjie-runtime.so') == sha(normal/'libcangjie-runtime.so'):
            errors.append('derived: cut runtime did not change')
        if sha(out/'restored/retained/libcangjie-runtime.so') != sha(normal/'libcangjie-runtime.so'):
            errors.append('derived: restored runtime differs from normal')
    if 'restored' in arms and sha(out/'restored/retained/libcangjie-runtime.so') != sha(normal/'libcangjie-runtime.so'):
        errors.append('normal/restored runtime differs')
    for arm in CUTS.keys() & set(arms):
        if sha(out/arm/'retained/libcangjie-runtime.so') == sha(normal/'libcangjie-runtime.so'):
            errors.append(f'{arm}: cut runtime did not change')
    manifest.update(results=results, errors=errors)
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2))
    raise SystemExit(1 if errors else 0)


if __name__ == '__main__':
    main()
