import concurrent.futures, hashlib, json, os, pathlib, subprocess, time
root=pathlib.Path('/root/sym_cangjie_runtime_740_implement_r5747608277-baseline')
cached=pathlib.Path('/root/diff_b6d62daa8f35')
common=['StoreBuf.ProductWriteCarriesOldValueOnlyInPrevArm','StoreBuf.ProductPhaseFlushHandsPairedPrevToMark','StoreBuf.CompilerStoreBadOverwriteHandsObservedOldToMark','BarrierOldAtomic.NoAllocBufferOverwriteRetiresOldValue','BarrierOldAtomic.AllocBufferOverwriteRetiresOldValueControl','BarrierOldAtomic.ReflectionStaticAggregateStoreRetiresNativeOldValue']
young=['YoungConc.RemovingExportRootPublishesPreviousValue','YoungConc.BulkWritePublishesSatbWithoutYoungRegions','YoungConc.TraceStorePublishesPreviousYoungTarget','YoungConc.MarkEndDomainContainsPublishedYoungWork','YoungConc.StoreBufferFlushPublishesYoungMarkWork']
out=root/'reproduction'; out.mkdir(exist_ok=True)
def run(arm,name):
    lib=root/arm/'build/runtime-staging/lib/x86_64_Release'
    elf=cached/f'unit-{arm}'/'cj_gc_unit'
    env=os.environ.copy(); env.update(LD_LIBRARY_PATH=str(lib), GC_UNIT_FILTER=name)
    t=time.monotonic()
    p=subprocess.run([str(elf)],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=60)
    (out/f'{arm}-{name}.log').write_bytes(p.stdout)
    return dict(arm=arm,name=name,rc=p.returncode,wall=time.monotonic()-t,elf=str(elf),elf_sha256=hashlib.sha256(elf.read_bytes()).hexdigest(),so_sha256=hashlib.sha256((lib/'libcangjie-runtime.so').read_bytes()).hexdigest())
(out/'uptime-before.txt').write_bytes(subprocess.check_output(['uptime']))
with concurrent.futures.ThreadPoolExecutor(max_workers=17) as pool:
    results=list(pool.map(lambda a:run(*a),[(a,n) for a in ['default','testable'] for n in common+(young if a=='testable' else [])]))
(out/'results.json').write_text(json.dumps(results,indent=2))
(out/'uptime-after.txt').write_bytes(subprocess.check_output(['uptime']))
for r in results: print(r['arm'],r['name'],'rc='+str(r['rc']))
