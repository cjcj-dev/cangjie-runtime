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
#include "Heap/shared/collectedHeap.hpp"
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
// ThreadLocalAllocBuffer::initial_desired_size (cpp:265): a new thread
// starts with the published allocation fraction instead of a fixed extent.
void RegionManager::InitializeTLAB(AllocBuffer& buffer)
{
    std::lock_guard<std::mutex> lock(tlabStatisticsLock);
    const size_t threads = std::max(static_cast<size_t>(tlabAllocatingThreads.Average() + 0.5), size_t{1});
    buffer.ResizeTLAB(GetTLABCapacity(), tlabRequestedFraction.Average() / threads,
                      ZObjectSizeLimitSmall);
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
void RegionManager::PublishTLABStatistics(const TLABStatistics& statistics)
{
    std::lock_guard<std::mutex> lock(tlabStatisticsLock);
    const size_t capacity = GetTLABCapacity();
    TLABStatistics total = retiredTLABStatistics;
    retiredTLABStatistics = TLABStatistics{};
    total.Update(statistics);
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

// ZThreadLocalAllocBuffer::retire, zThreadLocalAllocBuffer.cpp:65-73.
// The caller owns the mutator (watermark processing or thread exit).
void RegionManager::RetireTLAB(AllocBuffer& buffer, TLABStatistics& statistics)
{
    std::lock_guard<std::mutex> lock(tlabStatisticsLock);
    statistics = TLABStatistics{};
    buffer.RetireTLAB(true);
    buffer.AccumulateTLABStatistics(statistics, GetTLABUsed(), GetTLABCapacity());
    const size_t threads = std::max(static_cast<size_t>(tlabAllocatingThreads.Average() + 0.5), size_t{1});
    buffer.ResizeTLAB(GetTLABCapacity(), tlabRequestedFraction.Average() / threads, ZObjectSizeLimitSmall);
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
ZPage* RegionManager::AllocateSharedPage(size_t size, ZPageType role,
                                             PageAge age, ZAllocationFlags flags, bool clearPayload)
{
    ZPage* page = Heap::alloc_page(size, role, false, !flags.non_blocking(), clearPayload, age, flags);
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
        const size_t pageBytes = page->GetRegionSize();
        const size_t index = page->granule_index();
        page->RetirePageMemory();
        ReturnRetiredPageMemory(PageMemory{index, pageBytes, 0, true}, false);
    });
}

// ZGC zObjectAllocator.cpp:56-64.
ZPage* ZObjectAllocator::PerAge::alloc_page(ZPageType type, size_t size, ZAllocationFlags flags,
                                          bool clearPayload)
{
    return Heap::GetHeap().page_allocator().AllocateSharedPage(size, type, age, flags, clearPayload);
}

void ZObjectAllocator::PerAge::undo_alloc_page(ZPage* page)
{
    Heap::GetHeap().page_allocator().UndoSharedPage(page);
}

// ZGC zObjectAllocator.cpp:66-110: reserve the object before publishing its page.
uintptr_t ZObjectAllocator::PerAge::alloc_object_in_shared_page(
    ZPage** shared, ZPageType type, size_t pageSize, size_t size, ZAllocationFlags flags)
{
    ZPage* page = __atomic_load_n(shared, __ATOMIC_ACQUIRE);
    uintptr_t addr = page == nullptr ? 0 : page->alloc_object_atomic(size);
    if (addr != 0) { return addr; }
    ZPage* fresh = alloc_page(type, pageSize, flags);
    if (fresh == nullptr) { return 0; }
    addr = fresh->alloc_object(size);
    CHECK(addr != 0);
    for (;;) {
        if (__atomic_compare_exchange_n(shared, &page, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return addr;
        }
        if (page == nullptr) { continue; }
        const uintptr_t previous = page->alloc_object_atomic(size);
        if (previous == 0) { continue; }
        undo_alloc_page(fresh);
        return previous;
    }
}

// ZGC zObjectAllocator.cpp:112-158: page-local, locked non-blocking, unlocked blocking.
uintptr_t ZObjectAllocator::PerAge::alloc_object_in_medium_page(size_t size, ZAllocationFlags flags)
{
    ZPage** shared = shared_medium_page_addr();
    ZPage* page = __atomic_load_n(shared, __ATOMIC_ACQUIRE);
    uintptr_t addr = page == nullptr ? 0 : page->alloc_object_atomic(size);
    if (addr == 0) {
        std::lock_guard<ZLock> lock(mediumPageAllocLock);
        ZAllocationFlags nonBlocking = flags;
        nonBlocking.set_non_blocking();
        if (ZPageSizeMediumMin != ZPageSizeMediumMax) {
            CHECK(ZPageSizeMediumEnabled);
            ZAllocationFlags fastMedium = nonBlocking;
            fastMedium.set_fast_medium();
            addr = alloc_object_in_shared_page(shared, ZPageType::medium, ZPageSizeMediumMax, size, fastMedium);
        }
        if (addr == 0) {
            addr = alloc_object_in_shared_page(shared, ZPageType::medium, ZPageSizeMediumMax, size, nonBlocking);
        }
    }
    if (addr == 0 && !flags.non_blocking()) {
        addr = alloc_object_in_shared_page(shared, ZPageType::medium, ZPageSizeMediumMax, size, flags);
    }
    return addr;
}

uintptr_t ZObjectAllocator::PerAge::alloc_large_object(size_t size, ZAllocationFlags flags, bool clearPayload)
{
    ZPage* page = alloc_page(ZPageType::large, AlignUp(size, ZGranuleSize), flags, clearPayload);
    return page == nullptr ? 0 : page->alloc_object(size);
}

uintptr_t ZObjectAllocator::PerAge::alloc_medium_object(size_t size, ZAllocationFlags flags)
{
    return alloc_object_in_medium_page(size, flags);
}

uintptr_t ZObjectAllocator::PerAge::alloc_small_object(size_t size, ZAllocationFlags flags)
{
    return alloc_object_in_shared_page(shared_small_page_addr(), ZPageType::small, ZPageSizeSmall, size, flags);
}

uintptr_t ZObjectAllocator::PerAge::alloc_object(size_t size, ZAllocationFlags flags, bool clearPayload)
{
    if (size <= ZObjectSizeLimitSmall) {
        return alloc_small_object(size, flags);
    } else if (size <= ZObjectSizeLimitMedium) {
        return alloc_medium_object(size, flags);
    } else {
        return alloc_large_object(size, flags, clearPayload);
    }
}

// ZGC zObjectAllocator.cpp:228-239.
size_t ZObjectAllocator::fast_available(PageAge age) const
{
    ZPage* const* shared = allocator(age)->shared_small_page_addr();
    ZPage* page = __atomic_load_n(shared, __ATOMIC_ACQUIRE);
    return page == nullptr ? 0 : page->remaining();
}

uintptr_t ZObjectAllocator::alloc(size_t size, PageAge age, bool nonBlocking, bool clearPayload)
{
    CHECK(untype(age) < kPageAgeCount);
    ZAllocationFlags flags;
    if (nonBlocking) { flags.set_non_blocking(); }
    return allocator(age)->alloc_object(size, flags, clearPayload);
}

// ZObjectAllocator::retire_pages / PerAge::retire_pages (cpp:208-237).
// Called in the corresponding generation's mark-start pause. The lifecycle
// lists retain pages; retirement only removes allocation shortcuts.
void ZObjectAllocator::retire_pages(PageAgeRange ages)
{
    // zObjectAllocator.cpp:198-203 PerAge::retire_pages: set_all(nullptr).
    for (PageAge age : ages) {
        auto* perAge = allocator(age);
        perAge->sharedSmallPage.set_all(nullptr);
        perAge->sharedMediumPage.set(nullptr);
    }
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
    // HotSpot memAllocator.cpp:327-347: both TLAB attempts precede the
    // outside-TLAB allocation. A failed refill is not yet an allocation failure.
    if (allocSize <= ZObjectSizeLimitSmall && ThreadLocal::GetMutator() != nullptr) {
        AllocBuffer* allocBuffer = ThreadLocal::GetMutator()->tlab();
        MAddress addr = allocBuffer->Allocate(allocSize, allocType);
        if (addr != 0) { return addr; }
        addr = allocBuffer->AllocateImpl(allocSize, allocType);
        if (addr != 0) { return addr; }
    }
    return AllocateOutsideTLAB(allocSize, allocType);
}

// HotSpot memAllocator.cpp:235-247: one outside-TLAB allocation operation.
MAddress RegionSpace::AllocateOutsideTLAB(size_t allocSize, AllocType allocType)
{
    return Heap::GetHeap().object_allocator().alloc(allocSize, PageAge::eden, false,
        allocType != AllocType::MOVEABLE_OBJECT_SEGMENTED_CLEAR);
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
