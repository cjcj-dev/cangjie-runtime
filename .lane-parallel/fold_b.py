from pathlib import Path
import re, subprocess
root=Path('runtime/src/Heap/z')
def grab(s,name,cls='HeapGcState'):
    m=re.search(r'^(?:template<[^\n]+>\n)?[^\n]*\b'+cls+r'::'+name+r'\(',s,re.M)
    assert m,(name,cls)
    a=m.start(); o=s.index('{',m.end()); level=1;b=o+1
    while level:
        if s[b]=='{':level+=1
        elif s[b]=='}':level-=1
        b+=1
    return a,b,s[a:b]
def pop(s,name):
    a,b,v=grab(s,name);return s[:a]+s[b:],v
def static(v,owner):
    v=v.replace('HeapGcState::',owner+'::')
    o=v.index('{');v=v[:o].replace(') const',')')+v[o:]
    return v
s=(root/'zRelocate.cpp').read_text();bar=[]
for name in ['TryUpdateRefFieldImpl','TryUpdateRefField','CasInstallResolvedTarget']:
    s,v=pop(s,name);bar.append(static(v,'ZBarrier'))
s,_=pop(s,'EvacuateYoungRegions')
# Migrate existing methods without changing instance ZRelocate implementations.
for name in ['IsUnmovableFromObject','RemapYoungRoots','StartRelocationTasks','Preforward','ResolveMinorReference','FixMinorEvacuatedSlot','FixMinorRootSlots','ForwardObject','ForwardObjectExclusive']:
    while re.search(r'^.*\bHeapGcState::'+name+r'\(',s,re.M):
        a,b,v=grab(s,name);s=s[:a]+static(v,'ZRelocate')+s[b:]
s=s.replace('std::atomic<size_t> g_minorRefCasFail{ 0 };\n','').replace('std::atomic<size_t> g_minorRefCasOk{ 0 };\n','')
s=s.replace('HeapGcState* collector, BaseObject* obj','BaseObject* obj')
s=s.replace('collector->IsUnmovableFromObject(', 'ZRelocate::IsUnmovableFromObject(').replace('collector->IsGhostFromObject(', 'ZRelocate::IsFromObject(').replace('collector->IsFromObject(', 'ZRelocate::IsFromObject(')
s=s.replace('EnsureRouteDomainMembership(collector, obj)','EnsureRouteDomainMembership(obj)')
s=s.replace('EnsureRouteDomainMembership(&Heap::GetHeap().GetCollector(), obj)','EnsureRouteDomainMembership(obj)')
s=s.replace('EnsureRouteDomainMembership(const_cast<HeapGcState*>(this), target)','EnsureRouteDomainMembership(target)')
s=s.replace('ForceRootRouteDomainWhileForwardable(const_cast<HeapGcState*>(this), target)','ForceRootRouteDomainWhileForwardable(target)')
s=s.replace('const_cast<HeapGcState*>(this)->ForwardObject(', 'ZRelocate::ForwardObject(')
s=s.replace('IsGhostFromObject(', 'IsFromObject(')
s=s.replace('RootSlotWriteback(current, field)', 'ZBarrier::GetAndTryTagRefField(current)')
s=s.replace('CasInstallResolvedTarget(field,', 'ZBarrier::CasInstallResolvedTarget(field,')
s=s.replace('[this, stw]', '[stw]').replace('[this](ObjectRef& root)', '[](ObjectRef& root)')
s=s.replace('ForwardingHolderKind::StackSlot, this, &root', 'ForwardingHolderKind::StackSlot, &Heap::GetHeap().young().relocate(), &root')
s=s.replace('HeapGcState::FixMinorEvacuatedSlot', 'ZRelocate::FixMinorEvacuatedSlot')
s=s.replace('VisitStrongPlainRoots(', 'ZMark::VisitStrongPlainRoots(')
s=re.sub(r'GetWorkers\(([^)]*)\)',r'*Heap::GetHeap().GetZGeneration(\1).Workers()',s)
s=s.replace('RemapPromotedField(HeapGcState& collector, RefField<>& field, zpointer observed)', 'RemapPromotedField(RefField<>& field, zpointer observed)')
s=s.replace('    HeapGcState& collector = Heap::GetHeap().GetCollector();\n','')
s=s.replace('    HeapGcState& collector = reinterpret_cast<HeapGcState&>(Heap::GetHeap().GetCollector());\n','')
s=s.replace('RemapPromotedField(collector, field, observed)', 'RemapPromotedField(field, observed)').replace('RemapPromotedField(Heap::GetHeap().GetCollector(), field, observed)', 'RemapPromotedField(field, observed)')
s=s.replace('collector.ForwardObjectExclusive(', 'ZRelocate::ForwardObjectExclusive(')
baseline=subprocess.check_output(['git','show','379aae5a8:runtime/src/Heap/z/zGeneration.cpp'],text=True)
extra=[]
for name in ['ForwardFromSpace','RefineFromSpace']:
    _,_,v=grab(baseline,name);v=static(v,'ZRelocate');v=re.sub(r'GetWorkers\(([^)]*)\)',r'*Heap::GetHeap().GetZGeneration(\1).Workers()',v);extra.append(v)
baseline_h=subprocess.check_output(['git','show','379aae5a8:runtime/src/Heap/z/zMark.hpp'],text=True)
m=re.search(r'    bool IsFromObject\(BaseObject\* obj\) const\n    \{.*?\n    \}',baseline_h,re.S);assert m
v=m.group().replace('    bool IsFromObject(', 'bool ZRelocate::IsFromObject(',1).replace(') const',')');extra.append(v)
i=s.index('namespace MapleRuntime {')+len('namespace MapleRuntime {');s=s[:i]+'\n\n'+'\n\n'.join(extra)+'\n'+s[i:];(root/'zRelocate.cpp').write_text(s)
baseline=subprocess.check_output(['git','show','379aae5a8:runtime/src/Heap/z/zMark.cpp'],text=True);_,_,v=grab(baseline,'GetAndTryTagObj');bar.append(static(v,'ZBarrier'))
bar=[v.replace('[this](zpointer value)', '[](zpointer value)') for v in bar]
p=root/'zBarrier.cpp';s=p.read_text();i=s.index('namespace MapleRuntime {')+len('namespace MapleRuntime {');s=s[:i]+'''\nstd::atomic<size_t> g_minorRefCasFail{ 0 };
std::atomic<size_t> g_minorRefCasOk{ 0 };
\n'''+ '\n\n'.join(bar)+'\n'+s[i:];p.write_text(s)
p=root/'zBarrier.hpp';s=p.read_text().replace('class HeapGcState;\n','');needle='class ZBarrier : public AllStatic {\npublic:\n';assert needle in s;s=s.replace(needle,needle+'''    enum class RefSlotKind : U8 { STRONG, WEAK_REFERENT };
    static BaseObject* GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field);
    static bool TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef);
    template<bool forward>
    static bool TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& field, BaseObject*& fromObj,
                                      BaseObject*& toObj, const ForwardingProvenance& provenance);
    static bool CasInstallResolvedTarget(RefField<>& field, MAddress expected, zaddress target,
                                         bool allowNull = false);

''');s=s.replace('namespace MapleRuntime {\n','namespace MapleRuntime {\nextern std::atomic<size_t> g_minorRefCasFail;\nextern std::atomic<size_t> g_minorRefCasOk;\n',1);p.write_text(s)
p=root/'zRelocate.hpp';s=p.read_text().replace('    friend class HeapGcState;\n','');s=s.replace('class FindToVersionResult;', 'class FindToVersionResult;\nclass ScopedStopTheWorld;');needle='class ZRelocate {\npublic:\n';assert needle in s;s=s.replace(needle,needle+'''    static void ForwardFromSpace(ZGenerationId generation);
    static void RefineFromSpace();
    static BaseObject* ForwardObject(BaseObject* object, Generation generation);
    static BaseObject* ForwardObjectExclusive(BaseObject* object);
    static bool IsFromObject(BaseObject* object);
    static bool IsUnmovableFromObject(BaseObject* object);
    static BaseObject* ResolveMinorReference(RefField<>& field,
                                             const ScopedStopTheWorld* stw = nullptr);
    static BaseObject* ResolveMinorReference(RootSlot& root,
                                             const ScopedStopTheWorld* stw = nullptr);
    static bool FixMinorEvacuatedSlot(RefField<>& field, BaseObject* knownBase = nullptr,
                                      const ScopedStopTheWorld* stw = nullptr);
    static bool FixMinorEvacuatedSlot(RootSlot& root, const ScopedStopTheWorld* stw = nullptr);
    static bool FixMinorEvacuatedSlot(DerivedSlot& derived, BaseObject* knownBase = nullptr,
                                      const ScopedStopTheWorld* stw = nullptr);
    static void FixMinorRootSlots(const ScopedStopTheWorld* stw = nullptr);
    static void RemapYoungRoots();
    static bool Preforward();
    static void StartRelocationTasks(ZGenerationId generation);

''');p.write_text(s)
