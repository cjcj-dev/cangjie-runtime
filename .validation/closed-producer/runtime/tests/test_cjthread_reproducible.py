#!/usr/bin/env python3
"""Exercise CJThread reproducibility through the real parent runtime build.

Run on a build host with --source pointing at runtime and a fresh --work.
The seed/repeat/hit ordering is intentional: the hit must consume the seed's
cache entries. Independent cold, cut and restored arms build concurrently.
Only the two cut copies omit the production toolchain's prefix maps.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import time


def sha(p):
    h = hashlib.sha256()
    with p.open('rb') as f:
        for data in iter(lambda: f.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()


def output(cmd, env=None):
    return subprocess.check_output(cmd, text=True, env=env).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compare', nargs=2, type=Path)
    parser.add_argument('--artifact', choices=['thread', 'runtime'], default='thread')
    parser.add_argument('--check-launchers', type=Path)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--work', type=Path)
    parser.add_argument('--commit')
    parser.add_argument('--build-type', choices=['Debug', 'Release'], default='Debug')
    parser.add_argument('--configure-only', action='store_true')
    parser.add_argument('--cache-control', choices=['recache', 'disabled'], default='recache')
    args = parser.parse_args()
    if args.compare:
        a, b = [json.loads(p.read_text())[args.artifact] for p in args.compare]
        ok = bool(a) and a == b
        print(f'CJTHREAD_IDENTITY kind={args.artifact} left={a} right={b} result={"PASS" if ok else "FAIL"}')
        return 0 if ok else 1
    if args.check_launchers:
        cache = json.loads(args.check_launchers.read_text())['child_cache']
        ok = all(re.search(r'^CMAKE_' + lang + r'_COMPILER_LAUNCHER:[^=]+=.+$', cache, re.M) for lang in ['C', 'CXX', 'ASM'])
        print(f'CJTHREAD_LAUNCHERS result={"PASS" if ok else "FAIL"}')
        return 0 if ok else 1
    if not all([args.work, args.source, args.commit]):
        parser.error('--source, --work and --commit are required for builds')
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    source = args.source.resolve()
    toolchain = Path('build/cmake/toolchain/linux_x86_64_cangjie.cmake')
    original = (source / toolchain).read_text()
    start_marker = 'foreach(_language C CXX ASM)'
    cut = original[:original.index(start_marker)]
    (work / 'cut-original.cmake').write_text(original)
    (work / 'cut-modified.cmake').write_text(cut)
    names = ['seed', 'hit', 'cold', 'restored', 'launcher_cut']
    if args.build_type == 'Debug':
        names += ['cut_a', 'cut_b']
    for name in names:
        dst = work / name / 'runtime'
        shutil.copytree(source, dst, ignore=shutil.ignore_patterns('output', 'build-*', '__pycache__'))
        if name.startswith('cut_'):
            (dst / toolchain).write_text(cut)
        if name == 'launcher_cut':
            parent = dst / 'CMakeLists.txt'
            text = parent.read_text()
            for lang in ['C', 'CXX', 'ASM']:
                text = text.replace('CJTHREAD_' + lang + '_LAUNCHER=${CMAKE_' + lang + '_COMPILER_LAUNCHER}', 'CJTHREAD_' + lang + '_LAUNCHER=')
            parent.write_text(text)
    def build(name, repeat=False):
        root = work / name
        label = 'repeat' if repeat else name
        builddir = root / 'build'
        env = dict(os.environ, GC_UNIT_GATE_SKIP='1', CCACHE_DIR='/root/.ccache',
                   CCACHE_BASEDIR=str(root), CCACHE_NOHASHDIR='1',
                   CCACHE_LOGFILE=str(work / (label + '-ccache.log')),
                   CCACHE_STATSLOG=str(work / (label + '-ccache-stats.log')))
        # Prove launcher handoff without relying on compiler-name PATH shims.
        env['PATH'] = os.pathsep.join(p for p in env['PATH'].split(os.pathsep) if 'ccache' not in p)
        env.pop('CCACHE_DISABLE', None)
        env.pop('CCACHE_RECACHE', None)
        if name in ['cold', 'cut_a', 'cut_b', 'restored', 'launcher_cut']:
            env['CCACHE_RECACHE' if args.cache_control == 'recache' else 'CCACHE_DISABLE'] = '1'
        elif name == 'seed' and not repeat:
            env['CCACHE_RECACHE'] = '1'
        maps = ' '.join(f'-f{k}-prefix-map={root}=/usr/src/cangjie-runtime' for k in ['file', 'debug', 'macro'])
        env.update(CFLAGS=maps, CXXFLAGS=maps, ASMFLAGS=maps)
        command = ['cmake', '-S', str(root / 'runtime'), '-B', str(builddir),
                   '-DCMAKE_BUILD_TYPE=' + args.build_type, '-DCOPYGC_FLAG=1', '-DDOPRA_FLAG=1',
                   '-DRUNTIME_TRACE_FLAG=1', '-DCJ_SDK_VERSION=0.0.1',
                   '-DDISABLE_VERSION_CHECK=1', '-DCMAKE_C_COMPILER=clang',
                   '-DCMAKE_CXX_COMPILER=clang++', '-DCMAKE_AR_PATH=ar',
                   '-DCMAKE_C_COMPILER_LAUNCHER=ccache',
                   '-DCMAKE_CXX_COMPILER_LAUNCHER=ccache',
                   '-DCMAKE_ASM_COMPILER_LAUNCHER=ccache',
                   '-DCMAKE_INSTALL_PREFIX=' + str(root / 'install'),
                   '-DCJ_RUNTIME_COMMIT=' + args.commit]
        record = dict(command=command, mode=label, uptime_before=output(['uptime']),
                      cpus=sorted(os.sched_getaffinity(0)), jobs=os.cpu_count(),
                      toolchain_sha256=sha(root / 'runtime' / toolchain))
        begin = time.monotonic()
        with (work / (label + '-configure.log')).open('w') as log:
            record['configure_rc'] = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
        if record['configure_rc'] == 0 and not args.configure_only:
            with (work / (label + '-build.log')).open('w') as log:
                record['build_rc'] = subprocess.run(['cmake', '--build', str(builddir), '-j' + str(os.cpu_count())], env=env, stdout=log, stderr=subprocess.STDOUT).returncode
        else:
            record['build_rc'] = None
        record['wall'] = time.monotonic() - begin
        record['uptime_after'] = output(['uptime'])
        record['artifacts'] = {str(p.relative_to(builddir)): sha(p) for p in sorted(builddir.rglob('*'))
                               if p.is_file() and p.suffix in ['.o', '.a', '.so']}
        record['thread'] = record['artifacts'].get('runtime-staging/lib/libcangjie-thread.a')
        record['runtime'] = record['artifacts'].get('runtime-staging/lib/x86_64_' + args.build_type + '/libcangjie-runtime.so')
        record['cache_environment'] = {k: v for k, v in env.items() if k.startswith('CCACHE_')}
        child_results = []
        cache_log = work / (label + '-ccache.log')
        child_pids = set()
        if cache_log.exists():
            cache_lines = cache_log.read_text().splitlines()
            for line in cache_lines:
                m = re.match(r'\[[^]]+ (\d+)\] Working directory: (.*)', line)
                if m and '/cjthread-build/src/' in m[2]:
                    child_pids.add(m[1])
            for line in cache_lines:
                m = re.match(r'\[[^]]+ (\d+)\] Result: (.*)', line)
                if m and m[1] in child_pids:
                    child_results.append(m[2])
        record['child_cache_results'] = {k: child_results.count(k) for k in sorted(set(child_results))}
        record['child_cache'] = (builddir / 'cjthread-build/CMakeCache.txt').read_text() if (builddir / 'cjthread-build/CMakeCache.txt').exists() else ''
        (work / (label + '.json')).write_text(json.dumps(record, indent=2) + '\n')
        print(f'BUILD {label} configure_rc={record["configure_rc"]} build_rc={record["build_rc"]} wall={record["wall"]:.1f}', flush=True)
        return record
    records = {'seed': build('seed')}
    records['repeat'] = build('seed', repeat=True)
    with ThreadPoolExecutor(max_workers=5) as pool:
        records.update(zip(names[1:], pool.map(build, names[1:])))
    checks = {}
    for name, rec in records.items():
        checks[name + '_built'] = rec['configure_rc'] == 0 and bool(rec['thread']) and (args.configure_only or (rec['build_rc'] == 0 and bool(rec['runtime'])))
        launchers = all(re.search(r'^CMAKE_' + lang + r'_COMPILER_LAUNCHER:[^=]+=.+$', rec['child_cache'], re.M) for lang in ['C', 'CXX', 'ASM'])
        checks[name + '_launchers'] = launchers == (name != 'launcher_cut')
    checks['child_cache_hit_observed'] = any(records['hit']['child_cache_results'].get(k, 0) > 0 for k in ['direct_cache_hit', 'preprocessed_cache_hit'])
    checks['child_recaching_observed'] = records['seed']['child_cache_results'].get('direct_cache_miss', 0) > 0 and not any(records['seed']['child_cache_results'].get(k, 0) for k in ['direct_cache_hit', 'preprocessed_cache_hit'])
    checks['launcher_cut_no_child_cache'] = not records['launcher_cut']['child_cache_results']
    for kind in (['thread'] if args.configure_only else ['thread', 'runtime']):
        for name in ['repeat', 'hit', 'cold', 'restored']:
            checks[kind + '_equal_' + name] = bool(records['seed'][kind]) and records['seed'][kind] == records[name][kind]
        if args.build_type != 'Debug':
            continue
        checks[kind + '_cut_diverges'] = bool(records['cut_a'][kind]) and bool(records['cut_b'][kind]) and records['cut_a'][kind] != records['cut_b'][kind]
        checks[kind + '_cut_differs_from_restored'] = records['cut_a'][kind] != records['restored'][kind]
    # The same identity predicate reports actual green/cut/restored return codes.
    pairs = [('green', 'seed', 'hit'), ('restored', 'seed', 'restored')]
    if args.build_type == 'Debug':
        pairs.append(('cut', 'cut_a', 'cut_b'))
    for label, left, right in pairs:
        with (work / (label + '-identity.log')).open('w') as log:
            rc = subprocess.run([sys.executable, __file__, '--compare',
                                 str(work / (left + '.json')), str(work / (right + '.json'))],
                                stdout=log, stderr=subprocess.STDOUT).returncode
        (work / (label + '-identity.rc')).write_text(str(rc) + '\n')
        checks[label + '_identity_rc'] = rc == (1 if label == 'cut' else 0)
    for arm in ['seed', 'launcher_cut', 'restored']:
        with (work / (arm + '-launcher.log')).open('w') as log:
            rc = subprocess.run([sys.executable, __file__, '--check-launchers', str(work / (arm + '.json'))],
                                stdout=log, stderr=subprocess.STDOUT).returncode
        (work / (arm + '-launcher.rc')).write_text(str(rc) + '\n')
        checks[arm + '_launcher_rc'] = rc == (1 if arm == 'launcher_cut' else 0)
    for name, ok in checks.items():
        print(f'CJTHREAD_REPRO_ASSERT name={name} result={"PASS" if ok else "FAIL"}', flush=True)
    (work / 'result.json').write_text(json.dumps(dict(build_type=args.build_type, configure_only=args.configure_only, cache_control=args.cache_control, checks=checks, records=records), indent=2) + '\n')
    return 0 if all(checks.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
