import json, subprocess
from pathlib import Path
out=Path(__file__).parent
base='9418ed6f65cfc4f7202500c45634e4289d326d16'
def run(args):
 p=subprocess.run(args,text=True,capture_output=True);return {'command':args,'rc':p.returncode,'stdout':p.stdout,'stderr':p.stderr}
records={}
records['coordinates']=[run(['git','-C','/root/cj_build/cangjie_runtime','rev-parse','cjcjdev/main']),run(['git','rev-parse','HEAD']),run(['git','rev-parse',base+'^{tree}']),run(['git','rev-parse','HEAD:runtime']),run(['git','diff','--stat',base])]
records['deleted_owner_reads']=[]
for symbol,path in [('GetMutatorPhase','runtime/src/Heap/z/zObjectAllocator.cpp'),('GetMutatorPhase','runtime/src/Heap/z/zMark.cpp'),('GetMutatorPhase','runtime/src/Interpreter/InterpreterSpecific.cpp'),('Heap::GetHeap().IsGcStarted()','runtime/src/Heap/z/zObjectAllocator.cpp')]:
 records['deleted_owner_reads'].append({'symbol':symbol,'base':run(['git','grep','-n','-F',symbol,base,'--',path]),'candidate':run(['git','grep','-n','-F',symbol,'--',path])})
records['remaining_phase_consumers']=run(['git','grep','-n','GetMutatorPhase','--','runtime/src'])
records['finalizer_entry_chain']=run(['git','grep','-n','-E','OnFinalizerCreated|MarkNewObject','--','runtime/src'])
records['main_content']=[]
for symbol in ['HandshakeTimeout','FollowArrayElements','FollowPartialReferences','RecordMajorGCFinish','FORWARDING_FACE_RESET_BIT','MarkFaceMatchesOwner','TryRecoverInteriorBase']:
 records['main_content'].append({'symbol':symbol,'main':run(['git','grep','-c',symbol,'cjcjdev/main','--','runtime/src']),'head':run(['git','grep','-c',symbol,'HEAD','--','runtime/src'])})
records['test_diff']=run(['git','diff',base,'--','runtime/tests'])
records['switches']=[run(['git','grep','-n','MRT_GCV2_','HEAD','--','runtime/src']),run(['git','grep','-n','MRT_GCV2_','cjcjdev/main','--','runtime/src']),run(['git','grep','-n','MRT_GCV2_MINOR_DEFERS_HEU','3c3216a7293b28690411a6283c7fd74076bdcd1b','--','runtime/src'])]
(out/'source-evidence.json').write_text(json.dumps(records,ensure_ascii=False,indent=2)+'\n')

prior=json.loads(Path('evidence/a10b/deletions.json').read_text())
checks=[]
for item in prior:
 symbol=item['symbol']
 checks.append({'symbol':symbol,'positive_control':run(['git','grep','-n','-F',symbol,'3c3216a7293b28690411a6283c7fd74076bdcd1b','--','runtime/src']),'candidate':run(['git','grep','-n','-F',symbol,'HEAD','--','runtime/src'])})
(out/'deletion-recheck.json').write_text(json.dumps(checks,ensure_ascii=False,indent=2)+'\n')
print('deletion recheck:',len(checks),'candidate rc:',sorted(set(x['candidate']['rc'] for x in checks)),'positive rc:',sorted(set(x['positive_control']['rc'] for x in checks)))
