import subprocess,os,time,json,hashlib,re
from pathlib import Path
lane='sym_cangjie_runtime_606_implement_r5674249495'
r=Path('/root')/(lane+'-final2');out=r/'p1-four-arms';out.mkdir(exist_ok=True)
elf=r/'p1-managed/p1_mark_start';observer=r/'p1-managed/libp1_mark_start.so'
source=r/'testable/runtime/tests/gc_unit/test_p1_mark_start.cpp'
callback=source.read_text().split('TracingCollector::testMarkStartState =')[1].split('    const auto youngBefore')[0]
expected=sorted(set(name for call in re.findall(r'Expect\([\s\S]*?\);',callback) for name in re.findall(r'"([a-z_]+)"',call)))
(out/'expected-phase-assertions.json').write_text(json.dumps(expected,indent=2))
(out/'uptime-before.txt').write_bytes(subprocess.check_output(['uptime']))
children=[]
for arm in ['green','cut-old-order','cut-young-order','restored']:
 product=(r if arm in ['green','restored'] else Path('/root')/(lane+'-final2-'+arm))/'testable/build/runtime-staging/lib/x86_64_Release'
 for n,cpus in enumerate(['0-31','32-63','96-127']):
  d=out/arm/str(n);d.mkdir(parents=True,exist_ok=True)
  files=[elf,observer,product/'libcangjie-runtime.so',product/'libboundscheck.so']
  identity={'files':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},'cpus':cpus,'time':time.time(),
            'stamp':[s for s in subprocess.check_output(['strings',str(files[2])],text=True).splitlines() if 'CJRT-COMMIT:' in s or 'CJRT-DECLARED:' in s]}
  (d/'identity.json').write_text(json.dumps(identity,indent=2))
  env=os.environ.copy();env['LD_LIBRARY_PATH']=str(elf.parent)+':'+str(product)+':/root/.cjv/toolchains/gate-colored-23e45a2e/runtime/lib/linux_x86_64_cjnative';env['cjGCInterval']='3600s'
  cmd=['taskset','-c',cpus,'timeout','60',str(elf)]
  log=(d/'run.log').open('wb');start=time.monotonic();p=subprocess.Popen(cmd,env=env,stdout=log,stderr=subprocess.STDOUT)
  children.append((arm,n,cmd,p,log,start,d))
rows=[]
for arm,n,cmd,p,log,start,d in children:
 rc=p.wait();log.close();s=(d/'run.log').read_text(errors='replace')
 observations=re.findall(r'^P1_ASSERT ([a-z_]+) (PASS|FAIL)$',s,re.M)
 seen={name:[status for key,status in observations if key==name] for name in expected}
 missing=[name for name,values in seen.items() if not values]
 failed=[name for name,values in seen.items() if 'FAIL' in values]
 row={'arm':arm,'n':n,'command':cmd,'rc':rc,'wall':time.monotonic()-start,'phase_assertions':seen,'missing':missing,'failed':failed,
      'phase_results':re.findall(r'^P1_MARK_START_PHASE_RESULT.*$',s,re.M),
      'post_phase_failure':[line for line in s.splitlines() if '[LOADFC][fail-closed]' in line]}
 rows.append(row);(d/'result.json').write_text(json.dumps(row,indent=2));(d/'rc').write_text(str(rc)+'\n')
 print(arm,n,'process_rc=',rc,'phase_failed=',failed,'phase_missing=',missing,'post_phase_LOADFC=',len(row['post_phase_failure']),flush=True)
(out/'results.json').write_text(json.dumps(rows,indent=2));(out/'uptime-after.txt').write_bytes(subprocess.check_output(['uptime']))
