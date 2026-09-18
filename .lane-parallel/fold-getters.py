from pathlib import Path
import re
r=Path('runtime/src/Heap/z')
p=r/'zMark.hpp';s=p.read_text();a=s.index('    ZGeneration& GetZGeneration(ZGenerationId generation);',s.index('class HeapGcState'));b=s.index('    [[noreturn]] static void AbortUnimplemented',a);s=s[:a]+s[b:];s=s.replace('    Allocator& GetAllocator() const { return Heap::GetHeap().GetAllocator(); }\n','');a=s.index('    GCStats& GetGCStats(');b=s.index('\n    }',a)+6;s=s[:a]+s[b:];p.write_text(s)
p=r/'zCollectedHeap.cpp';s=p.read_text()
for sig in ['ZGeneration& HeapGcState::GetZGeneration','const ZGeneration& HeapGcState::GetZGeneration']:
 a=s.index(sig); b=s.index('\n}',a)+2;s=s[:a]+s[b:]
p.write_text(s)
names=['GetZGeneration','GetCycleSnapshot','GetGCStats','GetAllocator','OldActiveRemsetIsCurrent']
# The named source bodies are the remaining collector methods; Heap and allocator
# method definitions are deliberately outside this set.
for name in ['zMark.hpp','zMark.cpp','zRelocate.cpp','zRelocationSet.cpp','zRootsIterator.cpp','zTracing.cpp','zCrossVM.cpp','zStat.cpp','zGeneration.cpp']:
 p=r/name;s=p.read_text()
 for method in names:
  s=s.replace('TheCollector().'+method+'(', 'Heap::GetHeap().'+method+'(')
  s=s.replace('collector.'+method+'(', 'Heap::GetHeap().'+method+'(')
  s=re.sub(r'(?<![\w.:>])'+method+r'\(', 'Heap::GetHeap().'+method+'(',s)
 p.write_text(s)
for name in ['zDirector.cpp','zPage.cpp','zRemembered.cpp']:
 p=r/name;s=p.read_text()
 for method in names:s=s.replace('collector.'+method+'(', 'Heap::GetHeap().'+method+'(')
 s=s.replace('    auto& collector = Heap::GetHeap().GetCollector();\n','').replace('    HeapGcState& collector = Heap::GetHeap().GetCollector();\n','')
 p.write_text(s)
# A fixture static adapter no longer needs a collector-derived shell.
p=Path('runtime/tests/gc_unit/gc_heap_fixture.hpp');s=p.read_text();a=s.index('struct LiveMapCycleAccess : HeapGcState {');b=s.index('\n};',a)+3;s=s[:a]+s[b:];s=s.replace('LiveMapCycleAccess::Cycle(Heap::GetHeap().GetCollector(), generation)', 'Heap::GetHeap().GetZGeneration(generation)');p.write_text(s)
