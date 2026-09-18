from pathlib import Path
import subprocess,re,textwrap
base='379aae5a8'
def source(p):return subprocess.check_output(['git','show',base+':'+p],text=True)
def bodyspan(s, pattern):
 m=re.search(pattern,s,re.M);assert m,pattern
 start=m.start();b=s.index('{',m.end()-1);depth=0
 for t in re.finditer(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',s[b:]):
  if t[0]=='{':depth+=1
  elif t[0]=='}':
   depth-=1
   if depth==0:return start,b+t.end()
 raise Exception(pattern)
def cpp(s,name):return bodyspan(s,r'^[^\n;{}]*HeapGcState::'+name+r'\([^;]*?\)\s*(?:const\s*)?\{')
def member(s,name):return bodyspan(s,r'^    (?:void|BaseObject\*) '+name+r'\([^;]*?\)\s*(?:const\s*)?\{')
mark=['VisitStrongPlainRoots','DiscoverWeakReference','MarkOldObjectIfActive','EnumAllCommonRoots','EnumAllExportRoots','DiscoverFinalizableRoot','MergeMutatorRoots','DoEnumeration','VisitStaticRoots','EnumRefFieldRoot','ProcessFinalizers','VisitMinorRootSlots','VisitMinorRoots','PushYoungObject','TraceYoungClosure','TraceYoungClosureStriped','FollowYoungMark','TryEndYoungMark','PublishHandshakeMarkWork','DrainAllocBufferMarkProducers','PublishThreadRoot','FlushThreadMarkProducers','FlushGCDataMarkProducers']
reloc=['ForwardFromSpace','RefineFromSpace','ForwardObject','ForwardObjectExclusive','IsFromObject','IsUnmovableFromObject','ResolveMinorReference','FixMinorEvacuatedSlot','FixMinorRootSlots','RemapYoungRoots','Preforward','StartRelocationTasks','ResolveStoreValue','FindToVersion']
barrier=['GetAndTryTagObj','TryUpdateRefField','TryUpdateRefFieldImpl','CasInstallResolvedTarget','ValidateCurrentValue','JudgeHandOutTarget','FailClosedLoad','GetAndTryTagRefField','GetAndTryTagRefFieldWithProvenance','CheckStoreGoodTarget']
phasehooks=['testCyclePrepared','testYoungMarkStarted','testOldMarkStarted','testMarkStartState','testYoungMarkCompleted']
receiver=r'(?:Heap::GetHeap\(\)\.GetCollector\(\)|Heap::GetHeap\(\)|TheCollector\(\)|GetCollector\(\)|\bcollector)\s*(?:\.|->)'
def routes(s):
 for owner,names in [('ZMark',mark),('ZRelocate',reloc),('ZBarrier',barrier),('ZGeneration',phasehooks),('ZMark',['testColoredRootResult','testOldMarkThreadResult']),('ZStat',['UpdateGCStats'])]:
  for name in names:
   s=s.replace('HeapGcState::'+name,owner+'::'+name)
   s=re.sub(receiver+name+r'\b',owner+'::'+name,s)
 s=re.sub(receiver+'IsGhostFromObject'+r'\b','ZRelocate::IsFromObject',s)
 s=re.sub(receiver+'IsMarkedObject'+r'\b','RegionSpace::IsMarkedObject',s)
 s=re.sub(receiver+'IsResurrectedObject'+r'\b','RegionSpace::IsResurrectedObject',s)
 s=re.sub(receiver+'MajorMark'+r'\(\)','Heap::GetHeap().old().MarkPtr()',s)
 s=re.sub(receiver+'YoungMark'+r'\(\)','Heap::GetHeap().young().MarkPtr()',s)
 return s
hbase=source('runtime/src/Heap/z/zMark.hpp')
heapadd=[]
for name in ['AddRawPointerObject','PinRawPointerObject','RemoveRawPointerObject']:
 a,b=member(hbase,name);part=textwrap.dedent(hbase[a:b]);part=re.sub(r'^(void|BaseObject\*) '+name,r'\1 Heap::'+name,part)
 heapadd.append(routes(part))
# Actual phase-owned operations, not another collector facade.
genadd=[]
for name in ['CollectLargeGarbage','CollectPinnedGarbage']:
 a,b=member(hbase,name);part=textwrap.dedent(hbase[a:b]).replace('void '+name,'void ZGenerationOld::'+name);genadd.append(routes(part))
rbase=source('runtime/src/Heap/z/zRelocate.cpp');a,b=cpp(rbase,'EvacuateYoungRegions');part=rbase[a:b].replace('HeapGcState::EvacuateYoungRegions','ZGenerationYoung::EvacuateYoungRegions')
part=part.replace('GetWorkers(ZGenerationId::young)','*Workers()')
for name in ['FixMinorEvacuatedSlot','FixMinorRootSlots','ForwardFromSpace']:
 part=re.sub(r'(?<![:\w])'+name+r'\(', 'ZRelocate::'+name+'(',part)
genadd.append(routes(part))
# Heap debug methods remain in the existing heap translation unit.
tbase=source('runtime/src/Heap/z/zTracing.cpp')
for name in ['DumpHeap','DumpRoots','DumpBeforeGC','DumpAfterGC']:
 a,b=cpp(tbase,name);part=tbase[a:b].replace('HeapGcState::'+name,'Heap::'+name)
 part=re.sub(r'(?<![:\w])IsMarkedObject<','RegionSpace::IsMarkedObject<',part)
 part=re.sub(r'(?<![:\w])IsResurrectedObject\(','RegionSpace::IsResurrectedObject(',part)
 part=re.sub(r'(?<![:\w])VisitStaticRoots\(', 'Heap::GetHeap().VisitStaticRoots(',part)
 heapadd.append('#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)\n'+routes(part)+'\n#endif')
p=Path('runtime/src/Heap/z/zHeap.cpp');s=p.read_text();s=re.sub(r'^.*collectorImpl.*\n','',s,flags=re.M)
s=s.replace('GetCollector().IsGhostFromObject(obj)','ZRelocate::IsFromObject(obj)').replace('GetCollector().IsUnmovableFromObject(obj)','ZRelocate::IsUnmovableFromObject(obj)').replace('GetCollector().ForwardObject(fromVersion, generation)','ZRelocate::ForwardObject(fromVersion, generation)')
s+='\nnamespace MapleRuntime {\n'+'\n\n'.join(heapadd)+'\n}\n';p.write_text(s)
p=Path('runtime/src/Heap/z/zHeap.hpp');s=p.read_text();s='\n'.join(l for l in s.splitlines() if not any(n in l for n in ['class HeapGcState;','GetCollector();','GetCollector() const;','collectorImpl']))+'\n'
s=s.replace('    void ResolveCycleRef();','''    void ResolveCycleRef();
    void AddRawPointerObject(BaseObject* obj);
    BaseObject* PinRawPointerObject(BaseObject* obj);
    void RemoveRawPointerObject(BaseObject* obj);
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    void DumpRoots(LogType logType);
    void DumpHeap(const CString& tag);
    void DumpBeforeGC();
    void DumpAfterGC();
#endif''');p.write_text(s)
p=Path('runtime/src/Heap/z/zGeneration.cpp');s=p.read_text()
for name in ['DoYoungGarbageCollection','DoGarbageCollection','ForwardFromSpace','RefineFromSpace']:
 a,b=cpp(s,name);s=s[:a]+s[b:]
a,b=bodyspan(s,r'^static HeapGcState& TheCollector\(\)\s*\{');s=s[:a]+s[b:]
s=s.replace('void HeapGcState::PreGarbageCollection(ZGenerationId generation, bool isConcurrent, uint64_t gcIndex)\n{','void ZGeneration::PreGarbageCollection(bool isConcurrent, uint64_t gcIndex)\n{\n    const ZGenerationId generation = id();')
s=s.replace('void HeapGcState::PostGarbageCollection(ZGenerationId generation, uint64_t gcIndex)\n{','void ZGeneration::PostGarbageCollection(uint64_t gcIndex)\n{\n    const ZGenerationId generation = id();')
s=s.replace('GetWorkers(generation)', '(*Workers())')
s=routes(s)
s=s.replace('TheCollector().PostTrace()', 'PostTrace()').replace('collector.CollectSmallSpace()', 'CollectSmallSpace()').replace('collector.EvacuateYoungRegions(', 'EvacuateYoungRegions(')
s=re.sub(r'^    HeapGcState& collector = TheCollector\(\);\n','',s,flags=re.M)
s=s.replace('    DumpBeforeGC();','    Heap::GetHeap().DumpBeforeGC();').replace('    DumpAfterGC();','    Heap::GetHeap().DumpAfterGC();')
s+='\nnamespace MapleRuntime {\n'+'\n\n'.join(genadd)+'\n#if defined(MRT_TESTABLE_INTERNALS)\n'
for line in tbase.splitlines():
 if any('HeapGcState::'+hook in line for hook in phasehooks):s+=line.replace('HeapGcState::','ZGeneration::')+'\n'
s+='#endif\n}\n';p.write_text(s)
p=Path('runtime/src/Heap/z/zGeneration.hpp');s=p.read_text().replace('class HeapGcState;\n','').replace('    friend class HeapGcState;\n','')
s=s.replace('    ZGenerationId id() const;','''    ZGenerationId id() const;
    void PreGarbageCollection(bool isConcurrent, uint64_t gcIndex);
    void PostGarbageCollection(uint64_t gcIndex);
#if defined(MRT_TESTABLE_INTERNALS)
    static std::function<void()> testCyclePrepared;
    static std::function<void()> testYoungMarkStarted;
    static std::function<void()> testOldMarkStarted;
    static std::function<void(ZGenerationId, MarkStartPoint, const ZMark*)> testMarkStartState;
    static std::function<void()> testYoungMarkCompleted;
#endif''')
s=s.replace('    ZGenerationYoung();','''    ZGenerationYoung();
    void EvacuateYoungRegions(const std::vector<BaseObject*>& reachableVec,
        const std::unordered_set<MAddress>& rememberedSlots, bool refFixSlotsCoveredByReachable,
        const std::unordered_map<MAddress, BaseObject*>& interiorBases,
        std::unique_ptr<ScopedStopTheWorld>* stw = nullptr);''')
s=s.replace('    ZGenerationOld();','''    ZGenerationOld();
    void PostTrace();
    void CollectSmallSpace();
    void CollectLargeGarbage();
    void CollectPinnedGarbage();''');p.write_text(s)
p=Path('runtime/src/Heap/z/zRelocationSet.cpp');s=p.read_text().replace('HeapGcState::PostTrace','ZGenerationOld::PostTrace').replace('HeapGcState::CollectSmallSpace','ZGenerationOld::CollectSmallSpace');s=routes(s)
s=re.sub(r'(?<![:\w])RefineFromSpace\(', 'ZRelocate::RefineFromSpace(',s)
s=re.sub(r'(?<![:\w])UpdateGCStats\(', 'ZStat::UpdateGCStats(',s);p.write_text(s)
p=Path('runtime/src/Heap/z/zStat.hpp');s=p.read_text().replace('class ZStat final : public ZThread {','class ZStat final : public ZThread {\npublic:\n    static void UpdateGCStats();');p.write_text(s)
p=Path('runtime/src/Heap/z/zStat.cpp');s=p.read_text().replace('HeapGcState::UpdateGCStats','ZStat::UpdateGCStats');p.write_text(s)
p=Path('runtime/src/Heap/z/zDriver.cpp');s=p.read_text().replace('Heap::GetHeap().GetCollector().PreGarbageCollection(generation, reason != GC_REASON_YOUNG, gcIndex)','cycle.PreGarbageCollection(reason != GC_REASON_YOUNG, gcIndex)').replace('Heap::GetHeap().GetCollector().PostGarbageCollection(generation, gcIndex)','cycle.PostGarbageCollection(gcIndex)');p.write_text(s)
# Remove the old tracing translation unit after all mechanisms found owners.
Path('runtime/src/Heap/z/zTracing.cpp').unlink()
p=Path('runtime/src/Heap/CMakeLists.txt');s=p.read_text().replace('    "z/zTracing.cpp"\n','');p.write_text(s)
