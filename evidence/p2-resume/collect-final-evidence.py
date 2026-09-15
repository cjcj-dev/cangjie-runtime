from pathlib import Path
import hashlib,json,re,subprocess
r=Path('/root/sym_cangjie_runtime_607_implement_r5684610492-build8')
d={'arms':{},'units':{}}
for p in sorted(r.glob('qualified-*/*/result.json')):
 o=p.parent; d['arms'][str(o.relative_to(r))]={'result':json.loads(p.read_text()),'files':{q.name:q.read_text() for q in o.iterdir() if q.suffix in ('.sha256','.txt','.rc') or q.name.endswith('.wall')}}
for a in ('default','filler','testable'):
 o=r/('unit-final-'+a);s=(o/'run.log').read_text();d['units'][a]={'rc':int((o/'run.rc').read_text()),'tail':s.splitlines()[-15:],'files':{q.name:q.read_text() for q in o.iterdir() if q.suffix in ('.sha256','.txt','.rc')}}
d['ohos']={q.name:q.read_text() for q in (r/'ohos/evidence').iterdir() if q.suffix in ('.rc','.txt')}
patterns=('MarkFromOldSlowPath','MarkFinalizableFromOldSlowPath','MarkFromYoungSlowPath','DiscoverFinalizableRoot','RescanRememberedSet','p2SlowFieldInputExercise','p2FieldBarrierExercise')
for name,p in [('product',r/'testable/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so'),('test',r/'slow-artifacts/libp2_field_barrier.so')]:
 out=subprocess.run(['nm','--defined-only','-C',str(p)],text=True,capture_output=True);(r/(name+'-defined-nm.txt')).write_text(out.stdout)
 d[name+'_symbols']={'nm_rc':out.returncode,'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'matches':{s:[x for x in out.stdout.splitlines() if s in x] for s in patterns}}
(r/'final-evidence.json').write_text(json.dumps(d,indent=2)+'\n')
print('arms',len(d['arms']),'units',{k:v['rc'] for k,v in d['units'].items()})
