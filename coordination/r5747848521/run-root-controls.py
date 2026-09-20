import concurrent.futures,hashlib,json,os,pathlib,subprocess,time
r=pathlib.Path('/root/sym_cangjie_runtime_627_implement_r5747848521')
out=r/'root-controls-v2';out.mkdir(exist_ok=True)
elf=r/'root-v2/cj_gc_unit'
green=r/'testable/build/runtime-staging/lib/x86_64_Release'
libs={'green':green,'producer':pathlib.Path(str(r)+'_producer')/'testable/build/runtime-staging/lib/x86_64_Release','consumer':pathlib.Path(str(r)+'_consumer')/'testable/build/runtime-staging/lib/x86_64_Release','restored':green}
tests=['RootStorageSegments.Strong','RootStorageSegments.WeakFinalizer','RootStorageSegments.Export','RegionAge.YoungAgeRoundTrip']
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
identity={'elf':str(elf),'elf_sha256':sha(elf),'arms':{a:{p.name:sha(p) for p in lib.glob('*.so')} for a,lib in libs.items()}}
(out/'identity.json').write_text(json.dumps(identity,indent=2))
(out/'uptime-before.txt').write_text(subprocess.check_output(['uptime'],text=True))
def run(spec):
 a,t,n=spec;log=out/f'{a}-{t}-{n}.log';start=time.monotonic()
 with log.open('w') as f:
  try:rc=subprocess.run(['taskset','-c','80-95',str(elf),'--gtest_filter='+t],env=dict(os.environ,LD_LIBRARY_PATH=str(libs[a])),stdout=f,stderr=subprocess.STDOUT,timeout=60).returncode
  except subprocess.TimeoutExpired:rc=124
 text=log.read_text();return dict(arm=a,test=t,n=n,rc=rc,wall=time.monotonic()-start,target=[l for l in text.splitlines() if 'ROOT_SEGMENT_TARGET' in l or 'EXPECT failed:' in l],log=str(log))
# Independent processes share no ports/files; restored is run after both cuts.
results=[]
for arms in [['green','producer','consumer'],['restored']]:
 with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:results+=list(pool.map(run,[(a,t,n) for a in arms for t in tests for n in range(1,4)]))
(out/'results.json').write_text(json.dumps(results,indent=2));(out/'uptime-after.txt').write_text(subprocess.check_output(['uptime'],text=True))
for a in libs:
 for t in tests:
  rows=[x for x in results if x['arm']==a and x['test']==t];print(a,t,[x['rc'] for x in rows],rows[0]['target'])
