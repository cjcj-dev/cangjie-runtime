import subprocess,json,hashlib,os,shutil,time
from pathlib import Path
r=Path('/root/sym_cangjie_runtime_606_implement_r5675167676-final')
out={}
for name,p in {'product':r/'testable/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so','elf':r/'unit-testable/cj_gc_unit'}.items():
 proc=subprocess.run(['nm','--defined-only',str(p)],capture_output=True,text=True)
 dem=subprocess.run(['c++filt'],input=proc.stdout,capture_output=True,text=True)
 (r/(name+'-defined.txt')).write_text(dem.stdout)
 keys=['MObject::NewPinnedObject(', 'RegionManager::RetireSharedPages(', 'TracingCollector::DoTracing(', ' main']
 out[name]={'rc':proc.returncode,'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'matches':{k:[x for x in dem.stdout.splitlines() if k in x] for k in keys}}
(r/'symbols.json').write_text(json.dumps(out,indent=2))
manifest=[]
for arm in ['final','retire','consumer','cut']:
 for shape in ['default','testable']:
  p=Path('/root/sym_cangjie_runtime_606_implement_r5675167676-'+arm)/shape/'build/runtime-staging/lib/x86_64_Release'
  sha=hashlib.sha256((p/'libcangjie-runtime.so').read_bytes()).hexdigest()
  d=Path('/root/sodepot')/sha;d.mkdir(exist_ok=True)
  for name in ['libcangjie-runtime.so','libboundscheck.so']:
   dst=d/name
   if not dst.exists():shutil.copy2(p/name,dst)
   assert dst.read_bytes()==(p/name).read_bytes()
  manifest.append({'arm':arm,'shape':shape,'source':str(p),'depot':str(d),'runtime':sha,'bounds':hashlib.sha256((p/'libboundscheck.so').read_bytes()).hexdigest()})
(r/'sodepot.json').write_text(json.dumps(manifest,indent=2))
print(json.dumps(out,indent=2))
