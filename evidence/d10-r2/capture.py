from pathlib import Path
import subprocess,json,re,hashlib,shlex
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'evidence/d10-r2'
BASE='78fc9ce028de705b3ea705b7069759c1036a2796'
def run(*args):
    p=subprocess.run(args,cwd=ROOT,text=True,capture_output=True)
    return {'command':shlex.join(args),'rc':p.returncode,'stdout':p.stdout,'stderr':p.stderr}
head=run('git','rev-parse','HEAD')['stdout'].strip()
main=run('git','rev-parse','cjcjdev/main')['stdout'].strip()
symbols='MarkGoodHeapGate PlausibleManagedObjectGate TryRecoverInteriorBase ManagedObjectGate.h kMinPlausibleTypeInfoAddr TipLow32IsZero ObjectFitsInRegion TipWordLooksLikeTypeInfo ClassifyInteriorOffset RecoverInteriorBaseImpl ToHeaderCovered PushHeapRootIfPlausible RetainedLiveInfoState IsRetainedLifeCurrent GetRetainedLiveInfo HasEverPreservedRetainedLiveInfo GetRetainedLiveInfoEpoch GetRetainedLiveInfoCoveredUpTo StampRetainedSnapshot RetainedOwnCopyEnabled CaptureRetainedMarkWords HasRetainedMarkWords RetainedMarkWordsSay FreeRetainedMarkWords BeginRetainedPreserve PreserveRetainedLiveInfo PreserveRetainedLiveInfoUpTo NoteRetainedPreserve IsRetainedSnapshotValid FORWARDING_FACE_RESET_BIT IsForwardingFaceReset SetForwardingFaceReset ClearForwardingFaceReset retainedLiveInfo retainedEverPreserved retainedLiveInfoEpoch retainedLiveInfoCoveredUpTo retainedLifeId retainedPreserveCnt retainedMarkWords retainedMarkWordCnt IsForwardingFaceCurrent ResetMarkBit KeepRememberedHolder RememberedHolderPolicy.h holderIsCurrentMinorRoot preservedByCurrentRoot currentRootRanges g_fixinputReject g_fixinputRecover g_fixinputUnrecoverable'.split()
scans=[];lines=[]
for symbol in symbols:
    row={'symbol':symbol}
    for arm,ref in [('base',BASE),('candidate',head)]:
        entry=run('git','grep','-n','-F',symbol,ref,'--','runtime/src')
        row[arm]=entry
        lines += ['$ '+entry['command'],entry['stdout'].rstrip(),entry['stderr'].rstrip(),'rc='+str(entry['rc']),'']
    scans.append(row)
(OUT/'deletion-scan.json').write_text(json.dumps({'base':BASE,'code_head':head,'n':len(symbols),'scans':scans},ensure_ascii=False,indent=2)+'\n')
(OUT/'deletion-scan.txt').write_text('\n'.join(lines))
assert all(r['base']['rc']==0 and r['candidate']['rc']==1 for r in scans)
script=[]
for ref in [BASE,head]:
    script.append(run('git','ls-tree',ref,'--','tools/trustp1_static_harness.sh'))
    script.append(run('git','grep','-n','-F','MRT_GCV2_',ref,'--','tools'))
(OUT/'legacy-script-scan.json').write_text(json.dumps(script,ensure_ascii=False,indent=2)+'\n')
features=['CollectorResources::RunYoungCollection','CollectorResources::RunDriverLoop','GenerationCycle::StartYoungMark','GenerationCycle::Begin','class DriverLocker','class DriverUnlocker','class YoungTypeSetter', 'ConcurrentGCBreakpoints::At', 'Breakpoint::At', 'ConcurrentGCBreakpoints::RunTo', 'class ZGCIdPrinter', 'class ZGCIdMinor', 'class ZGCIdMajor']
checks=[]
for feature in features:
    row={'symbol':feature}
    for arm,ref in [('main',main),('candidate',head)]:
        row[arm]=run('git','grep','-c','-F',feature,ref,'--','runtime/src')
    counts=lambda e:sum(int(l.rsplit(':',1)[1]) for l in e['stdout'].splitlines())
    row['preserved']=counts(row['candidate'])>=counts(row['main'])>0
    assert row['preserved']
    checks.append(row)
(OUT/'main-content.json').write_text(json.dumps({'main':main,'code_head':head,'checks':checks},ensure_ascii=False,indent=2)+'\n')
pattern=re.compile(r'\bGC_(?:OTHER_VM_)?TEST\(\s*(\w+)\s*,\s*(\w+)\s*\)')
def tests(ref):
    names={}
    files=run('git','ls-tree','-r','--name-only',ref,'--','runtime/tests/gc_unit')['stdout'].splitlines()
    for name in files:
        if not name.endswith(('.cpp','.h','.hpp')):continue
        text=run('git','show',ref+':'+name)['stdout']
        for m in pattern.finditer(text):names[m[1]+'.'+m[2]]=f'{name}:{text.count(chr(10),0,m.start())+1}'
    return names
before=tests(BASE);after=tests(head)
(OUT/'test-set-diff.json').write_text(json.dumps({'base':BASE,'code_head':head,'ruler':'GC_TEST/GC_OTHER_VM_TEST name set across runtime/tests/gc_unit; source-only, not execution','added':{n:after[n] for n in sorted(after.keys()-before.keys())},'deleted':{n:before[n] for n in sorted(before.keys()-after.keys())},'kept':sorted(after.keys()&before.keys())},ensure_ascii=False,indent=2)+'\n')
main_tests=tests(main)
(OUT/'package-test-set-diff.json').write_text(json.dumps({'main':main,'code_head':head,'added':{n:after[n] for n in sorted(after.keys()-main_tests.keys())},'deleted':{n:main_tests[n] for n in sorted(main_tests.keys()-after.keys())}},ensure_ascii=False,indent=2)+'\n')
checks=[run('git','diff','--check',BASE,head),run('git','log','--format=%H %an <%ae> %s',BASE+'..'+head),run('git','diff','--name-status',main+'...'+head)]
(OUT/'source-checks.json').write_text(json.dumps(checks,ensure_ascii=False,indent=2)+'\n')
files=run('git','ls-tree','-r','--name-only',head,'--','runtime/src','runtime/tests','tools')['stdout'].splitlines()
identity={f:hashlib.sha256((ROOT/f).read_bytes()).hexdigest() for f in files if (ROOT/f).is_file()}
(OUT/'product-files.sha256').write_text(''.join(f'{h}  {f}\n' for f,h in identity.items()))
print(f'code_head={head} deletion_symbols={len(symbols)} added_tests={len(after.keys()-before.keys())} removed_tests={len(before.keys()-after.keys())} main_features={len(features)}')
