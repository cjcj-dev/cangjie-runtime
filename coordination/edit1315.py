from pathlib import Path
root=Path('runtime/src/Heap/z')
def edit(name,fn):
 p=root/name;p.write_text(fn(p.read_text()))
def remove_func(s,needle):
 start=s.index(needle);brace=s.index('{',start);n=1;end=brace+1
 while n:
  n += (s[end]=='{')-(s[end]=='}');end+=1
 if end<len(s) and s[end]==';':end+=1
 return s[:start]+s[end:]
def repl(s,a,b):
 assert a in s,a
 return s.replace(a,b)
# Set membership remains the only forwarding identity.
def forward_h(s):
 s=s.replace('using RegionLifeId = uint64_t;','')
 start=s.index('    static ZForwarding* alloc(size_t');end=s.index('    MAddress start() const;',start);s=s[:start]+s[end:]
 for line in ['    RegionLifeId page_life_id() const { return _page_life_id; }\n','    bool page_life_current() const;\n','    const RegionLifeId _page_life_id;\n'] :s=s.replace(line,'')
 start=s.index('    // zPage.inline.hpp:176-185');end=s.index('    bool covers(',start);s=s[:start]+s[end:]
 return s.replace('RegionLifeId pageLifeId, PageAge from_age','PageAge from_age')
edit('zForwarding.hpp',forward_h)
edit('zForwarding.inline.hpp',lambda s:remove_func(s,'inline ZForwarding* ZForwarding::alloc(size_t').replace('RegionLifeId pageLifeId, PageAge from_age','PageAge from_age').replace('          _page_life_id(pageLifeId),\n',''))
edit('zForwarding.cpp',lambda s:remove_func(remove_func(s,'ZPage::InPlaceClaimScope::InPlaceClaimScope('),'bool ZForwarding::page_life_current()').replace('nentries, page->GetRegionLifeId(), page->age()','nentries, page->age()'))
page_funcs=['IsCompacted','IsRoutingState','WaitCopiedBeforePayloadWipe','GetGhostRegionSize','RetirePageMemory','RetainForwarding','ReleaseForwarding','ClaimForwarding','MarkForwardingDone','IsForwardingDone','ForwardingRefCount','ForwardingClaimed','BumpRegionLifeId','RelocateObserve','RetirePage','RetireDescriptor','EnableSafeDestroy','DisableSafeDestroy','VisitPageOwners']
def page_inline(s):
 import re
 for f in page_funcs:
  m=re.search(r'inline [^\n]*ZPage::'+f+r'\(',s);assert m,f
  s=remove_func(s,m.group())
 start=s.index('        // Invalidate every old-life carrier');end=s.index('        _top =',start);s=s[:start]+s[end:]
 return s
edit('zPage.inline.hpp',page_inline)
def page_h(s):
 import re
 s=s.replace('using RegionLifeId = uint64_t;','')
 start=s.index('    // RetirePage hook:');end=s.index('public:',start);s=s[:start]+s[end:]
 for f in ['GetRegionLifeSeq','GetRegionLifeId','PeekForwardingOwner','CopyInflightWord','TryLockReadFromRegion','UnlockReadFromRegion','IsFromRegion']:
  m=re.search(r'    [^\n]*\b'+f+r'\(',s);assert m,f;s=remove_func(s,m.group())
 start=s.index('    // zPageAllocator.cpp:2248-2250');end=s.index('    // Metadata over one unit',start);s=s[:start]+s[end:]
 for f in page_funcs+['GetFromPageCarrier']:
  s=re.sub(r'^    [^\n]*\b'+f+r'\([^\n]*;\n','',s,flags=re.M)
 s=s.replace('        explicit RetainScope(ZPage* region) : RetainScope(forwarding_for_page(region)) {}\n','').replace('            CHECK(!retained || owner->page_life_current());\n','')
 start=s.index('    // zForwarding.cpp:110-181 in_place');end=s.index('    // These interfaces',start);s=s[:start]+s[end:]
 for field in ['regionLifeId','retiredLivemap','ownerRegion','ownerRegion0','regionLifeSequence','fwdOwner','copyInflight','ghostLifeId']:
  s=re.sub(r'^        [^\n]*\b'+field+r'\b[^\n]*\n','',s,flags=re.M)
 return s
edit('zPage.hpp',page_h)
edit('zPage.cpp',lambda s:remove_func(s,'ZForwarding* ZPage::GetFromPageCarrier()').replace('ZSafeDelete<ZPage> ZPage::safeDestroy;\n','').replace('    if (_retireHook) {\n        _retireHook();\n    }\n',''))
# Allocator owns descriptor deferral; memory return happens independently.
def alloc_h(s):
 s=s.replace('#include "Heap/z/zFuture.hpp"','#include "Heap/z/zFuture.hpp"\n#include "Heap/z/zSafeDelete.inline.hpp"')
 s=remove_func(s,'    void VisitPageOwners(')
 s=s.replace('    void ResetFlipPromotedPages();\n','').replace('    void safe_destroy_page(ZPage* page);','    void safe_destroy_page(ZPage* page);\n    void enable_safe_destroy() const;\n    void disable_safe_destroy() const;\n    void VisitPageOwners(const std::function<void(ZPage*)>& visitor) const;')
 s=s.replace('    void ReclaimRetiredRegion(ZPage* region);\n','').replace('    void ReleaseRetiredRegion(ZPage* region);\n','')
 pos=s.index('private:',s.index('class RegionManager {'))
 s=s[:pos]+s[pos:].replace('private:','private:\n    mutable ZSafeDelete<ZPage> _safe_destroy;\n    PageMemory prepare_memory_for_free(ZPage* page);',1)
 for f in ['GetRecentAllocatedSize','GetSurvivedSize','GetFromSpaceSize','SumAllocatedByRoles','GetLargeObjectSize']:
  import re
  s=re.sub(r'^    [^\n]*\b'+f+r'\([^\n]*;\n','',s,flags=re.M)
 return s
edit('zPageAllocator.hpp',alloc_h)
def alloc_cpp(s):
 s=s.replace('    ZPage::RetireDescriptor(page);','    _safe_destroy.schedule_delete(page);')
 start=s.index('    const PageMemory memory{page->granule_index(), size, 0, true};');end=s.index('    decrease_used_generation',start)
 s=s[:start]+'    const PageMemory memory = prepare_memory_for_free(page);\n'+s[end:]
 start=s.index('void RegionManager::ReclaimRegion(');end=s.index('// ZGC zPageAllocator.cpp:426',start)
 s=s[:start]+'''void RegionManager::enable_safe_destroy() const
{
    _safe_destroy.enable_deferred_delete();
}

void RegionManager::disable_safe_destroy() const
{
    _safe_destroy.disable_deferred_delete();
}

PageMemory RegionManager::prepare_memory_for_free(ZPage* page)
{
    const size_t index = page->granule_index();
    const size_t size = page->size();
    const uint32_t partition = page->partition_id();
    safe_destroy_page(page);
    return PageMemory{index, size, partition, true};
}

void RegionManager::VisitPageOwners(const std::function<void(ZPage*)>& visitor) const
{
    for (ZGenerationId id : {ZGenerationId::young, ZGenerationId::old}) {
        ZGenerationPagesIterator iter(&ZPageTable::heap_table(), id, const_cast<RegionManager*>(this));
        for (ZPage* page; iter.next(&page);) {
            visitor(page);
        }
    }
}

void RegionManager::ReclaimRegion(ZPage* region)
{
    Heap::free_page(region);
}

'''+s[end:]
 start=s.index('        // Only materialized page geometry');end=s.index('        return;',start)
 s=s[:start]+'''        ZPageTable::heap_table().remove(region);
        const PageMemory retired = prepare_memory_for_free(region);
        ReturnRetiredPageMemory(retired);
'''+s[end:]
 start=s.index('size_t RegionManager::ReleaseRegion(');end=s.index('void RegionManager::PromoteAllRegions()',start)
 s=s[:start]+'''size_t RegionManager::ReleaseRegion(ZPage* region)
{
    const size_t size = region->size();
    Heap::free_page(region);
    return size;
}

'''+s[end:]
 return remove_func(s,'size_t RegionManager::GetLargeObjectSize()')
edit('zPageAllocator.cpp',alloc_cpp)
def alloc_inline(s):
 for f in ['SumAllocatedByRoles','GetRecentAllocatedSize','GetSurvivedSize','GetFromSpaceSize']:
  s=remove_func(s,'inline size_t RegionManager::'+f+'(')
 return s
edit('zPageAllocator.inline.hpp',alloc_inline)
def obj(s):
 start=s.index('    ZPage::RetirePage(page,');end=s.index('\n}',start)
 return s[:start]+'''    ZPageTable::heap_table().remove(page);
    const PageMemory memory = prepare_memory_for_free(page);
    ReturnRetiredPageMemory(memory);'''+s[end:]
edit('zObjectAllocator.cpp',obj)
edit('zPageTable.cpp',lambda s:s.replace('    (void)_page_allocator;\n','').replace('ZPage::EnableSafeDestroy()','_page_allocator->enable_safe_destroy()').replace('ZPage::DisableSafeDestroy()','_page_allocator->disable_safe_destroy()'))
edit('zRemembered.cpp',lambda s:s.replace('ZPage::EnableSafeDestroy()','Heap::GetHeap().page_allocator().enable_safe_destroy()').replace('ZPage::DisableSafeDestroy()','Heap::GetHeap().page_allocator().disable_safe_destroy()'))
# Set residual shape.
def reloc_set(s):
 start=s.index('static const ZStatSubPhase PPostTrace');end=s.index('ZRelocationSet::ZRelocationSet',start)
 block=s[start:s.index('} // namespace MapleRuntime',start)]
 edit('zGeneration.cpp',lambda g:g.replace('    space.GetRegionManager().ResetFlipPromotedPages();\n','').replace('id, nullptr);','id, &Heap::GetHeap().page_allocator());').replace('        forwarding->page()->SetRegionRole(ZPageRole::From);\n','')+'\nnamespace MapleRuntime {\n'+block+'\n}\n')
 s=s[:start]+s[end:]
 s=s.replace('_generation != nullptr ? _generation->Workers() : nullptr','_generation->Workers()')
 s=repl(s,'    if (ZWorkers* w = workers()) {\n        w->run(&task);\n    } else {\n        task.work();\n    }','    workers()->run(&task);')
 s=s.replace('    if (_generation != nullptr) {\n        _generation->StatRelocation()->AtInstallRelocationSet(_allocator.size());\n    }','    _generation->StatRelocation()->AtInstallRelocationSet(_allocator.size());')
 s=s.replace('    _forwardings = nullptr;\n','')
 s=s.replace('void ZRelocationSet::register_relocate_promoted(const ZArray<ZPage*>& pages)\n{','void ZRelocationSet::register_relocate_promoted(const ZArray<ZPage*>& pages)\n{\n    if (pages.is_empty()) {\n        return;\n    }')
 s=s.replace('        _relocate_promoted_pages.push(pages.at(i));','        CHECK(!_relocate_promoted_pages.contains(pages.at(i)));\n        _relocate_promoted_pages.push(pages.at(i));').replace('    _in_place_relocate_promoted_pages.push(page);','    CHECK(!_in_place_relocate_promoted_pages.contains(page));\n    _in_place_relocate_promoted_pages.push(page);')
 return s
edit('zRelocationSet.cpp',reloc_set)
def reloc(s):
 s=remove_func(s,'ZRelocateQueue::EnqueueResult ZRelocateQueue::Add(void*')
 start=s.index('            // ZGC zRelocate.cpp:1026-1037');end=s.index('\n        } else {',start)
 return s[:start]+s[end:]
edit('zRelocate.cpp',reloc)
edit('zRelocate.hpp',lambda s: __import__('re').sub(r'^    .*Add\(void\*[^\n]*\n','',s,flags=__import__('re').M))
edit('zForwardingTable.cpp',lambda s:remove_func(s,'ZForwarding* forwarding_for_page('))
edit('zForwardingTable.hpp',lambda s:'\n'.join(l for l in s.split('\n') if 'forwarding_for_page(' not in l))
