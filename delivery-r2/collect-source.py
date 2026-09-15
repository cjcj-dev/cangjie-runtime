from pathlib import Path
import subprocess,json,re,hashlib
root=Path.cwd();out=root/'delivery-r2';base='fb7d5282867aaa3b9d8b5df2f6691227ff3424ce'
head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
def run(args):
 p=subprocess.run(args,text=True,capture_output=True);return {'command':args,'rc':p.returncode,'stdout':p.stdout,'stderr':p.stderr}
records=[run(['git','-C','/root/cj_build/cangjie_runtime','rev-parse','cjcjdev/main']),run(['git','rev-parse','HEAD']),run(['git','branch','--show-current'])]
removed='markStartAllocPtr|InitializeAllocationWatermark|AllocatedAfterMarkStart|RegionIsAllocatingPage|ExemptMarkStartAllocatingFromCSet|AllocPinnedFromFreeList'
for ref in [base,head]: records.append(run(['git','grep','-n','-E',removed,ref,'--','runtime/src']))
(out/'source-checks-final.json').write_text(json.dumps(records,indent=2))
features=['ZIterator::','basic_oop_iterate_safe','MarkYoungGoodBarrierOnOopField','MarkYoungRootObject','MarkYoungRootsTask','PushHeapRoot','PublishThreadRoot','ColorAddressMarkYoungGood','ResolveCurrentValueRoot','VisitAllColoredRoots','RemapYoungRoots','PushHeaderlessRecord','VisitRawObjects']
features.extend(r['command'][5] for r in json.loads((root/'evidence/a2-main-features.json').read_text()) if len(r['command'])>6 and r['command'][4]=='-e')
rows=[]
for feature in sorted(set(features)):
 pair=[]
 for ref in [base,head]:
  row=run(['git','grep','-c','-F','-e',feature,ref,'--','runtime/src'])
  row['count']=sum(int(line.rsplit(':',1)[1]) for line in row['stdout'].splitlines())
  pair.append(row)
 rows.append({'feature':feature,'base':pair[0],'head':pair[1],'preserved':pair[1]['count']>=pair[0]['count']})
(out/'main-content-counts-final.json').write_text(json.dumps(rows,indent=2))
def test_names(ref):
 names={}
 files=subprocess.check_output(['git','ls-tree','-r','--name-only',ref,'--','runtime/tests/gc_unit'],text=True).splitlines()
 for f in files:
  if not f.endswith(('.cpp','.hpp')):continue
  s=subprocess.check_output(['git','show',ref+':'+f],text=True)
  for m in re.finditer(r'GC_(?:OTHER_VM_)?TEST\(\s*(\w+)\s*,\s*(\w+)\s*\)',s):names[m[1]+'.'+m[2]]=f+':'+str(s[:m.start()].count('\n')+1)
 return names
old,new=test_names(base),test_names(head)
(out/'test-name-delta-final.json').write_text(json.dumps({'base':base,'head':head,'added':{k:new[k] for k in new.keys()-old.keys()},'removed':{k:old[k] for k in old.keys()-new.keys()}},indent=2))
call=run(['rg','-n',r'MarkBits\(|MarkFinalizableBits\(|StartYoungMark\(|StartOldMark\(|GenerationSequenceFixture::|MarkYoungRootObject\(|MarkObjectIfActive<','runtime/src','runtime/tests/gc_unit'])
(out/'producer-consumers-final.json').write_text(json.dumps(call,indent=2))
files=subprocess.check_output(['git','diff','--name-only',base,head,'--','runtime/src'],text=True).splitlines()
(out/'product-source-hashes-final.json').write_text(json.dumps({f:hashlib.sha256((root/f).read_bytes()).hexdigest() if (root/f).exists() else 'deleted' for f in files},indent=2))
print('feature regressions',[(r['feature'],r['base']['count'],r['head']['count']) for r in rows if not r['preserved']])
print('test additions',sorted(new.keys()-old.keys()),'removals',sorted(old.keys()-new.keys()))
