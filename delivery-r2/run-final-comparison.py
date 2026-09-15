import subprocess, os, json, time, hashlib, re
from pathlib import Path
lane='sym_cangjie_runtime_606_implement_r5674249495'
r=Path('/root')/(lane+'-final2');out=r/'comparison';out.mkdir(exist_ok=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def identity(product, files):
 return {'files':{str(p):sha(p) for p in files+[product/'libcangjie-runtime.so',product/'libboundscheck.so']},
         'timestamp':time.time(),'stamp':[x for x in subprocess.check_output(['strings',str(product/'libcangjie-runtime.so')],text=True).splitlines() if 'CJRT-COMMIT:' in x or 'CJRT-DECLARED:' in x]}
arms=['green','cut-strong','cut-final','cut-producer','cut-consumer','cut-mixed-selection','cut-combined','restored']
tests=['P1BitMap.StrongClaimResult','P1BitMap.FinalizableClaimResult','P1BitMap.ClaimContentsAndUpgradeControl',
       'P1Mark.AllocatingAndRelocatablePolicyMatrix','P1Mark.DuplicateAnyThreadStopsAtConsumer','P1Mark.ResurrectAndInactivePhasePolicies',
       'YoungWeakClosure.SingleWorkerKeepsYoungReferentStrong','YoungWeakClosure.StripedKeepsYoungReferentStrong',
       'MarkingStacksProduct.MajorSerialEntersFromDoGarbageCollection','MarkAllocation.LargeHolderKeepsRootedExistingTargetLive']
main=r/'unit-testable/cj_gc_unit';publication=r/'unit-testable/cj_gc_forwarding_publication_unit'
(out/'uptime-before.txt').write_bytes(subprocess.check_output(['uptime']))
children=[]
for arm in arms:
 product=(r if arm in ['green','restored'] else Path('/root')/(lane+'-final2-'+arm))/'testable/build/runtime-staging/lib/x86_64_Release'
 d=out/arm;d.mkdir(exist_ok=True)
 doc=identity(product,[main,publication]);doc['cpus']=sorted(os.sched_getaffinity(0));(d/'identity.json').write_text(json.dumps(doc,indent=2))
 env=os.environ.copy();env.update(LD_LIBRARY_PATH=str(product),MRT_TESTABLE_INTERNALS='1',GC_UNIT_JOBS='192')
 commands=[('suite',['bash',str(r/'testable/runtime/tests/gc_unit/run_parallel_tests.sh'),str(main),str(publication),str(d/'suite'),str(product)])]
 commands += [(test,['taskset','-c','0-31','timeout','60',str(main),'--gtest_filter='+test]) for test in tests]
 for test,cmd in commands:
  log=(d/(test+'.log')).open('wb');start=time.monotonic();p=subprocess.Popen(cmd,env=env,stdout=log,stderr=subprocess.STDOUT)
  children.append((arm,test,cmd,p,log,start,d))
results=[]
for arm,test,cmd,p,log,start,d in children:
 rc=p.wait();log.close();row=dict(arm=arm,test=test,command=cmd,rc=rc,wall=time.monotonic()-start);results.append(row)
 (d/(test+'.rc')).write_text(str(rc)+'\n')
(out/'results.json').write_text(json.dumps(results,indent=2))
(out/'uptime-after.txt').write_bytes(subprocess.check_output(['uptime']))
summary={}
for arm in arms:
 content=(out/arm/'suite.log').read_text(errors='replace')
 failed=sorted(set(re.findall(r'^\[  FAILED  \] ([A-Za-z_][\w]*\.[\w]+)$',content,re.M)))
 counts=re.findall(r'^\[========\] (\d+) tests: (\d+) passed, (\d+) failed$',content,re.M)
 summary[arm]={'failed':failed,'counts':counts[-1] if counts else None}
base=set(summary['green']['failed'])
for arm in arms:
 summary[arm]['new_failures']=sorted(set(summary[arm]['failed'])-base)
 summary[arm]['recovered']=sorted(base-set(summary[arm]['failed']))
 print(arm,summary[arm]['counts'],'new_failures=',summary[arm]['new_failures'],flush=True)
(out/'suite-deltas.json').write_text(json.dumps(summary,indent=2))
