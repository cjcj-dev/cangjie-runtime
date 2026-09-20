import concurrent.futures, hashlib, json, os, pathlib, re, subprocess, time
lane='sym_cangjie_runtime_740_implement_r5747608277'
root=pathlib.Path('/root/'+lane+'-green'); out=root/'matrix-final';out.mkdir(exist_ok=True)
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
common=[]
for f in ['test_store_barrier_buffer.cpp','test_barrier_old_atomic.cpp']:
 text=(root/'default/runtime/tests/gc_unit'/f).read_text()
 common += [a+'.'+b for a,b in re.findall(r'GC_(?:OTHER_VM_)?TEST\((\w+),\s*(\w+)\)',text)]
young=['YoungConc.RemovingExportRootPublishesPreviousValue','YoungConc.BulkWritePublishesSatbWithoutYoungRegions','YoungConc.TraceStorePublishesPreviousYoungTarget','YoungConc.MarkEndDomainContainsPublishedYoungWork','YoungConc.StoreBufferFlushPublishesYoungMarkWork','YoungConc.ExportRootRegistrationDoesNotMarkIncomingValue','YoungConc.IdleStoreDoesNotPublishMarkWork']
ident={}
for config in ['default','testable']:
 for arm in ['green','entry','add','remember','flush','restored']:
  lib=pathlib.Path('/root/'+lane+'-'+arm)/config/'build/runtime-staging/lib/x86_64_Release'
  elf=root/f'unit-final-{config}'/'cj_gc_unit'
  ident[config+'/'+arm]=dict(elf=str(elf),elf_sha256=digest(elf),so=str(lib/'libcangjie-runtime.so'),so_sha256=digest(lib/'libcangjie-runtime.so'),bounds_sha256=digest(lib/'libboundscheck.so'))
 for arm in ['restored']:
  assert ident[config+'/green']['so_sha256']==ident[config+'/'+arm]['so_sha256'], 'restored identity mismatch'
 for arm in ['entry','add','remember','flush']:
  assert ident[config+'/green']['so_sha256']!=ident[config+'/'+arm]['so_sha256']
(out/'identity.json').write_text(json.dumps(ident,indent=2))
def run(job):
 config,arm,name=job;lib=pathlib.Path(ident[config+'/'+arm]['so']).parent;elf=ident[config+'/'+arm]['elf']
 folder=out/(config+'-'+arm);folder.mkdir(exist_ok=True)
 env=os.environ.copy();env.update(LD_LIBRARY_PATH=str(lib),GC_UNIT_FILTER=name)
 start=time.monotonic()
 try:
  p=subprocess.run([elf],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=60)
  rc=p.returncode;data=p.stdout
 except subprocess.TimeoutExpired as e:rc=124;data=(e.stdout or b'')+b'\nMATRIX TIMEOUT\n'
 (folder/(name+'.log')).write_bytes(data)
 return dict(config=config,arm=arm,name=name,rc=rc,wall=time.monotonic()-start,log=str(folder/(name+'.log')))
registered = {}
for config in ['default', 'testable']:
    identity=ident[config+'/green']
    env=os.environ.copy(); env.update(LD_LIBRARY_PATH=str(pathlib.Path(identity['so']).parent),GC_UNIT_LIST_TESTS='1')
    listing=subprocess.run([identity['elf']],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,check=True)
    (out/(config+'-registered.txt')).write_bytes(listing.stdout)
    names=set(); suite=''
    for line in listing.stdout.decode().splitlines():
        if line.endswith('.') and not line.startswith(' '): suite=line[:-1]
        elif line.startswith('  '): names.add(suite+'.'+line.strip())
    registered[config]=names
jobs=[(c,a,n) for c in ['default','testable'] for a in ['green','entry','add','remember','flush','restored'] for n in common+(young if c=='testable' else []) if n in registered[c]]
(out/'excluded-unregistered.json').write_text(json.dumps({c:sorted(set(common+(young if c=='testable' else []))-registered[c]) for c in registered},indent=2))
(out/'uptime-before.txt').write_bytes(subprocess.check_output(['uptime']))
(out/'cpu-domain.txt').write_text('affinity='+str(sorted(os.sched_getaffinity(0)))+'\nworkers=192\n')
start=time.monotonic()
with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:results=list(pool.map(run,jobs))
(out/'uptime-after.txt').write_bytes(subprocess.check_output(['uptime']))
(out/'results.json').write_text(json.dumps(results,indent=2))
for c in ['default','testable']:
 for a in ['green','entry','add','remember','flush','restored']:
  rows=[r for r in results if r['config']==c and r['arm']==a]
  print(c,a,'n='+str(len(rows)),'failed='+str(sum(r['rc']!=0 for r in rows)))
  for r in rows:
   if r['rc']!=0:print(' ',r['name'],'rc='+str(r['rc']))
print('wall='+str(time.monotonic()-start))
