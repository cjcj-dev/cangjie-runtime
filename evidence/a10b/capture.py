from pathlib import Path
import subprocess, json, re, shlex
root=Path.cwd(); out=root/'evidence/a10b'
base='3c3216a7293b28690411a6283c7fd74076bdcd1b'
head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
main=subprocess.check_output(['git','rev-parse','cjcjdev/main'],text=True).strip()
def run(args):
 r=subprocess.run(args,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 return {'command':shlex.join(args),'rc':r.returncode,'output':r.stdout}
symbols=['isGCActive','gcMainThread','gcTid','criticalNum','isHeapMarked','gcWorking','YoungPreludeRequest','youngPreludeRequest','ActiveCycle','activeCycle','SelectCycle','GetCycleReason','ActiveForwardingGeneration','driverRequestActive','CollectorResources::RunTaskLoop','GCMainThreadEntry','PostIgnoredGcRequest','HasSyncTaskCompleted','finishedGcIndex','isGcStarted','currentTagID','FlipTagID','GetCurrentTagID','TaskQueue<GCExecutor>* taskQueue','GCWorkers* youngWorkers','GCWorkers* oldWorkers','GetGCPhase()','MRT_GCV2_MINOR_DEFERS_HEU']
rows=[]
for symbol in symbols:
 rows.append({'symbol':symbol,'positive_control':run(['git','grep','-n','-F',symbol,base,'--','runtime/src']),'candidate':run(['git','grep','-n','-F',symbol,'HEAD','--','runtime/src'])})
(out/'deletions.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2)+'\n')
with (out/'deletions.txt').open('w') as f:
 for row in rows:
  f.write('\nSYMBOL: '+row['symbol']+'\n')
  for label in ['positive_control','candidate']:
   r=row[label];f.write(label+'\n$ '+r['command']+'\n'+r['output']+'rc='+str(r['rc'])+'\n')
features=['HandshakeTimeout','FollowArrayElements','FollowPartialReferences','RecordMajorGCFinish','FORWARDING_FACE_RESET_BIT','MarkFaceMatchesOwner','TryRecoverInteriorBase']
preserved=[]
for symbol in features:
 a=run(['git','grep','-c','-F',symbol,main,'--','runtime/src']);b=run(['git','grep','-c','-F',symbol,'HEAD','--','runtime/src'])
 count=lambda x:sum(int(line.rsplit(':',1)[1]) for line in x['output'].splitlines() if line.rsplit(':',1)[1].isdigit())
 preserved.append({'symbol':symbol,'main':a,'candidate':b,'main_count':count(a),'candidate_count':count(b)})
(out/'main-content.json').write_text(json.dumps(preserved,ensure_ascii=False,indent=2)+'\n')
def test_names(ref):
 r=run(['git','grep','-h','-E',r'GC_(OTHER_VM_)?TEST\(',ref,'--','runtime/tests'])
 return set('.'.join(m) for m in re.findall(r'GC_(?:OTHER_VM_)?TEST\(\s*(\w+)\s*,\s*(\w+)',r['output']))
tests={}
for ref in [base,main]:
 before=test_names(ref);after=test_names('HEAD');tests[ref]={'added':sorted(after-before),'removed':sorted(before-after)}
(out/'test-set-diff.json').write_text(json.dumps(tests,ensure_ascii=False,indent=2)+'\n')
checks=[run(['git','diff','--check',main]),run(['git','grep','-n','-E',r'^(<<<<<<< |=======|>>>>>>> )','HEAD','--','runtime/src'])]
(out/'source-checks.json').write_text(json.dumps({'frozen':base,'main':main,'head':head,'runtime_tree':subprocess.check_output(['git','rev-parse','HEAD:runtime'],text=True).strip(),'checks':checks},indent=2)+'\n')
print('deletions',[(r['symbol'],r['positive_control']['rc'],r['candidate']['rc']) for r in rows])
print('preserved',[(r['symbol'],r['main_count'],r['candidate_count']) for r in preserved])
print('tests vs current main',tests[main])
