import json,re,hashlib,subprocess
from pathlib import Path
r=Path('/root/sym_cangjie_runtime_606_implement_r5673376405-accept')
rows={}
logs={x:r/f'unit-{x}.log' for x in ['default','testable','filler']}
logs.update({x:r/'control-arms'/x/'suite.log' for x in ['cut-alloc','cut-retire','cut-consumer','cut-pinned','restored']})
for arm,p in logs.items():
 s=p.read_text(errors='replace');counts=re.findall(r'\[========\] (\d+) tests: (\d+) passed, (\d+) failed',s)
 failed=sorted(set(re.findall(r'\[  FAILED  \] ([A-Za-z_]\w*\.\S+)',s)))
 if not failed: failed=sorted(set(re.findall(r'\[  FAIL  \] ([A-Za-z_]\w*\.\S+)',s)))
 rcpath=r/f'unit-{arm}.rc' if arm in ['default','testable','filler'] else p.parent/'rc.txt'
 rows[arm]={'rc':int(rcpath.read_text()),'counts':list(map(int,counts[-1])),'failed':failed,'log':str(p)}
base=set(rows['restored']['failed'])
for arm in ['cut-alloc','cut-retire','cut-consumer','cut-pinned']:
 rows[arm]['new_failures']=sorted(set(rows[arm]['failed'])-base)
 rows[arm]['lost_failures']=sorted(base-set(rows[arm]['failed']))
rows['focused']=json.loads((r/'focused/results.json').read_text())
rows['identities']={arm:json.loads((r/'focused'/arm/'identity.json').read_text()) for arm in ['green','cut-alloc','cut-retire','cut-consumer','cut-pinned','restored']}
rows['uptime']={k:(r/k).read_text() for k in ['tests-uptime-before.txt','tests-uptime-after.txt','focused/uptime-before.txt','focused/uptime-after.txt','control-arms/uptime-before.txt','control-arms/uptime-after.txt']}
(r/'evidence/summary.json').write_text(json.dumps(rows,indent=2))
print(json.dumps({k:{x:v[x] for x in ['rc','counts','new_failures'] if x in v} for k,v in rows.items() if k in logs},indent=2))
