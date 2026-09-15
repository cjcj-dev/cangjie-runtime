import concurrent.futures, hashlib, json, os, pathlib, subprocess, sys, time
base=pathlib.Path('/root/sym_cangjie_runtime_596_implement_r5673746875-base')
root=pathlib.Path('/root/sym_cangjie_runtime_596_implement_r5673746875')
arm=sys.argv[1]; lib=pathlib.Path(sys.argv[2]); out=root/('a2-'+arm); out.mkdir(parents=True,exist_ok=True)
unit=base/'testable/a2-export-unit'
tests=[('cj_gc_unit','ThreadRootCurrent.'+n) for n in ['C1StackFieldHistoricalColor','C2ObjectRefHistoricalColor','C3InvisibleHistoricalColor','C4HeaderlessHistoricalColor']]
tests += [('cj_gc_unit','NativeRootCurrent.MajorSeed')]
source=base/'testable/runtime/tests/gc_unit/clear_entries_product_unit.cpp'
import re
tests += [('cj_gc_forwarding_publication_unit', 'ValueRootCurrentization.'+n) for n in re.findall(r'GC_OTHER_VM_TEST\(ValueRootCurrentization, (\w+)\)',source.read_text())]
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
so={p.name:sha(p) for p in lib.glob('*.so')}
elf={name:sha(unit/name) for name,_ in tests}
def run_group(i):
    rows=[]; cores=['32-35','36-39','40-43'][i-1]
    for name,test in tests:
        d=out/test/str(i);d.mkdir(parents=True,exist_ok=True)
        env=dict(os.environ,LD_LIBRARY_PATH=str(lib));t=time.monotonic();before=subprocess.check_output(['uptime'],text=True)
        cmd=['taskset','-c',cores,str(unit/name),'--gtest_filter='+test]
        with (d/'run.log').open('w') as f:
            try: rc=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=60).returncode
            except subprocess.TimeoutExpired: rc=124
        log=(d/'run.log').read_text()
        row=dict(arm=arm,test=test,n=i,rc=rc,cmd=cmd,elf_sha256=elf[name],so_sha256=so,lib=str(lib),cores=cores,uptime_before=before,uptime_after=subprocess.check_output(['uptime'],text=True),wall=time.monotonic()-t,target=[l for l in log.splitlines() if 'TARGET' in l or 'marker_current executed' in l],passed='[  PASS  ] '+test in log)
        (d/'result.json').write_text(json.dumps(row,indent=2));rows.append(row)
    return rows
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool: rows=sum(pool.map(run_group,[1,2,3]),[])
(out/'results.json').write_text(json.dumps(rows,indent=2))
print(arm, [(r['test'],r['n'],r['rc']) for r in rows if r['rc']]);print('runs=',len(rows))
