import concurrent.futures, hashlib, json, os, pathlib, subprocess, time
root = pathlib.Path('/root/sym_cangjie_runtime_471_implement_r5747758651-evidence')
tests = ['OldRelocationStatistics.FullCollectionPublishesLiveInput',
         'OldRelocationStatistics.DirectorConsumesOldSelection',
         'RelocationSetSelector.EmptySet',
         'GcDirector.CollectionCountsFollowYoungMarkStarts',
         'RelocateWorkers.ProductParallelEntryRegistersWorkersAndClosesGeneration',
         'RelocateWorkers.ProductSerialEntryRegistersWorkerAndClosesGeneration',
         'RelocateWorkers.ProductYoungRuntimeEntryClosesRelocationRequestGeneration']
(root/'causal').mkdir(exist_ok=True)
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def run(item):
    arm, config, name = item
    d = root/'causal'/arm/config
    d.mkdir(parents=True, exist_ok=True)
    elf = root/'tests'/config/'cj_gc_unit'
    lib = root/'products'/arm/config
    env = os.environ.copy()
    env['LD_LIBRARY_PATH'] = str(lib)
    env.pop('GC_UNIT_OTHER_VM_CHILD', None)
    log = d/(name+'.log')
    started = time.monotonic()
    with log.open('wb') as out:
        result = subprocess.run(['taskset','-c','32-47',str(elf),'--gtest_filter='+name],
                                env=env, stdout=out, stderr=subprocess.STDOUT, timeout=90)
    record = dict(arm=arm, config=config, test=name, rc=result.returncode,
                  wall=time.monotonic()-started, cores='32-47',
                  elf_sha256=sha(elf), so_sha256=sha(lib/'libcangjie-runtime.so'),
                  boundscheck_sha256=sha(lib/'libboundscheck.so'), log=str(log))
    (d/(name+'.json')).write_text(json.dumps(record,indent=2))
    return record
jobs=[(arm,config,name) for arm in ('green','producer','consumer','restored')
      for config in ('default','testable') for name in tests
      if config=='testable' or name in (tests[0],tests[2],tests[3])]
(root/'causal'/'uptime-before.txt').write_bytes(subprocess.check_output(['uptime']))
with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
    records=list(pool.map(run,jobs))
(root/'causal'/'uptime-after.txt').write_bytes(subprocess.check_output(['uptime']))
(root/'causal'/'results.json').write_text(json.dumps(records,indent=2))
for arm in ('green','producer','consumer','restored'):
    for config in ('default','testable'):
        group=[r for r in records if r['arm']==arm and r['config']==config]
        print(arm,config,'n='+str(len(group)), 'failed='+str([r['test'] for r in group if r['rc']]))
