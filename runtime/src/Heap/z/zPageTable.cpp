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
ZPageTable& ZPageTable::heap_table()
{
    return Heap::GetHeap().page_table();
}

// ZGC zPageTable.cpp:32-41: size the map before constructing distributors.
static size_t get_max_offset_for_map()
{
    const size_t max_count = ZAddressOffsetMax >> ZGranuleSizeShift;
    const size_t required_count = ZIndexDistributor::get_count(max_count);
    return required_count << ZGranuleSizeShift;
}

ZPageTable::ZPageTable() : _map(get_max_offset_for_map()) {}

ZPage* ZPageTable::get(MAddress addr) const
{
    if (addr < ZAddressHeapBase || addr - ZAddressHeapBase >= ZAddressOffsetMax) {
        return nullptr;
    }
    return _map.get(ZAddress::offset(to_zaddress_unsafe(addr)));
}

void ZPageTable::insert(ZPage* page)
{
    const zoffset offset = page->start();
    CHECK(_map.get(offset) == nullptr);
    std::atomic_thread_fence(std::memory_order_release);
    _map.put(offset, page->GetRegionSize(), page);
    if (!page->IsYoungRegion()) {
        ZGeneration::young()->register_with_remset(page);
    }
}

void ZPageTable::remove(ZPage* page)
{
    const zoffset offset = page->start();
    CHECK(_map.get(offset) == page);
    _map.put(offset, page->GetRegionSize(), nullptr);
}

void ZPageTable::replace(ZPage* old_page, ZPage* new_page)
{
    const zoffset offset = old_page->start();
    CHECK(_map.get(offset) == old_page);
    _map.release_put(offset, old_page->GetRegionSize(), new_page);
    if (!new_page->IsYoungRegion()) {
        ZGeneration::young()->register_with_remset(new_page);
    }
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
    : _iterator(page_table), _generation_id(id), _page_allocator(page_allocator)
{
    ZPage::EnableSafeDestroy();
}

ZGenerationPagesParallelIterator::~ZGenerationPagesParallelIterator()
{
    ZPage::DisableSafeDestroy();
}

} // namespace MapleRuntime
