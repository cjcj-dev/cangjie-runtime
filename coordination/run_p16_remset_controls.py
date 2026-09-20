#!/usr/bin/env python3
"""Run retained product arms with one fixed ELF and an exact, enumerated test set."""
import concurrent.futures, hashlib, json, os, pathlib, subprocess, time
lane='sym_cangjie_runtime_627_implement_r5744767112'
root=pathlib.Path('/root')
elf=pathlib.Path(os.environ.get('P16_TEST_ELF',str(root/(lane+'-tests14/unit-current-default/cj_gc_unit'))))
names=[x for x in (elf.parent/'test-lists/main.txt').read_text().splitlines() if x.startswith('ZVerify.')]
assert names and 'ZVerify.RelocationEntryRejectsInactiveRemset' in names
out=root/lane/os.environ.get('P16_CONTROL_OUT','remset-exact')
out.mkdir(parents=True,exist_ok=False)
(out/'tests.json').write_text(json.dumps(names,indent=2))
subprocess.run(['uptime'],stdout=(out/'uptime-before.txt').open('w'),check=True)
libs={'green':root/'diff_e0d865927fe7/default/build/runtime-staging/lib/x86_64_Release','cut':root/(lane+os.environ.get('P16_CUT_SUFFIX','-remset-cut')+'/default/build/runtime-staging/lib/x86_64_Release'),'restored':root/(lane+'-remset-restored/default/build/runtime-staging/lib/x86_64_Release')}
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
identities={arm:{p.name:sha(p) for p in [elf,*lib.glob('*.so')]} for arm,lib in libs.items()}
(out/'identity.json').write_text(json.dumps(identities,indent=2))
assert identities['green']==identities['restored']
assert identities['green'][elf.name]==identities['cut'][elf.name]
def run(item):
 arm,name,n=item;path=out/arm;path.mkdir(exist_ok=True);log=path/(name+'-'+str(n)+'.log');start=time.monotonic()
 env=dict(os.environ,LD_LIBRARY_PATH=str(libs[arm]));
 with log.open('w') as stream:
  r=subprocess.run(['taskset','-c','32-47','timeout','45',str(elf),'--gtest_filter='+name],env=env,stdout=stream,stderr=subprocess.STDOUT)
 (path/(name+'-'+str(n)+'.rc')).write_text(str(r.returncode)+'\n')
 return dict(arm=arm,test=name,n=n,rc=r.returncode,wall=time.monotonic()-start)
with concurrent.futures.ThreadPoolExecutor(max_workers=39) as pool:
 result=list(pool.map(run,[(a,t,n) for a in libs for t in names for n in range(1,4)]))
(out/'result.json').write_text(json.dumps(result,indent=2))
subprocess.run(['uptime'],stdout=(out/'uptime-after.txt').open('w'),check=True)
for arm in libs:
 failures=sorted(set(x['test'] for x in result if x['arm']==arm and x['rc']!=0))
 print(arm,'runs',sum(x['arm']==arm for x in result),'failed_tests',failures)
