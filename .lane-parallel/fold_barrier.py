from pathlib import Path
import re
root=Path('runtime/src/Heap/z'); names=['ValidateCurrentValue','JudgeHandOutTarget','FailClosedLoad','CheckStoreGoodTarget','GetAndTryTagRefField','GetAndTryTagRefFieldWithProvenance','NoteStoreGoodOnBadTarget'];bodies=[]
def extract(s,start):
 a=s.index('{',start);n=1;i=a+1
 while n:
  if s[i]=='{':n+=1
  elif s[i]=='}':n-=1
  i+=1
 return s[start:i],s[:start]+s[i:]
hp=root/'zMark.hpp';h=hp.read_text()
for name in names:
 m=re.search(r'^    (?:RefField<>|void) '+name+r'\([^;]*?\) const\n    \{',h,re.M)
 if m:
  body,h=extract(h,m.start());body=body.lstrip().replace(name+'(', 'ZBarrier::'+name+'(',1).replace(') const\n',')\n',1);bodies.append(body)
for name in names+['FindLatestVersion']:
 h=re.sub(r'^    (?:\[\[noreturn\]\] )?(?:static )?(?:HandVerdict|BaseObject\*|void) '+name+r'\([^;]*?;\n','',h,flags=re.M)
h=h.replace('    static constexpr bool kColourWhoProbe = true;','').replace('    mutable std::atomic<uint64_t> colourWhoTotal{ 0 };','').replace('    mutable std::atomic<uint64_t> colourWhoBad{ 0 };','')
hp.write_text(h)
helper=''
for fn in ['zCollectedHeap.cpp','zRelocate.cpp']:
 p=root/fn;s=p.read_text()
 for name in names+['FindLatestVersion']:
  m=re.search(r'^(?:\[\[noreturn\]\] )?(?:BaseObject\*|HandVerdict|void) HeapGcState::'+name+r'\(',s,re.M)
  if m:
   body,s=extract(s,m.start())
   if name!='FindLatestVersion':bodies.append(body.replace('HeapGcState::'+name,'ZBarrier::'+name,1).replace(') const\n',')\n',1))
 if fn=='zCollectedHeap.cpp':
  start=s.index('HandVerdict ClassifyRawHeader(');helper,s=extract(s,start)
 p.write_text(s)
p=root/'zBarrier.hpp';s=p.read_text().replace('#include "Common/BaseObject.h"','#include <atomic>\n#include "Common/BaseObject.h"',1).replace('struct ForwardingProvenance;','struct ForwardingProvenance;\nenum class HandVerdict : uint8_t;',1)
new='''
    static HandVerdict JudgeHandOutTarget(BaseObject* target);
    [[noreturn]] static void FailClosedLoad(const char* site, BaseObject* target, uintptr_t slotBits,
                                           const ForwardingProvenance& provenance);
    static BaseObject* ValidateCurrentValue(BaseObject* target, const ForwardingProvenance& provenance);
    static void CheckStoreGoodTarget(const char* consumer, BaseObject* target,
                                    const ForwardingProvenance& provenance);
    static RefField<> GetAndTryTagRefField(BaseObject* target);
    static RefField<> GetAndTryTagRefFieldWithProvenance(BaseObject* target,
                                                       const ForwardingProvenance& provenance);
    static void NoteStoreGoodOnBadTarget(BaseObject* target);
'''
s=s.replace('class ZBarrier : public AllStatic {\npublic:', 'class ZBarrier : public AllStatic {\npublic:'+new,1)
s=s.replace('\n};\n\n} // namespace MapleRuntime','\nprivate:\n    static constexpr bool kColourWhoProbe = true;\n    static std::atomic<uint64_t> colourWhoTotal;\n    static std::atomic<uint64_t> colourWhoBad;\n};\n\n} // namespace MapleRuntime');p.write_text(s)
joined='\n\n'.join(bodies).replace('RefField<>(static_cast<BaseObject*>(nullptr))','RefField<>(zpointer::null)')
joined=joined.replace('        LOG(RTLOG_ERROR, "[COLOURWHO] bad=', '        const bool inFrom = Heap::GetHeap().GetCollector().IsFromObject(target);\n        LOG(RTLOG_ERROR, "[COLOURWHO] bad=').replace('IsFromObject(target) ? 1 : 0','inFrom ? 1 : 0').replace('IsGhostFromObject(target) ? 1 : 0','inFrom ? 1 : 0')
p=root/'zBarrier.cpp';s=p.read_text().replace('#include "Heap/z/zBarrier.inline.hpp"','#include "Heap/z/zBarrier.inline.hpp"\n#include "Heap/z/zMark.hpp"',1)
s=s.replace('} // namespace MapleRuntime','namespace {\n'+helper+'\n}\nstd::atomic<uint64_t> ZBarrier::colourWhoTotal{0};\nstd::atomic<uint64_t> ZBarrier::colourWhoBad{0};\n\n'+joined+'\n} // namespace MapleRuntime');p.write_text(s)
# rewrite consumers in authorized source files, preserving already-canonical definitions
for fn in ['zMark.hpp','zMark.cpp','zCollectedHeap.cpp','zRelocate.cpp','zCrossVM.cpp','zBarrier.cpp']:
 p=root/fn;s=p.read_text()
 for name in names:
  s=s.replace('HeapGcState::'+name,'ZBarrier::'+name)
  s=re.sub(r'(?:Heap::GetHeap\(\)\.GetCollector\(\)\.|collector\.)'+name+r'\(', 'ZBarrier::'+name+'(',s)
  s=re.sub(r'(?<![\w:.])'+name+r'\(', 'ZBarrier::'+name+'(',s)
 p.write_text(s)
