// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zObjectAllocator.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "Heap/z/zGlobals.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sched.h>
#include <unistd.h>
#include <vector>
#if defined(_WIN64)
#include <processthreadsapi.h>
#endif

#include "Heap/Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/Allocator/HeapFiller.h"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"

namespace MapleRuntime {
#if defined(MRT_TESTABLE_INTERNALS)
void (*RegionManager::testPinnedPageAcquired)(ZPage*) = nullptr;
#endif
// ThreadLocalAllocBuffer::initial_desired_size (cpp:265): a new thread
// starts with the published allocation fraction instead of a fixed extent.
void RegionManager::InitializeTLAB(AllocBuffer& buffer)
{
    std::lock_guard<std::mutex> lock(tlabStatisticsLock);
    const size_t threads = std::max(static_cast<size_t>(tlabAllocatingThreads.Average() + 0.5), size_t{1});
    buffer.ResizeTLAB(GetTLABCapacity(), tlabRequestedFraction.Average() / threads,
                      GetThreadLocalRegionSize());
}

// ZTLABUsage::reset (zTLABUsage.cpp:41), called before retiring allocating
// regions in young mark-start (zGeneration.cpp:862).
void RegionManager::ResetTLABUsage()
{
    std::lock_guard<std::mutex> lock(tlabStatisticsLock);
    const size_t used = tlabUsed.exchange(0, std::memory_order_relaxed);
    if (used != 0) {
        // TruncatedSeq::davg uses AbsSeq's exponential average, alpha=0.3;
        // its last value and average are stable throughout the next cycle.
        tlabCapacity = lastTLABUsed == 0 ? used : tlabCapacity + 0.3 * (used - tlabCapacity);
        lastTLABUsed = used;
    }
}

// ZThreadLocalAllocBuffer::publish_statistics (zThreadLocalAllocBuffer.cpp:52).
// Thread retirement statistics consume the already published backing history.
void RegionManager::PublishTLABStatistics()
{
    std::lock_guard<std::mutex> lock(tlabStatisticsLock);
    const size_t capacity = GetTLABCapacity();
    TLABStatistics total = retiredTLABStatistics;
    retiredTLABStatistics = TLABStatistics{};
    const size_t threads = std::max(static_cast<size_t>(tlabAllocatingThreads.Average() + 0.5), size_t{1});
    const double fallback = tlabRequestedFraction.Average() / threads;
    Heap::GetHeap().GetAllocator().VisitAllocBuffers([&](AllocBuffer& buffer) {
        buffer.AccumulateTLABStatistics(total, GetTLABUsed(), capacity);
        buffer.ResizeTLAB(capacity, fallback, GetThreadLocalRegionSize());
    });
    if (total.Used() != 0) {
        tlabAllocatingThreads.Sample(total.allocatingThreads);
        if (lastTLABUsed > 0.5 * capacity) {
            tlabRequestedFraction.Sample(std::min(static_cast<double>(total.Used()) /
                                                 std::max(capacity, size_t{1}), 1.0));
        }
    }
    VLOG(REPORT, "TLAB totals: used=%zu capacity=%zu allocated=%zu refills=%zu refill-waste=%zu gc-waste=%zu threads=%zu",
         lastTLABUsed, capacity, total.allocatedSize, total.refills, total.refillWaste, total.gcWaste,
         total.allocatingThreads);
}

void RegionManager::RetireTLABStatistics(AllocBuffer& buffer)
{
    std::lock_guard<std::mutex> lock(tlabStatisticsLock);
    buffer.AccumulateTLABStatistics(retiredTLABStatistics, GetTLABUsed(), GetTLABCapacity());
}

// zObjectAllocator.cpp:40-45
ZObjectAllocator::PerAge::PerAge(PageAge pageAge)
    : age(pageAge),
      usePerCpuSharedSmallPages(ZHeuristics::use_per_cpu_shared_small_pages()),
      sharedSmallPage(nullptr),
      sharedMediumPage(nullptr) {}

// zHeap.cpp:229: shared-page TLAB accounting includes only small eden pages.
static bool IsSmallEdenPage(const ZPage* page)
{
    return page->IsSmallRegion() && page->IsYoungRegion() &&
           page->GetYoungAge() == static_cast<uint8_t>(untype(PageAge::eden));
}

// ZObjectAllocator::PerAge::alloc_page, ZHeap::alloc_page/account_alloc_page.
ZPage* RegionManager::AllocateSharedPage(size_t units, ZPageType role,
                                             PageAge age, bool nonBlocking)
{
    ZPage* page = Heap::alloc_page(units, role, false, !nonBlocking, true, age);
    if (page == nullptr) { return nullptr; }
    page->reset(age);
    if (IsSmallEdenPage(page)) {
        tlabUsed.fetch_add(page->GetRegionSize(), std::memory_order_relaxed);
    }
    // zObjectAllocator.cpp:40-45: the shared page is a per-CPU/per-age
    // pointer; the lifecycle role word records the page as recent
    // (zPageAllocator.cpp:1518 accounting follows the role).
    if (role == ZPageType::large) {
        page->SetRegionRole(ZPageRole::RecentLarge);
    } else {
        page->SetRegionRole(ZPageRole::RecentFull);

    }
    return page;
}

// ZObjectAllocator::PerAge::undo_alloc_page: this unpublished candidate was
// never used by a caller. Undo its page charge, not a TLAB's ownership.
void RegionManager::UndoSharedPage(ZPage* page)
{
    page->SetRegionRole(ZPageRole::None);

    if (IsSmallEdenPage(page)) {
        tlabUsed.fetch_sub(page->GetRegionSize(), std::memory_order_relaxed);
    }
    // ZHeap::undo_alloc_page: remove the unused page-table entry and return
    // the extent without suspending a caller holding an unpublished object.
    ZPage::RetirePage(page, [this, page] {
        const size_t units = page->GetUnitCount();
        const size_t index = page->GetUnitIdx();
        page->InitFreeUnits();
        ReturnRetiredPageMemory(PageMemory{index, units, 0, true}, false);
    });
}

uintptr_t ZObjectAllocator::alloc(size_t size, PageAge age, bool nonBlocking)
{
    CHECK(untype(age) < kPageAgeCount);
    RegionManager& manager = Heap::GetHeap().page_allocator();
    PerAge& allocator = *this->allocator(age);
    if (size > ZObjectSizeLimitSmall) {
        if (ZPageSizeMediumEnabled && size <= ZObjectSizeLimitMedium) {
            std::lock_guard<ZLock> mediumLock(allocator.mediumPageAllocLock);
            ZPage** const shared = allocator.shared_medium_page_addr();
            ZPage* page = __atomic_load_n(shared, __ATOMIC_ACQUIRE);
            uintptr_t addr = page == nullptr ? 0 : page->AtomicAlloc(size);
            if (addr != 0) { return addr; }
            const size_t units = AlignUp(ZPageSizeMediumMin, ZPage::UNIT_SIZE) / ZPage::UNIT_SIZE;
            ZPage* fresh = manager.AllocateSharedPage(units, ZPageType::medium, allocator.age, nonBlocking);
            if (fresh == nullptr) { return 0; }
            addr = fresh->Alloc(size);
            CHECK(addr != 0);
            for (;;) {
                if (__atomic_compare_exchange_n(shared, &page, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                    return addr;
                }
                if (page == nullptr) { continue; }
                const uintptr_t previous = page->AtomicAlloc(size);
                if (previous == 0) { continue; }
                manager.UndoSharedPage(fresh);
                return previous;
            }
        }
        const size_t units = AlignUp(size, ZPage::UNIT_SIZE) / ZPage::UNIT_SIZE;
        ZPage* page = manager.AllocateSharedPage(units, ZPageType::large, allocator.age, nonBlocking);
        return page == nullptr ? 0 : page->Alloc(size);
    }
    ZPage** const shared = allocator.shared_small_page_addr();
    ZPage* page = __atomic_load_n(shared, __ATOMIC_ACQUIRE);
    uintptr_t addr = page == nullptr ? 0 : page->AtomicAlloc(size);
    if (addr != 0) { return addr; }

    ZPage* fresh = manager.AllocateSharedPage(manager.SharedPageUnitCount(), ZPageType::small,
                                           allocator.age, nonBlocking);
    if (fresh == nullptr) { return 0; }
    addr = fresh->Alloc(size);
    CHECK(addr != 0);
    for (;;) {
        if (__atomic_compare_exchange_n(shared, &page, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return addr;
        }
        if (page == nullptr) { continue; }
        const uintptr_t previous = page->AtomicAlloc(size);
        if (previous == 0) { continue; }
        manager.UndoSharedPage(fresh);
        return previous;
    }
}

// ZObjectAllocator::retire_pages / PerAge::retire_pages (cpp:208-237).
// Called in the corresponding generation's mark-start pause. The lifecycle
// lists retain pages; retirement only removes allocation shortcuts.
void ZObjectAllocator::retire_pages(PageAgeRange ages)
{
    // zObjectAllocator.cpp:198-203 PerAge::retire_pages: set_all(nullptr).
    for (PageAge age : ages) {
        auto* perAge = allocator(age);
        perAge->pinnedPage.store(nullptr, std::memory_order_release);
        perAge->sharedSmallPage.set_all(nullptr);
        perAge->sharedMediumPage.set(nullptr);
    }
}

ZPage* RegionManager::AllocateThreadLocalRegion(size_t size, bool expectPhysicalMem, bool youngRegion,
                                                   bool allowSaferegion)
{
    // ZHeap::max_tlab_size / unsafe_max_tlab_alloc (zHeap.cpp:144-160):
    // the caller computes the refill size; the allocator enforces its extent.
    if (size == 0 || size > GetThreadLocalRegionSize()) {
        return nullptr;
    }
    const size_t units = AlignUp(size, ZPage::UNIT_SIZE) / ZPage::UNIT_SIZE;
    ZPage* region = Heap::alloc_page(units, ZPageType::small, expectPhysicalMem,
                                    allowSaferegion, true, youngRegion ? PageAge::eden : PageAge::old);
    if (region != nullptr) {
        {
            region->reset(youngRegion ? PageAge::eden : PageAge::old);
            if (youngRegion) {
                // zHeap.cpp:233: charge the backing extent even before a
                // prepared region is installed as a thread's current TLAB.
                tlabUsed.fetch_add(region->GetRegionSize(), std::memory_order_relaxed);
            }
            region->SetRegionRole(ZPageRole::ThreadLocal);
            DLOG(REGION, "alloc tl-region %p @[0x%zx+%zu, 0x%zx) units[%zu+%zu, %zu) type %u",
                region, region->GetRegionStart(), region->GetRegionSize(), region->GetRegionEnd(),
                region->GetUnitIdx(), region->GetUnitCount(), region->GetUnitIdx() + region->GetUnitCount(),
                0u);
        }
    }

    return region;
}

// ZHeap::undo_alloc_page (zHeap.cpp:270): only a failed publication of
// a newly allocated, unused backing region cancels its allocation charge.
// GC retirement/reclamation is not an undo and must retain that cycle's usage.
void RegionManager::UndoThreadLocalRegionAllocation(ZPage* region)
{
    CHECK(region != nullptr && region->IsEmpty() && region->IsThreadLocalRegion());
    if (region->IsYoungRegion()) {
        const size_t size = region->GetRegionSize();
        const size_t previous = tlabUsed.fetch_sub(size, std::memory_order_relaxed);
        CHECK(previous >= size);
    }
    RemoveThreadLocalRegion(region);
    ReclaimRegion(region);
}

void RegionManager::RequestForRegion(size_t size)
{
    if (IsGcThread()) {
        // gc thread is always permitted for allocation.
        return;
    }

    Heap& heap = Heap::GetHeap();
    const size_t liveAfterGC = lastLiveBytesAfterGC.load(std::memory_order_acquire);
    size_t allocatedBytes = GetAllocatedSize() - liveAfterGC;
    constexpr double pi = 3.14;
    size_t availableBytesAfterGC = heap.GetMaxCapacity() - liveAfterGC;
    double heuAllocRate = std::cos((pi / 2.0) * allocatedBytes / availableBytesAfterGC) *
        lastCollectionRate.load(std::memory_order_acquire);
    // for maximum performance, choose the larger one.
    double allocRate = std::max(
        static_cast<double>(CangjieRuntime::GetHeapParam().allocationRate) * MB / SECOND_TO_NANO_SECOND, heuAllocRate);
    size_t waitTime = static_cast<size_t>(size / allocRate);
    uint64_t now = TimeUtil::NanoSeconds();
    if (prevRegionAllocTime + waitTime <= now) {
        prevRegionAllocTime = TimeUtil::NanoSeconds();
        return;
    }

    uint64_t sleepTime = std::min<uint64_t>(CangjieRuntime::GetHeapParam().allocationWaitTime,
                                  prevRegionAllocTime + waitTime - now);
    DLOG(ALLOC, "wait %zu ns to alloc %zu(B)", sleepTime, size);
    std::this_thread::sleep_for(std::chrono::nanoseconds{ sleepTime });
    prevRegionAllocTime = TimeUtil::NanoSeconds();
}

} // namespace MapleRuntime

// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Allocator/RegionSpace.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Base/TimeUtils.h"
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Common/ScopedObjectAccess.h"
#include "Common/ColourEncoding.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
MAddress RegionSpace::TryAllocateOnce(size_t allocSize, AllocType allocType)
{
    if (UNLIKELY(allocType == AllocType::PINNED_OBJECT)) {
        return GetRegionManager().AllocPinned(allocSize);
    }
    if (allocSize > ZObjectSizeLimitSmall) {
        return Heap::GetHeap().object_allocator().alloc(allocSize, PageAge::eden);
    }
    if (UNLIKELY(allocSize >= GetRegionManager().GetLargeObjectThreshold())) {
        return GetRegionManager().AllocLarge(
            allocSize, allocType != AllocType::MOVEABLE_OBJECT_SEGMENTED_CLEAR);
    }
    CHECK_DETAIL(allocType != AllocType::MOVEABLE_OBJECT_SEGMENTED_CLEAR,
                 "segmented-clear allocation must be a large object: size=%zu threshold=%zu",
                 allocSize, GetRegionManager().GetLargeObjectThreshold());
    AllocBuffer* allocBuffer = AllocBuffer::GetOrCreateAllocBuffer();
    return allocBuffer->Allocate(allocSize, allocType);
}

MAddress RegionSpace::Allocate(size_t size, AllocType allocType)
{
    uintptr_t internalAddr = 0;
    size_t allocSize = ToAllocSize(size);
    internalAddr = TryAllocateOnce(allocSize, allocType);
    if (UNLIKELY(internalAddr == 0)) {
        // GC workers are strictly non-blocking: inability to obtain a region
        // means this move cannot be completed in the current collection.
        if (IsGcThread()) {
            return 0;
        }
        // Page allocation owns the request through stall and consumption.
        // Reaching this point means that request failed, not a retry promise.
        GetRegionManager().DumpRegionStats("region statistics when gc ends");
        VLOG(REPORT, "Cannot allocate memory of %zu(B), throw an OutOfMemory exception", size);
        LOG(RTLOG_ERROR, "Cannot allocate memory of %zu(B), throw an OutOfMemory exception", size);
        ExceptionManager::OutOfMemory();
        return 0;
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanAllocObject(reinterpret_cast<void *>(internalAddr), allocSize);
#endif
    return internalAddr + HEADER_SIZE;
}

}

namespace MapleRuntime {
RegionManager::RegionManager()
        : freeRegionManager(*this)
    {
        tlabAllocatingThreads.Sample(1);
        tlabRequestedFraction.Sample(0.1);
    }

ZObjectAllocator::ZObjectAllocator()
{
    for (PageAge age : kPageAgeRangeAll) {
        objectAllocators[untype(age)].initialize(age);
    }
}
}
