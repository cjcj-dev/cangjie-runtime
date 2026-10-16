from pathlib import Path
import re
root=Path('runtime/tests/gc_unit')
def remove_func(s,needle):
 start=s.index(needle);brace=s.index('{',start);n=1;end=brace+1
 while n:
  n+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[:start]+s[end:]
def splitargs(s):
 args=[];depth=0;start=0
 for i,c in enumerate(s):
  if c in '([{':depth+=1
  elif c in ')]}':depth-=1
  elif c==',' and depth==0:args.append(s[start:i].strip());start=i+1
 args.append(s[start:].strip());return args
def replace_calls(s,name,fn):
 while name+'(' in s:
  i=s.index(name+'(');a=i+len(name)+1;b=a;depth=1
  while depth:
   depth+=(s[b]=='(')-(s[b]==')');b+=1
  s=s[:i]+fn(splitargs(s[a:b-1]))+s[b:]
 return s
for p in root.glob('*'):
 if p.suffix not in ['.hpp','.cpp']:continue
 s=p.read_text()
 s=replace_calls(s,'forwarding_for_page',lambda a:f'ZGeneration::generation(({a[0]})->generation_id())->forwarding(({a[0]})->GetRegionStart())')
 s=s.replace('ZPage::RetireDescriptor(', 'Heap::GetHeap().page_allocator().safe_destroy_page(')
 # Focused fixtures know the pointer generation before promoting/replacing pages.
 s=re.sub(r'ZPage::RetainScope lease\((forwarding|copyPage|region|page)\);',r'ZPage::RetainScope lease(ZGeneration::generation(\1->generation_id())->forwarding(\1->GetRegionStart()));',s)
 for fn,replacement in [('MarkForwardingDone','mark_done'),('IsForwardingDone','is_done'),('ReleaseForwarding','release_page')]:
  s=re.sub(r'([\w.]+(?:\(\))?)->'+fn+r'\(\)',r'ZGeneration::generation(\1->generation_id())->forwarding(\1->GetRegionStart())->'+replacement+'()',s)
 s=re.sub(r'([\w.]+)->RetainForwarding\(\)',r'ZGeneration::generation(\1->generation_id())->forwarding(\1->GetRegionStart())->retain_page(ZGeneration::generation(\1->generation_id())->relocate().queue())',s)
 s=re.sub(r'^.*(?:_scratch\.fwdOwner|_scratch\.retiredLivemap|SetRegionRole\(ZPageRole::From\)).*\n','',s,flags=re.M)
 if p.name=='test_isfromreg.cpp':
  # This sole case measures the deleted From-role claim mechanism.
  s='// From-role claim test removed by #1315: ZGC uses relocation-set membership.\n'
 if p.name=='gc_heap_fixture.hpp':
  s=re.sub(r'        for \(ZPage\* region : \{current0, current1\}\) \{\n            if \(region != nullptr\) \{\n            \}\n        \}\n','',s)
 if p.name=='test_relocation_request_queue.cpp':
  s=re.sub(r'f.queue.Add\(f.heap.region0\(\), reinterpret_cast<MAddress>\(f.heap.obj0\)(?: \+ 8)?\)', 'f.queue.Add(f.owner)',s)
 if p.name=='clear_entries_product_unit.cpp':
  s=s.replace('ZPage::RetirePage(previous, []() {});','Heap::page_table().remove(previous);\n        Heap::GetHeap().page_allocator().safe_destroy_page(previous);')
  start=s.index('    const auto oldLife =');end=s.index('    // ZPage::reset_seqnum',start)
  s=s[:start]+'    Heap::page_table().remove(region);\n    Heap::GetHeap().page_allocator().safe_destroy_page(region);\n'+s[end:]
 if p.name=='test_zLiveMap.cpp':s=s.replace('ZPage::RetirePage(region, [] {});','Heap::page_table().remove(region);\n        Heap::GetHeap().page_allocator().safe_destroy_page(region);')
 if p.name=='test_native_root_current.cpp':
  # This checks only the deleted role word, not a forwarding result.
  s=remove_func(s,'GC_OTHER_VM_TEST(YoungMarkStart, DoesNotParkPreviousFromPages)')
 if p.name=='test_relocation_set_selector.cpp':s=s.replace('    GC_EXPECT_FALSE(page->IsFromRegion());\n','') # consumer of deleted role assertion
 if p.name=='test_zPage.cpp':
  s=s.replace('                const bool stillFrom = now != nullptr && now->IsFromRegion();\n','').replace(' && !stillFrom','')
 if p.name=='test_zArray.cpp':
  # Replace callback-specific case separately with descriptor/memory lifetime test.
  s=remove_func(s,'GC_TEST(ZSafeDelete, page_retirement_defers_until_page_walk_ends)')
 if s!=p.read_text():p.write_text(s)
