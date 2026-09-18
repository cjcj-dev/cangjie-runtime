from pathlib import Path
import re,json
mark='VisitStrongPlainRoots DiscoverWeakReference MarkOldObjectIfActive EnumAllCommonRoots EnumAllExportRoots DiscoverFinalizableRoot MergeMutatorRoots DoEnumeration VisitStaticRoots EnumRefFieldRoot ProcessFinalizers VisitMinorRootSlots VisitMinorRoots PushYoungObject TraceYoungClosure TraceYoungClosureStriped FollowYoungMark TryEndYoungMark'.split()
reloc='ForwardFromSpace RefineFromSpace ForwardObject ForwardObjectExclusive IsFromObject IsUnmovableFromObject ResolveMinorReference FixMinorEvacuatedSlot FixMinorRootSlots RemapYoungRoots Preforward StartRelocationTasks'.split()
barrier='GetAndTryTagObj TryUpdateRefField TryUpdateRefFieldImpl CasInstallResolvedTarget'.split()
phase='testCyclePrepared testYoungMarkStarted testOldMarkStarted testMarkStartState testYoungMarkCompleted'.split()
markhooks='testColoredRootResult testOldMarkThreadResult'.split()
receiver=r'(?:const_cast<HeapGcState&>\(collector\)|(?:[A-Za-z_]\w*\.)*(?:collector|tracing|productCollector))'
changed=[]
for p in Path('runtime/tests/gc_unit').rglob('*'):
 if p.suffix not in ('.cpp','.hpp','.h'):continue
 s=p.read_text();old=s
 for owner,methods in [('ZMark',mark+markhooks),('ZRelocate',reloc),('ZBarrier',barrier),('ZGeneration',phase)]:
  for method in methods:
   s=re.sub(receiver+r'(?:\.|->)'+method+r'\b',owner+'::'+method,s)
   s=s.replace('HeapGcState::'+method,owner+'::'+method)
 s=re.sub(receiver+r'\.IsGhostFromObject\(',r'ZRelocate::IsFromObject(',s)
 s=re.sub(receiver+r'\.IsMarkedObject<',r'RegionSpace::IsMarkedObject<',s)
 s=re.sub(receiver+r'\.IsResurrectedObject\(',r'RegionSpace::IsResurrectedObject(',s)
 s=re.sub(receiver+r'\.MajorMark\(\)',r'Heap::GetHeap().old().MarkPtr()',s)
 s=re.sub(receiver+r'\.YoungMark\(\)',r'Heap::GetHeap().young().MarkPtr()',s)
 s=re.sub(receiver+r'\.NewWorkStack\(\)',r'WorkStack{}',s)
 s=re.sub(receiver+r'\.GetWorkers\((ZGenerationId::\w+)\)',r'*Heap::GetHeap().GetZGeneration(\1).Workers()',s)
 s=re.sub(receiver+r'\.PostTrace\(\)',r'Heap::GetHeap().old().PostTrace()',s)
 for gen in ('young','old'):
  s=re.sub(receiver+r'\.DoGarbageCollection\(ZGenerationId::'+gen+r'\)',f'Heap::GetHeap().{gen}().collect()',s)
 s=s.replace('collector.DoGarbageCollection(major ? ZGenerationId::old : ZGenerationId::young);','if (major) {\n            Heap::GetHeap().old().collect();\n        } else {\n            Heap::GetHeap().young().collect();\n        }')
 for n,typ in [('MinorSlotSet','std::unordered_set<MAddress>'),('MinorObjectSet','std::unordered_set<BaseObject*>'),('MinorRegionSet','std::unordered_set<ZPage*>'),('MinorInteriorBaseMap','std::unordered_map<MAddress, BaseObject*>')]:s=s.replace('HeapGcState::'+n,typ)
 s=s.replace('HeapGcState::RefSlotKind','ZBarrier::RefSlotKind')
 s=s.replace(' : HeapGcState {',' {')
 s=s.replace('.GetCollector()','')
 s=re.sub(r'\bHeapGcState\b', 'Heap', s)
 # The old RescanRememberedSet exists only in historical fixture comments.
 s=s.replace('Heap::RescanRememberedSet','ZRemembered::scan_and_follow')
 if s!=old:
  needs=[]
  for sym,h in [('ZRelocate::','Heap/z/zRelocate.hpp'),('ZBarrier::','Heap/z/zBarrier.hpp')]:
   if sym in s and '#include "'+h+'"' not in s:needs.append('#include "'+h+'"\n')
  if needs:
   incs=list(re.finditer(r'^#include [^\n]+\n',s,re.M));pos=incs[-1].end();s=s[:pos]+''.join(needs)+s[pos:]
  p.write_text(s);changed.append(str(p))
print('\n'.join(changed));Path('.lane-parallel/final-test-owner-files.json').write_text(json.dumps(changed,indent=2)+'\n')
