import subprocess, os, json, time, hashlib
from pathlib import Path
lane='sym_cangjie_runtime_606_implement_r5674249495'
r=Path('/root')/(lane+'-green5'); out=r/'cuts';out.mkdir(exist_ok=True)
tests=['P1BitMap.StrongClaimResult','P1BitMap.FinalizableClaimResult','P1BitMap.ClaimContentsAndUpgradeControl','P1Mark.AllocatingAndRelocatablePolicyMatrix','P1Mark.DuplicateAnyThreadStopsAtConsumer','P1Mark.ResurrectAndInactivePhasePolicies','YoungConc.Y2yDirtyVisibleBeforePauseMarkEnd','YoungConc.Y2yAfterReleaseBatchForcesContinueAndReachesClosure','MarkAllocation.LargeHolderKeepsRootedExistingTargetLive','YoungWeakClosure.SingleWorkerKeepsYoungReferentStrong','YoungWeakClosure.StripedKeepsYoungReferentStrong','ValueRootCurrentization.MinorRuntimeDispatchMarksCurrentAndWritesBack','MarkingStacksProduct.MajorSerialEntersFromDoGarbageCollection']
(out/'uptime-before.txt').write_bytes(subprocess.check_output(['uptime']))
children=[]
for arm in ['green','cut-strong','cut-final','cut-producer','cut-consumer','cut-old-order','cut-young-order','cut-mixed-selection','restored']:
 product=(r if arm in ['green','restored'] else Path('/root')/(lane+'-v5-'+arm))/'testable/build/runtime-staging/lib/x86_64_Release'
 d=out/arm;d.mkdir(exist_ok=True)
 files=[r/'unit-testable/cj_gc_unit',r/'unit-testable/cj_gc_forwarding_publication_unit',product/'libcangjie-runtime.so',product/'libboundscheck.so']
 identity={'files':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},'cores':sorted(os.sched_getaffinity(0)),'timestamp':time.time(),'stamp':[s for s in subprocess.check_output(['strings',str(files[2])],text=True).splitlines() if 'CJRT-COMMIT:' in s or 'CJRT-DECLARED:' in s]}
 (d/'identity.json').write_text(json.dumps(identity,indent=2))
 env=os.environ.copy();env['LD_LIBRARY_PATH']=str(product);env['MRT_TESTABLE_INTERNALS']='1';env['GC_UNIT_JOBS']='192'
 log=(d/'suite.log').open('wb');start=time.monotonic()
 p=subprocess.Popen(['bash',str(r/'testable/runtime/tests/gc_unit/run_parallel_tests.sh'),str(files[0]),str(files[1]),str(d/'suite'),str(product)],env=env,stdout=log,stderr=subprocess.STDOUT)
 children.append((arm,'suite',p,log,start,d))
 for test in tests:
  f=(d/(test+'.log')).open('wb');start=time.monotonic()
  p=subprocess.Popen(['taskset','-c','0-31','timeout','60',str(files[0]),'--gtest_filter='+test],env=env,stdout=f,stderr=subprocess.STDOUT)
  children.append((arm,test,p,f,start,d))
results=[]
for arm,test,p,log,start,d in children:
 rc=p.wait();log.close();row=dict(arm=arm,test=test,rc=rc,wall=time.monotonic()-start);results.append(row)
 (d/(test+'.rc')).write_text(str(rc)+'\n')
(out/'results.json').write_text(json.dumps(results,indent=2));(out/'uptime-after.txt').write_bytes(subprocess.check_output(['uptime']))
for arm in ['green','cut-strong','cut-final','cut-producer','cut-consumer','cut-old-order','cut-young-order','cut-mixed-selection','restored']:
 print(arm,[(x['test'],x['rc']) for x in results if x['arm']==arm and x['rc']!=0],flush=True)
