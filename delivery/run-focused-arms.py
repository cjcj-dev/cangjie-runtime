import subprocess,os,time,json,hashlib
from pathlib import Path
lane='sym_cangjie_runtime_606_implement_r5673376405'
r=Path('/root')/(lane+'-accept');dest=r/'focused';dest.mkdir(exist_ok=True)
tests=['P1Mark.AllocatingAndRelocatablePolicyMatrix','P1Mark.DuplicateAnyThreadStopsAtConsumer','P1Mark.ResurrectAndInactivePhasePolicies','P1Mark.PinnedReclaimedSlotIsNotAllocationSource','MarkAllocation.LargeHolderAndNewTargetAreImplicitlyLive','MarkAllocation.LargeHolderKeepsRootedExistingTargetLive','LargePageGeneration.ArrayRootKeepsYoungTargetLive','PinRoot.NativeHeldUnmarkedPinnedObjectSurvivesUntilRemove','LiveMap.PageBirthSequenceIsImplicitLive','MarkStackEntry.ObjectPoliciesAreIndependent']
elf=r/'unit-testable/cj_gc_unit';children=[]
(dest/'uptime-before.txt').write_bytes(subprocess.check_output(['uptime']))
for arm in ['green','cut-alloc','cut-retire','cut-consumer','cut-pinned','restored']:
 product=Path('/root')/(lane+'-'+('accept' if arm in ['green','restored'] else arm))/'testable/build/runtime-staging/lib/x86_64_Release'
 d=dest/arm;d.mkdir(exist_ok=True)
 (d/'identity.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [elf,product/'libcangjie-runtime.so',product/'libboundscheck.so']},indent=2))
 for test in tests:
  log=(d/(test+'.log')).open('wb');env=os.environ.copy();env['LD_LIBRARY_PATH']=str(product);env['GC_UNIT_TALLY_FILE']=str(d/(test+'.tally'))
  start=time.monotonic();p=subprocess.Popen(['taskset','-c','0-31','timeout','60',str(elf),'--gtest_filter='+test],env=env,stdout=log,stderr=subprocess.STDOUT)
  children.append((arm,test,p,log,start,d))
results=[]
for arm,test,p,log,start,d in children:
 rc=p.wait();log.close();results.append(dict(arm=arm,test=test,rc=rc,wall=time.monotonic()-start))
 (d/(test+'.rc')).write_text(str(rc)+'\n')
(dest/'results.json').write_text(json.dumps(results,indent=2))
(dest/'uptime-after.txt').write_bytes(subprocess.check_output(['uptime']))
for arm in ['green','cut-alloc','cut-retire','cut-consumer','cut-pinned','restored']:
 rows=[x for x in results if x['arm']==arm];print(arm,'pass',sum(x['rc']==0 for x in rows),'fail',[x['test'] for x in rows if x['rc']!=0],flush=True)
