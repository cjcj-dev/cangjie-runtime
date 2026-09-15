import concurrent.futures,hashlib,json,os,pathlib,subprocess,time
base=pathlib.Path('/root/sym_cangjie_runtime_596_implement_r5673746875')
fixture=pathlib.Path('/root/sym_cangjie_runtime_593_implement_r5670415547')
elf=fixture/'managed/finalizer_trigger-1/finalizer_trigger'
runner=fixture/'testable/runtime/tests/gc_unit/run_finalizer_trigger.sh'
source=runner.read_text();body=source[source.index('set +e\nLD_LIBRARY_PATH='):]
root=base/'finalizer-qualified';root.mkdir(exist_ok=True)
(root/'original-run-and-assert.sh').write_text(body)
sdk=pathlib.Path('/root/sym_cangjie_runtime_585_implement_r5669360435/gate-sdk')
host=pathlib.Path('/root/sym_cangjie_runtime_585_implement_r5669360435/host')
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
assert sha(elf)=='b601f77e18539bb6e850d98b8211dd4115b1dbaae92252d9199575ae4677f355'
def run(job):
 arm,config,i=job;d=root/arm/config/str(i);d.mkdir(parents=True,exist_ok=True)
 lib=pathlib.Path(str(base)+'-'+arm)/config/'build/runtime-staging/lib/x86_64_Release'
 env=dict(os.environ,BIN=str(elf),RUN_LOG=str(d/'run.log'),RUNTIME_LIB_DIR=str(lib),SDK_RUNTIME=str(sdk/'runtime/lib/linux_x86_64_cjnative'),LD_LIBRARY_PATH=f'{host}/runtime/lib/linux_x86_64_cjnative:{host}/third_party/llvm/lib:{host}/tools/lib')
 before=subprocess.check_output(['uptime'],text=True);start=time.monotonic();cpu=f'{32+(i-1)*4}-{32+i*4-1}'
 with (d/'runner.log').open('w') as f:
  p=subprocess.Popen(['taskset','-c',cpu,'bash','-c','set -euo pipefail\n'+body],env=env,stdout=f,stderr=f)
  # Hash/map the actual inferior selected by its known executable path.
  deadline=time.monotonic()+.6
  while p.poll() is None and time.monotonic()<deadline:
   for entry in pathlib.Path('/proc').iterdir():
    if not entry.name.isdigit():continue
    try:
     if (entry/'exe').resolve()!=elf:continue
     environ=(entry/'environ').read_bytes()
     if ('RUN_LOG='+str(d/'run.log')).encode()+b'\0' not in environ:continue
     (d/'maps.txt').write_text((entry/'maps').read_text());deadline=0;break
    except OSError:pass
   time.sleep(.02)
  rc=p.wait()
 lines=(d/'run.log').read_text().splitlines();ids=[x.split('id=',1)[1] for x in lines if x.startswith('FINALIZER_CALLED id=')]
 result=dict(arm=arm,config=config,sample=i,runner_rc=rc,runner_text=(d/'runner.log').read_text(),done=lines.count('FINALIZER_TRIGGER_DONE allocated=64'),finalized=len(ids),unique_ids=len(set(ids)),elf_sha256=sha(elf),so={p.name:sha(p) for p in lib.glob('*.so')},original_runner_sha256=sha(runner),run_assert_sha256=hashlib.sha256(body.encode()).hexdigest(),compile='not repeated; frozen ELF',cores=cpu,uptime_before=before,uptime_after=subprocess.check_output(['uptime'],text=True),wall=time.monotonic()-start)
 (d/'result.json').write_text(json.dumps(result,indent=2));return result
jobs=[(a,c,i) for a in ['green','old-strong-cut','finalizer-cut','restored'] for c in ['default','testable'] for i in range(1,4)]
with concurrent.futures.ThreadPoolExecutor(max_workers=len(jobs)) as p:rows=list(p.map(run,jobs))
(root/'results.json').write_text(json.dumps(rows,indent=2));print([(r['arm'],r['config'],r['sample'],r['runner_rc'],r['done'],r['finalized'],r['unique_ids']) for r in rows])
