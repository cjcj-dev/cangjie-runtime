// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zPageTable.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zHeap.hpp"

#include <memory>

namespace MapleRuntime {

namespace {
std::unique_ptr<ZPageTable> g_heap_table;
}

void ZPageTable::install(MAddress base, size_t heapSize, size_t granule)
{
    g_heap_table.reset(new ZPageTable(heapSize, base, granule));
}

ZPageTable& ZPageTable::heap_table()
{
    CHECK(g_heap_table != nullptr);
    return *g_heap_table;
}

int ZPageTable::count() const
{
    int n = 0;
    ZPageTableIterator iter(this);
    ZPage* page = nullptr;
    while (iter.next(&page)) {
        ++n;
    }
    return n;
}

ZPage* ZPageTable::get(MAddress addr) const
{
    zoffset offset;
    return _map.offset_for_address(addr, &offset) ? _map.get(offset) : nullptr;
}

void ZPageTable::insert(ZPage* page)
{
    zoffset offset;
    CHECK(_map.offset_for_address(page->GetRegionStart(), &offset));
    CHECK(_map.get(offset) == nullptr);
    std::atomic_thread_fence(std::memory_order_release);
    _map.put(offset, page->GetRegionSize(), page);
    if (!page->IsYoungRegion()) {
        Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::YOUNG).register_with_remset(page);
    }
}

void ZPageTable::remove(ZPage* page)
{
    zoffset offset;
    CHECK(_map.offset_for_address(page->GetRegionStart(), &offset));
    CHECK(_map.get(offset) == page);
    _map.put(offset, page->GetRegionSize(), nullptr);
}

void ZPageTable::replace(ZPage* old_page, ZPage* new_page)
{
    zoffset offset;
    CHECK(_map.offset_for_address(old_page->GetRegionStart(), &offset));
    CHECK(_map.get(offset) == old_page);
    _map.release_put(offset, old_page->GetRegionSize(), new_page);
    if (!new_page->IsYoungRegion()) {
        Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::YOUNG).register_with_remset(new_page);
    }
}

ZPageTableIterator::ZPageTableIterator(const ZPageTable* table)
    : _map(&table->_map), _index(0), _prev(nullptr)
{}

bool ZPageTableIterator::next(ZPage** page)
{
    while (_index < _map->size()) {
        ZPage* candidate = _map->at(_index++);
        if (candidate != nullptr && candidate != _prev) {
            _prev = candidate;
            *page = candidate;
            return true;
        }
    }
    return false;
}

ZGenerationPagesIterator::ZGenerationPagesIterator(const ZPageTable* page_table, ZGenerationId id,
                                                 ZPageAllocator* page_allocator)
    : _iterator(page_table), _generation_id(id), _page_allocator(page_allocator)
{
    (void)_page_allocator;
    ZPage::EnableSafeDestroy();
}

ZGenerationPagesIterator::~ZGenerationPagesIterator()
{
    ZPage::DisableSafeDestroy();
}

bool ZGenerationPagesIterator::next(ZPage** page)
{
    ZPage* candidate = nullptr;
    while (_iterator.next(&candidate)) {
        if (candidate->generation_id() == _generation_id) {
            *page = candidate;
            return true;
        }
    }
    return false;
}

void ZGenerationPagesIterator::yield(const std::function<void()>& function)
{
    ZPage::DisableSafeDestroy();
    function();
    ZPage::EnableSafeDestroy();
}

ZGenerationPagesParallelIterator::ZGenerationPagesParallelIterator(const ZPageTable* page_table, ZGenerationId id,
                                                                   ZPageAllocator* page_allocator)
    : _iterator(page_table->map()), _generation_id(id), _page_allocator(page_allocator)
{
    ZPage::EnableSafeDestroy();
}

ZGenerationPagesParallelIterator::~ZGenerationPagesParallelIterator()
{
    ZPage::DisableSafeDestroy();
}

} // namespace MapleRuntime
