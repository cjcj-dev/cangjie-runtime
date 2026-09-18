from pathlib import Path
import re,json,subprocess
names='AbortUnimplemented ShouldIgnoreRequest EmitNeverInstalledDiagnostic EnumAndTagRawRoot EnumMutatorRoot FixMinorObjectSlots FlushAllocationRegions FlushMarkProducers RunOldCollection TryForwardRefField TryUntagRefField TryUpdateRefFieldWithProvenance VisitAllResurrectExportObjects IsOldPointer IsCurrentPointer ColourStaleLoadBad ColourResolvedRefField IsMarkedObjectForProbe CurrentRemapColourForProbe'.split()
def scan():
 out={}
 for n in names:
  p=subprocess.run(['rg','-n',n,'runtime'],capture_output=True,text=True)
  out[n]={'rc':p.returncode,'output':p.stdout}
 return out
before=scan()
# Mask strings/comments without changing offsets before balancing bodies.
def mask(s):
 return re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',lambda m:''.join('\n' if c=='\n' else ' ' for c in m[0]),s)
files=['zMark.hpp','zMark.cpp','zCollectedHeap.cpp','zCrossVM.cpp','zRelocate.cpp','zGeneration.cpp']
removed=[]
for f in files:
 p=Path('runtime/src/Heap/z')/f;s=p.read_text()
 for name in names:
  pattern=r'(?m)^ *(?:\[\[noreturn\]\] *)?(?:[\w:<>&*]+ +)+(?:HeapGcState::)?'+name+r'\('
  while True:
   m=re.search(pattern,s)
   if not m:break
   masked=mask(s)
   a=m.start();brace=masked.find('{',m.end());semi=masked.find(';',m.end())
   if semi!=-1 and (brace==-1 or semi<brace):end=semi+1
   else:
    assert brace!=-1,(f,name)
    depth=1;end=brace+1
    while depth:
     if masked[end]=='{':depth+=1
     elif masked[end]=='}':depth-=1
     end+=1
   if end<len(s) and s[end]=='\n':end+=1
   removed.append({'file':str(p),'method':name,'text':s[a:end]})
   s=s[:a]+s[end:]
 p.write_text(s)
p=Path('runtime/tests/gc_unit/gc_unit_stubs.cpp');p.unlink()
for f in ('runtime/tests/gc_unit/CMakeLists.txt','runtime/tests/gc_unit/run_standalone.sh'):
 p=Path(f);p.write_text(''.join(l for l in p.read_text().splitlines(True) if 'gc_unit_stubs.cpp' not in l))
p=Path('runtime/src/ObjectModel/MClass.h');s=p.read_text().replace('// HeapGcState::EnumAndTagRawRoot heals them with StorePlain, so a coloured write there is','// Plain root visitors heal them with StorePlain, so a coloured write there is');p.write_text(s)
p=Path('runtime/src/Heap/z/zMark.hpp');s=p.read_text().replace('after Flip their colour is IsOldPointer while the payload is','after Flip their colour is load-bad while the payload is');p.write_text(s)
p=Path('runtime/src/Heap/z/zGeneration.cpp');s=p.read_text().replace('// their holders are in reachableVec and will be scanned by FixMinorObjectSlots.','// its holder coverage belongs to the mark scan.');p.write_text(s)
Path('.lane-parallel/dead-methods.json').write_text(json.dumps({'scope':'runtime; raw-name search includes declarations, calls, mangled names and comments','before':before,'removed':removed,'after':scan()},indent=2)+'\n')
print('removed declarations/definitions',len(removed))
for n,v in scan().items():
 if v['rc']!=1:print(n,v)
