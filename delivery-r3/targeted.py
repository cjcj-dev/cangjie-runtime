import subprocess, os, json, time, hashlib, re
from pathlib import Path
lane='sym_cangjie_runtime_606_implement_r5675167676'
r=Path('/root')/(lane+'-final');out=r/'targeted';out.mkdir(exist_ok=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def identity(product, files):
 return {'files':{str(p):sha(p) for p in files+[product/'libcangjie-runtime.so',product/'libboundscheck.so']},
         'timestamp':time.time(),'stamp':[x for x in subprocess.check_output(['strings',str(product/'libcangjie-runtime.so')],text=True).splitlines() if 'CJRT-COMMIT:' in x or 'CJRT-DECLARED:' in x]}
arms=['green','retire','consumer','cut','restored']
tests=['P1Mark.PinnedMarkStartRetiresAllocationPage', 'P1Mark.PinnedReclaimedSlotIsNotAllocationSource',
       'P1Mark.DuplicateAnyThreadStopsAtConsumer', 'YoungWeakClosure.SingleWorkerKeepsYoungReferentStrong']
main=r/'unit-testable/cj_gc_unit';publication=r/'unit-testable/cj_gc_forwarding_publication_unit'
(out/'uptime-before.txt').write_bytes(subprocess.check_output(['uptime']))
children=[]
for arm in arms:
 product=(r if arm in ['green','restored'] else Path('/root')/(lane+'-'+arm))/'testable/build/runtime-staging/lib/x86_64_Release'
 d=out/arm;d.mkdir(exist_ok=True)
 doc=identity(product,[main,publication]);doc['cpus']=sorted(os.sched_getaffinity(0));(d/'identity.json').write_text(json.dumps(doc,indent=2))
 env=os.environ.copy();env.update(LD_LIBRARY_PATH=str(product),MRT_TESTABLE_INTERNALS='1',GC_UNIT_JOBS='192')
 commands=[]
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
print(json.dumps(results,indent=2))
