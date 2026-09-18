from pathlib import Path
import re
root=Path('runtime/src/Heap/z')
hp=root/'zMark.hpp';h=hp.read_text();bodies=[]
def extract(s,start):
 a=s.index('{',start);level=1;i=a+1
 while level:
  if s[i]=='{':level+=1
  elif s[i]=='}':level-=1
  i+=1
 return s[start:i],s[:start]+s[i:]
# observation and root types
start=h.index('struct ExportOwnershipTestObservation {'); obs,h=extract(h,start);h=h.replace(';\n\nstruct RemapYoungRootsTestReceipt','\n\nstruct RemapYoungRootsTestReceipt',1)
a=h.index('struct ValueRoot {');b=h.index('\nclass HeapGcState',a);types=h[a:b];h=h[:a]+h[b:]
h=h.replace('using CrossRefHandler = void(*)(BaseObject*, BaseObject*);','')
inline=['ResurrectExportObject','PrepareCycleRef','MergeResurrectExportObjects']
for name in inline:
 m=re.search(r'    void '+name+r'\([^\n]*\)\n    \{',h);body,h=extract(h,m.start());bodies.append(body.replace('    void '+name,'void ZCrossVM::'+name,1))
# state and helper declarations removed together
start=h.index('    std::mutex externMtx;');end=h.index('    // enum all common roots.',start)
state=h[start:end]
# only block through CurrentizeValueRootMap; preserve unrelated tail if any
last=state.index('    void CurrentizeValueRootMap');last=state.index(';',last)+1
h=h[:start]+state[last:]+h[end:];state=state[:last]
methods=['ResolveCurrentValueRoot','CurrentizeValueRootSet','CurrentizeValueRootMap','VisitSurrectedExportRoots','VisitMinorValueRoots','FindUselessExternObjects','PreforwardDiscoveredExternObjects','PreforwardAllResurrectExportFromObjects','ProcessExportRoots']
for fn in ['zMark.cpp','zGeneration.cpp','zRelocate.cpp']:
 p=root/fn;s=p.read_text()
 for name in methods:
  m=re.search(r'^(?:BaseObject\*|void) HeapGcState::'+name+r'\(',s,re.M)
  if m:
   body,s=extract(s,m.start());bodies.append(body.replace('HeapGcState::'+name,'ZCrossVM::'+name,1))
 p.write_text(s)
for name in methods+['ResolveCycleRef','PostResolveCycleTask','GetCrossRefHandler']:
 h=re.sub(r'^    (?:void|CrossRefHandler|BaseObject\*) '+name+r'\([^;]*?;\n','',h,flags=re.M)
h=re.sub(r'^    void SetCycleRefHandlerForTest[^\n]*\n','',h,flags=re.M)
h=h.replace('    CrossRefHandler cycleRefHandlerForTest = nullptr;','')
h=h.replace('    static std::function<void(const ExportOwnershipTestObservation&)> testExportOwnershipResult;','')
h=h.replace('class HeapGcState {','class HeapGcState {\n    friend class ZCrossVM;',1)
h=h.replace('#include "Heap/z/zMarkStack.hpp"','#include "Heap/z/zMarkStack.hpp"\n#include "Heap/z/zCrossVM.hpp"',1)
hp.write_text(h)
# new independent header, enum incomplete until cpp
header='''// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#ifndef MRT_ZCROSSVM_HPP
#define MRT_ZCROSSVM_HPP
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Common/TypeDef.h"
#include "Common/MarkWorkStack.h"
#include "Heap/z/zGenerationId.hpp"
namespace MapleRuntime {
class BaseObject;
enum class ForwardingStage : uint8_t;
using CrossRefHandler = void(*)(BaseObject*, BaseObject*);
struct ValueRoot {
    BaseObject* object;
    ForwardingStage stage;
    uintptr_t color;
    Generation generation;
    ValueRoot(BaseObject* value);
    ValueRoot(BaseObject* value, ForwardingStage source);
    operator BaseObject*() const { return object; }
    ForwardingStage Stage() const;
};
struct ValueRootHash {
    size_t operator()(const ValueRoot& root) const { return std::hash<BaseObject*>{}(root.object); }
};
using ValueRootSet = std::unordered_set<ValueRoot, ValueRootHash>;
using ValueRootList = std::list<ValueRoot>;
using ValueRootMap = std::unordered_map<ValueRoot, ValueRootList, ValueRootHash>;
#if defined(MRT_TESTABLE_INTERNALS)
'''+obs+''';
#endif
// Cangjie foreign-runtime ownership and managed cycle-resolution infrastructure.
// This state has no ZGC collector hierarchy counterpart.
class ZCrossVM {
public:
    void ResurrectExportObject(BaseObject* obj);
    void PrepareCycleRef();
    void MergeResurrectExportObjects(Generation generation);
    void ResolveCycleRef();
    void PostResolveCycleTask();
    void ProcessExportRoots(WorkStack& foreignRootsSet);
    void FindUselessExternObjects();
    void VisitMinorValueRoots(const std::function<void(BaseObject*)>& visitor);
    void VisitSurrectedExportRoots(const std::function<void(BaseObject*)>& visitor);
    void PreforwardDiscoveredExternObjects(Generation generation);
    void PreforwardAllResurrectExportFromObjects(Generation generation);
#if defined(MRT_TESTABLE_INTERNALS)
    static std::function<void(const ExportOwnershipTestObservation&)> testExportOwnershipResult;
    void ObserveExportOwnershipForTest(bool afterHandoff);
#endif
#if defined(MRT_GC_UNIT_TESTS)
    void SetCycleRefHandlerForTest(CrossRefHandler handler) { cycleRefHandlerForTest = handler; }
#endif
private:
#if defined(MRT_GC_UNIT_TESTS) || defined(MRT_TESTABLE_INTERNALS)
    friend struct RelocationReceiptTestAccess;
    friend struct ZGenerationRootTestAccess;
#endif
    CrossRefHandler GetCrossRefHandler(BaseObject* foreignProxy);
#if defined(MRT_GC_UNIT_TESTS)
    CrossRefHandler cycleRefHandlerForTest = nullptr;
#endif
'''+state.replace('#if defined(MRT_TESTABLE_INTERNALS)\n    void ObserveExportOwnershipForTest(bool afterHandoff);\n#endif\n','').replace('ForwardingStage stage = ForwardingStage::OverwritePrevious','ForwardingStage stage')+'''
};
} // namespace MapleRuntime
#endif
'''
(root/'zCrossVM.hpp').write_text(header)
p=root/'zCrossVM.cpp';s=p.read_text();s=s.replace('HeapGcState::GetCrossRefHandler','ZCrossVM::GetCrossRefHandler').replace('HeapGcState::ResolveCycleRef','ZCrossVM::ResolveCycleRef').replace('HeapGcState::PostResolveCycleTask','ZCrossVM::PostResolveCycleTask')
value='''
ValueRoot::ValueRoot(BaseObject* value) : ValueRoot(value, ForwardingStage::OverwritePrevious) {}
ValueRoot::ValueRoot(BaseObject* value, ForwardingStage source)
    : object(value), stage(source), color(::g_cjLoadGoodMask),
      generation(source == ForwardingStage::IncomingNew && Heap::IsHeapAddress(value)
          ? Heap::page(reinterpret_cast<MAddress>(value))->GetOwnerGeneration() : Generation::Old) {}
ForwardingStage ValueRoot::Stage() const
{
    const uintptr_t mask = generation == Generation::Young ? ZPointerRemappedYoungMask : ZPointerRemappedOldMask;
    return (ZPointer::remap_bits(color) & mask) != 0 ? stage : ForwardingStage::OverwritePrevious;
}
extern thread_local const char* gMinorRootOrigin;
'''
joined='\n\n'.join(bodies)
joined=joined.replace('return ValidateCurrentValue(value, provenance);','return Heap::GetHeap().GetCollector().ValidateCurrentValue(value, provenance);').replace(': ResolveStoreValue(value, provenance,',': Heap::GetHeap().GetCollector().ResolveStoreValue(value, provenance,')
joined=joined.replace('        MarkOldObjectIfActive(exportObj, true);','        Heap::GetHeap().old().MarkObjectIfActive<false, true, true, false>(from_object(exportObj));').replace('        RunMajorStripeMark();','        Heap::GetHeap().GetCollector().RunMajorStripeMark();').replace('GetAndTryTagObj(RefSlotKind::STRONG, object, field)','Heap::GetHeap().GetCollector().GetAndTryTagObj(RefSlotKind::STRONG, object, field)')
s=s.replace('} // namespace MapleRuntime',value+'\n'+joined+'\n} // namespace MapleRuntime');p.write_text(s)
# reroute remaining authorized call sites
names=methods+inline+['PostResolveCycleTask']
for fn in ['zMark.cpp','zGeneration.cpp','zRelocate.cpp']:
 p=root/fn;s=p.read_text()
 for name in names:
  s=re.sub(r'(?:Heap::GetHeap\(\)\.GetCollector\(\)\.|TheCollector\(\)\.|collector\.)?\b'+name+r'\(', 'Heap::GetHeap().cross_vm().'+name+'(',s)
 p.write_text(s)
p=root/'zExportOwnershipTestObservations.hpp';p.write_text(p.read_text().replace('HeapGcState::ObserveExportOwnershipForTest','ZCrossVM::ObserveExportOwnershipForTest'))
p=root/'zTracing.cpp';p.write_text(p.read_text().replace('HeapGcState::testExportOwnershipResult','ZCrossVM::testExportOwnershipResult'))
