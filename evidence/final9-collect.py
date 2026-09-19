from pathlib import Path
import json,re
base=Path('/root/sym_cangjie_runtime_607_implement_r5739597397')
rows=[]
for root in [Path(str(base)+'-final/green'),base/'final9-cuts']:
 for p in sorted(root.rglob('*.log')):
  rc=p.with_suffix('.rc')
  if not rc.exists():continue
  text=p.read_text(errors='replace')
  hashes=[dict(sha=m.group(1),path=m.group(2)) for m in re.finditer(r'^([0-9a-f]{64})  (.+)$',text,re.M)]
  rows.append(dict(log=str(p),rc=rc.read_text().strip(),hashes=hashes,failed=re.findall(r'^P2_ASSERT (\S+) FAIL$',text,re.M),passed=re.findall(r'^P2_ASSERT (\S+) PASS$',text,re.M),results=[x for x in text.splitlines() if re.match(r'^P2_.*RESULT ',x)],checks=[x for x in text.splitlines() if 'Check failed:' in x]))
identities=[]
cutroot=base/'final9-cuts'
for p in sorted((cutroot/'green').glob('*/run.log')):
 entry=p.parent.name
 green=next(r for r in rows if r['log']==str(p))
 gh=[x['sha'] for x in green['hashes']]
 for arm in ['entry','consumer','partial','final','strong','nonmajor','index','binding','restored']:
  row=next(r for r in rows if r['log']==str(cutroot/arm/entry/'run.log'))
  h=[x['sha'] for x in row['hashes']]
  identities.append(dict(arm=arm,entry=entry,four_hashes=len(gh)==4 and len(h)==4,test_and_bounds_same=len(h)==4 and h[:2]==gh[:2] and h[3]==gh[3],runtime_same=len(h)==4 and h[2]==gh[2]))
res=dict(candidate='9ca4e791f930af109a0cf66b127cfa045cd8fa5f',rows=rows,identities=identities)
(base/'final9-summary.json').write_text(json.dumps(res,indent=2))
for row in rows:print(row['log'],row['rc'],row['failed'])
print('IDENTITY', all(x['four_hashes'] and x['test_and_bounds_same'] and (x['runtime_same']==(x['arm']=='restored')) for x in identities))
