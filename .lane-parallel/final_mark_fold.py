from pathlib import Path
import re
r=Path('runtime/src/Heap/z');p=r/'zMark.hpp';h=p.read_text();names='VisitStrongPlainRoots DiscoverWeakReference MarkOldObjectIfActive EnumAllCommonRoots EnumAllExportRoots DiscoverFinalizableRoot MergeMutatorRoots DoEnumeration VisitStaticRoots EnumRefFieldRoot ProcessFinalizers VisitMinorRootSlots VisitMinorRoots PushYoungObject TraceYoungClosure TraceYoungClosureStriped FollowYoungMark TryEndYoungMark'.split();cls=h[h.index('class HeapGcState {'):];decl=[]
for name in names:
 matches=list(re.finditer(r'^    (?:void|bool) '+name+r'\([^;]*?;',cls,re.M))
 for m in matches:decl.append(m.group(0).replace('    ','    static ',1).replace(') const;',');').replace('MinorSlotSet','std::unordered_set<MAddress>'))
h=h[:h.index('class HeapGcState {')]+h[h.index('};\n} // namespace MapleRuntime',h.index('class HeapGcState {'))+3:]
h=h.replace('class ZMark {\n    friend class ZMarkTask;\npublic:','class ZMark {\n    friend class ZMarkTask;\npublic:\n'+'\n'.join(decl)+'''\n#if defined(MRT_TESTABLE_INTERNALS)
    static std::function<void(ZGenerationId, NativeSlot*)> testColoredRootResult;
    static std::function<void(Mutator&)> testOldMarkThreadResult;
#endif
''',1);h=h.replace('class Mutator;','class Mutator;\nstruct YoungConcWindowStats;',1);p.write_text(h)
# extract standalone function with balanced braces
p=r/'zMark.cpp';s=p.read_text();a=s.index('BaseObject* HeapGcState::GetAndTryTagObj(');b=s.index('{',a);n=1;i=b+1
while n:
 if s[i]=='{':n+=1
 elif s[i]=='}':n-=1
 i+=1
s=s[:a]+s[i:];p.write_text(s)
for fn in ['zMark.cpp','zRootsIterator.cpp']:
 p=r/fn;s=p.read_text()
 # captures this are only removed in old-owner extracted functions
 for name in names:
  start=0
  while True:
   m=re.search(r'^(?:void|bool) HeapGcState::'+name+r'\(',s[start:],re.M)
   if not m:break
   a=start+m.start();b=s.index('{',a);n=1;i=b+1
   while n:
    if s[i]=='{':n+=1
    elif s[i]=='}':n-=1
    i+=1
   body=s[a:i].replace('HeapGcState::'+name,'ZMark::'+name,1).replace(') const\n',')\n',1)
   body=re.sub(r'\[this,\s*', '[',body).replace(', this]',']').replace('[this]','[]')
   s=s[:a]+body+s[i:];start=a+len(body)
 s=s.replace('MinorSlotSet','std::unordered_set<MAddress>')
 s=s.replace('HeapGcState::testColoredRootResult','ZMark::testColoredRootResult').replace('HeapGcState::testOldMarkThreadResult','ZMark::testOldMarkThreadResult')
 s=s.replace('GetAndTryTagObj(', 'ZBarrier::GetAndTryTagObj(').replace('RefSlotKind::','ZBarrier::RefSlotKind::')
 s=s.replace('(void)DiscoverReference(reference, ReferenceType::WEAK);','(void)Heap::GetHeap().GetFinalizerProcessor().GetReferenceProcessor().DiscoverReference(reference, ReferenceType::WEAK);')
 s=s.replace('IsMarkedObject<Generation::Old>(obj)','RegionSpace::IsMarkedObject<Generation::Old>(obj)')
 s=re.sub(r'GetWorkers\((ZGenerationId::\w+)\)',r'(*Heap::GetHeap().GetZGeneration(\1).Workers())',s)
 s=s.replace('Heap::GetHeap().GetCollector().ResolveMinorReference(', 'ZRelocate::ResolveMinorReference(')
 s=re.sub(r'(?<![\w:.])ResolveMinorReference\(', 'ZRelocate::ResolveMinorReference(',s)
 s=s.replace('    auto& collector = static_cast<HeapGcState&>(Heap::GetHeap().GetCollector());\n','').replace('collector.DiscoverWeakReference(', 'ZMark::DiscoverWeakReference(')
 s=s.replace('HeapGcState::ScrubMinorFreeTarget','ZMark::ScrubMinorFreeTarget')
 p.write_text(s)
print('migrated declarations',len(decl))
