import concurrent.futures,hashlib,json,os,pathlib,subprocess,sys,time
root=pathlib.Path('/root/sym_cangjie_runtime_596_implement_r5673746875');build=pathlib.Path(sys.argv[1]);label=sys.argv[2];n=int(sys.argv[3]) if len(sys.argv)>3 else 3
elf=pathlib.Path('/root/sym_cangjie_runtime_593_implement_r5670415547/managed/finalizer_trigger-1/finalizer_trigger');script='/root/sym_cangjie_runtime_596_implement_r5673746875/observe_chain.py'
source=build/'testable/runtime/src/Heap/z/zGeneration.cpp';line=next(i for i,s in enumerate(source.read_text().splitlines(),1) if 'if (!IsMarkedObject<Generation::Old>(finalizerObj))' in s)
enqueue_source=build/'testable/runtime/src/Heap/Collector/FinalizerProcessor.cpp';enqueue_line=next(i for i,s in enumerate(enqueue_source.read_text().splitlines(),1) if 'hasFinalizableJob = true;' in s)
pending_source=build/'testable/runtime/src/Heap/z/zReferenceProcessor.cpp';pending_line=next(i for i,s in enumerate(pending_source.read_text().splitlines(),1) if 'Push(pendingList, node);' in s)
def run(i):
 d=root/label/str(i);d.mkdir(parents=True,exist_ok=True);lib=build/'testable/build/runtime-staging/lib/x86_64_Release'
 env=dict(os.environ,LD_LIBRARY_PATH=str(lib),MRT_LOG_LEVEL='e',cjHeapSize='256MB',GDB_COUNT_OUT=str(d/'chain.json'),B19_HIST_INPUT='/root/sym_cangjie_runtime_596_explore_r5671598814/hist.py',B19_DISCOVERY_LINE=str(line),B19_ENQUEUE_LINE=str(enqueue_line),B19_PENDING_LINE=str(pending_line),B19_CANDIDATE='0' if 'baseline' in str(build) else '1')
 before=subprocess.check_output(['uptime'],text=True);start=time.monotonic()
 with (d/'gdb.log').open('w') as f:r=subprocess.run(['taskset','-c',f'{32+(i-1)*4}-{32+i*4-1}','timeout','200s','gdb','-batch','-nx','-x',script,'-ex','run','-ex','b19-dump','--args',str(elf)],env=env,stdout=f,stderr=f)
 text=(d/'gdb.log').read_text();data=json.loads((d/'chain.json').read_text()) if (d/'chain.json').exists() else {}
 result=dict(gdb_rc=r.returncode,counts=data.get('counts'),errors=data.get('errors'),finalized=sum(x.startswith('FINALIZER_CALLED id=') for x in text.splitlines()),done=text.splitlines().count('FINALIZER_TRIGGER_DONE allocated=64'),elf_sha256=hashlib.sha256(elf.read_bytes()).hexdigest(),so={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in lib.glob('*.so')},script_sha256=hashlib.sha256(pathlib.Path(script).read_bytes()).hexdigest(),cores=f'{32+(i-1)*4}-{32+i*4-1}',wall=time.monotonic()-start,uptime_before=before,uptime_after=subprocess.check_output(['uptime'],text=True))
 (d/'result.json').write_text(json.dumps(result,indent=2));return dict(run=i,**{k:result[k] for k in ('gdb_rc','counts','errors','finalized','done','wall')})
with concurrent.futures.ThreadPoolExecutor(max_workers=n) as pool:print(json.dumps(list(pool.map(run,range(1,n+1))),indent=2))
