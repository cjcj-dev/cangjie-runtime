import concurrent.futures, hashlib, json, os, pathlib, subprocess, time
root = pathlib.Path('/root/sym_cangjie_runtime_627_implement_r5744767112-cont-debug-qualified')
out = root / 'results'
out.mkdir(exist_ok=False)
elf = pathlib.Path('/root/sym_cangjie_runtime_627_implement_r5744767112/debug-funnels/verify_debug_funnels_v4')
arms = ['green', 'iterator-cut', 'uncolored-cut', 'restored']
modes = [f'{f}-{s}' for f in ['iterator', 'uncolored'] for s in ['valid', 'saferegion', 'gcworker', 'worldstopped']]
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
identity = {a: {p.name: sha(p) for p in [elf, *(root/'arms'/a).glob('*.so')]} for a in arms}
assert identity['green'] == identity['restored']
(out/'identity.json').write_text(json.dumps(identity, indent=2))
subprocess.run(['uptime'], stdout=(out/'uptime-before.txt').open('w'), check=True)
for a in arms:
    subprocess.run(['nm', '-C', '--defined-only', str(root/'arms'/a/'libcangjie-runtime.so')],
                   stdout=(out/(a+'-product-defined.txt')).open('w'), check=True)
for kind, flags in [('defined', ['--defined-only']), ('imports', ['-u'])]:
    subprocess.run(['nm', '-C', *flags, str(elf)], stdout=(out/('elf-'+kind+'.txt')).open('w'), check=True)
definitions = (out/'elf-defined.txt').read_text()
imports = (out/'elf-imports.txt').read_text()
for symbol in ['ZIterator::basic_oop_iterate_safe', 'Mutator::GcPhaseEnum']:
    assert symbol in imports and symbol not in definitions, symbol
def run(job):
    arm, mode, n = job
    path = out/arm
    path.mkdir(exist_ok=True)
    start = time.monotonic()
    env = dict(os.environ, LD_LIBRARY_PATH=str(root/'arms'/arm),
               ZVerifyOops='0', ZVerifyRoots='0', ZVerifyMarking='0', ZVerifyRemembered='0')
    log = path/(mode+'-'+str(n)+'.log')
    with log.open('w') as stream:
        p = subprocess.run(['taskset', '-c', '32-47', 'timeout', '25', str(elf), mode],
                           env=env, stdout=stream, stderr=subprocess.STDOUT)
    text = log.read_text()
    expected = 134 if mode.endswith('saferegion') else 0
    target = 'WorldStopped()' if expected == 134 else 'DEBUG_FUNNEL_RESULT'
    rc = p.returncode if p.returncode >= 0 else 128 - p.returncode
    matched = rc == expected and target in text
    if expected == 0: matched = matched and 'matched=1' in text
    (path/(mode+'-'+str(n)+'.rc')).write_text(str(rc)+'\n')
    return dict(arm=arm, mode=mode, n=n, raw_rc=p.returncode, rc=rc, expected=expected, matched=matched,
                wall=time.monotonic()-start)
with concurrent.futures.ThreadPoolExecutor(max_workers=32) as pool:
    result = list(pool.map(run, [(a,m,n) for a in arms for m in modes for n in range(1,4)]))
(out/'result.json').write_text(json.dumps(result, indent=2))
subprocess.run(['uptime'], stdout=(out/'uptime-after.txt').open('w'), check=True)
for a in arms:
    rows = [x for x in result if x['arm'] == a]
    failures = sorted(set(x['mode'] for x in rows if not x['matched']))
    print(a, 'runs', len(rows), 'failed_targets', failures)
    expected = [] if a in ['green', 'restored'] else [a.removesuffix('-cut')+'-saferegion']
    assert failures == expected, (a, failures, expected)
