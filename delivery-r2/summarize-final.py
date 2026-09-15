from pathlib import Path
import json,re,subprocess,hashlib,time
r=Path('/root/sym_cangjie_runtime_606_implement_r5674249495-final2')
def read(p):return p.read_text(errors='replace')
def load(p):return json.loads(read(p))
def rc(p):return read(p).strip() if p.exists() else 'MISSING'
def marks(s):return [x for x in s.splitlines() if re.search(r'P1_|TARGET_YOUNG|Check failed: region->GetOwnerGeneration|EXPECT failed:',x)]
result={'root':str(r),'time':time.time(),'unit':{},'comparison':load(r/'comparison/suite-deltas.json'),'comparison_runs':load(r/'comparison/results.json'),'identities':{},'focused':{},'uptime':{}}
for arm in ['default','testable','filler']:
 text=read(r/f'unit-{arm}.log');count=re.findall(r'^\[========\] (\d+) tests: (\d+) passed, (\d+) failed$',text,re.M)
 result['unit'][arm]={'rc':rc(r/f'unit-{arm}.rc'),'counts':count[-1] if count else None,'log':str(r/f'unit-{arm}.log')}
for arm in result['comparison']:
 d=r/'comparison'/arm;result['identities'][arm]=load(d/'identity.json')
 result['focused'][arm]={p.name:{'rc':rc(p.with_suffix('.rc')),'evidence':marks(read(p))} for p in d.glob('*.log') if p.name!='suite.log'}
 result['comparison'][arm]['owner_check_count']=read(d/'suite.log').count('Check failed: region->GetOwnerGeneration() == gen')
for p in r.rglob('*uptime*.txt'):
 if 'build/' not in str(p):result['uptime'][str(p)]=read(p)
result['phase_expected']=load(r/'p1-four-arms/expected-phase-assertions.json')
result['phase_runs']=load(r/'p1-four-arms/results.json')
for row in result['phase_runs']:
 d=r/'p1-four-arms'/row['arm']/str(row['n']);row['identity']=load(d/'identity.json')
 row['mark_stale']=[x for x in read(d/'run.log').splitlines() if '[MARKSTALE]' in x]
result['original_managed']=[{'n':n,'rc':rc(r/f'managed-{n}.rc'),'exit_codes':re.findall(r'CYCLE_RC=\d+ SATB_RC=\d+',read(r/f'managed-{n}.log')),
 'failures':[x for x in read(r/f'managed-{n}.log').splitlines() if 'finalizable job predicate' in x or 'invalid other-vm child' in x]} for n in range(3)]
result['ohos']={k:rc(r/'ohos-cwdfix'/f'{k}.rc') for k in ['configure','build','unit']}
result['ohos']['identity']=read(r/'ohos-cwdfix/product.sha256')
result['ohos']['reason']=[x for x in read(r/'ohos-cwdfix/unit.log').splitlines() if 'fatal:' in x]
need=['MapleRuntime::RegionBitmap::MarkBits(', 'MapleRuntime::RegionBitmap::MarkFinalizableBits(',
      'MapleRuntime::GenerationCycle::StartYoungMark(', 'MapleRuntime::GenerationCycle::StartOldMark(',
      'MapleRuntime::WCollector::DoYoungGarbageCollection(', 'MapleRuntime::Collector::RequestGC(', 'p1MarkStartExercise',' main']
result['symbols']={}
for path in [r/'testable/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so',r/'unit-testable/cj_gc_unit',r/'p1-managed/libp1_mark_start.so',r/'p1-managed/p1_mark_start']:
 p=subprocess.run(['nm','--defined-only',str(path)],text=True,capture_output=True)
 dem=subprocess.run(['c++filt'],input=p.stdout,text=True,capture_output=True)
 result['symbols'][str(path)]={'command':['nm','--defined-only',str(path)],'rc':p.returncode,'matches':[x for x in dem.stdout.splitlines() if any(n in x for n in need)]}
result['compiler']={}
for path in [Path('/root/.cjv/toolchains/gate-colored-23e45a2e/bin/cjc'),Path('/root/.cjv/toolchains/gate-colored-23e45a2e/third_party/llvm/bin/llc')]:
 if path.exists():result['compiler'][str(path)]={'resolved':str(path.resolve()),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
(r/'final-evidence-summary.json').write_text(json.dumps(result,indent=2))
print('units',result['unit'])
print('cuts',[(k,v['counts'],len(v['new_failures']),v['owner_check_count']) for k,v in result['comparison'].items()])
print('phase',[(x['arm'],x['n'],x['rc'],x['failed'],x['missing']) for x in result['phase_runs']])
print('OHOS',result['ohos']['configure'],result['ohos']['build'],result['ohos']['unit'])
