// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zObjectAllocator.hpp"

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
#include "Heap/Collector/CopyCollector.h"
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
    // ZGenerationYoung::mark_start, zGeneration.cpp:865.
    RetireSharedPages(kPageAgeRangeYoung);
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

// zValue.inline.hpp:80-89 / zCPU.cpp:69-81. Use the configured CPU domain,
// not the caller's affinity-mask population (CPU ids can be sparse).
size_t RegionManager::SharedPageCPUCount()
{
    static const size_t count = [] {
#if defined(__linux__) || defined(hongmeng)
        const long configured = sysconf(_SC_NPROCESSORS_CONF);
        if (configured > 0) { return static_cast<size_t>(configured); }
#endif
        return static_cast<size_t>(std::max(1U, std::thread::hardware_concurrency()));
    }();
    return count;
}

size_t RegionManager::CurrentSharedPageCPU()
{
#if defined(__linux__) || defined(hongmeng)
    const int cpu = sched_getcpu();
    if (cpu >= 0 && static_cast<size_t>(cpu) < SharedPageCPUCount()) {
        return static_cast<size_t>(cpu);
    }
#elif defined(_WIN64)
    // os_windows.cpp:1090: Windows reports the current processor number.
    const size_t cpu = static_cast<size_t>(GetCurrentProcessorNumber());
    if (cpu < SharedPageCPUCount()) { return cpu; }
#elif defined(__APPLE__) && defined(__x86_64__)
    // os_bsd.cpp:2228-2261: compact the initial APIC id into the CPU domain.
    struct ProcessorMap {
        std::atomic<int> ids[256];
        std::atomic<unsigned> next{0};
        ProcessorMap() { for (auto& id : ids) { id.store(-1, std::memory_order_relaxed); } }
    };
    static ProcessorMap processors;
    unsigned eax = 1, ebx = 0, ecx = 0, edx = 0;
    __asm__("cpuid" : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx));
    auto& entry = processors.ids[(ebx >> 24) & 255];
    int cpu = entry.load(std::memory_order_acquire);
    while (cpu < 0) {
        int expected = -1;
        if (entry.compare_exchange_strong(expected, -2, std::memory_order_acq_rel)) {
            cpu = static_cast<int>(processors.next.fetch_add(1, std::memory_order_relaxed) % SharedPageCPUCount());
            entry.store(cpu, std::memory_order_release);
        } else {
            cpu = entry.load(std::memory_order_acquire);
        }
    }
    return static_cast<size_t>(cpu);
#endif
    // os_bsd.cpp:2262-2266 / os_linux.cpp:4994-5009: unsupported or invalid
    // processor ids share slot zero; every page allocation remains atomic.
    return 0;
}

RegionManager::PerAgeObjectAllocator::PerAgeObjectAllocator(PageAge pageAge)
    : age(pageAge), smallPages(new SharedSmallPage[SharedPageCPUCount()]) {}

// zHeap.cpp:229: shared-page TLAB accounting includes only small eden pages.
static bool IsSmallEdenPage(const RegionInfo* page)
{
    return page->IsSmallRegion() && page->IsYoungRegion() &&
           page->GetYoungAge() == static_cast<uint8_t>(untype(PageAge::eden));
}

// ZObjectAllocator::PerAge::alloc_page, ZHeap::alloc_page/account_alloc_page.
RegionInfo* RegionManager::AllocateSharedPage(size_t units, RegionInfo::UnitRole role,
                                             PageAge age, bool nonBlocking)
{
    RegionInfo* page = TakeRegion(units, role, false, !nonBlocking, true, age);
    if (page == nullptr) { return nullptr; }
    page->SetYoungRegionFlag(age != PageAge::old);
    page->SetYoungAge(age == PageAge::old ? 0 : static_cast<uint8_t>(untype(age)));
    if (IsSmallEdenPage(page)) {
        tlabUsed.fetch_add(page->GetRegionSize(), std::memory_order_relaxed);
    }
    const GCPhase phase = Heap::GetHeap().GetGCPhase(page->IsYoungRegion() ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD);
    if (phase == GC_PHASE_TRACE || phase == GC_PHASE_CLEAR_SATB_BUFFER) {
        page->SetTraceRegionFlag(1);
    }
    if (phase == GC_PHASE_POST_TRACE || phase == GC_PHASE_PREFORWARD || phase == GC_PHASE_FORWARD) {
        page->SetNotRelocatableThisCycle(1);
    }
    // Register with the page lifecycle, never with tlRegionList. Registration
    // precedes object allocation, as RegionList's byte accounting requires.
    if (role == RegionInfo::UnitRole::LARGE_SIZED_UNITS) {
        recentLargeRegionList.PrependRegion(page, RegionInfo::RegionType::RECENT_LARGE_REGION);
    } else {
        recentFullRegionList.PrependRegion(page, RegionInfo::RegionType::RECENT_FULL_REGION);
        RecentFullAccounting::Enqueue(1, page->GetUnitCount());
    }
    return page;
}

// ZObjectAllocator::PerAge::undo_alloc_page: this unpublished candidate was
// never used by a caller. Undo its page charge, not a TLAB's ownership.
void RegionManager::UndoSharedPage(RegionInfo* page)
{
    recentFullRegionList.DeleteRegion(page);
    RecentFullAccounting::Dequeue(1, page->GetUnitCount());
    if (IsSmallEdenPage(page)) {
        tlabUsed.fetch_sub(page->GetRegionSize(), std::memory_order_relaxed);
    }
    // ZHeap::undo_alloc_page: remove the unused page-table entry and return
    // the extent without suspending a caller holding an unpublished object.
    RegionInfo::RetirePage(page, [this, page] {
        const size_t units = page->GetUnitCount();
        const size_t index = page->GetUnitIdx();
        if (units >= HUGE_PAGE) { UntagHugePage(page, units); }
        page->InitFreeUnits();
        ReturnRetiredPageMemory(PageMemory{index, units, 0, true}, false);
    });
}

uintptr_t RegionManager::AllocSharedObject(size_t size, PageAge age, bool nonBlocking)
{
    CHECK(untype(age) < kPageAgeCount);
    PerAgeObjectAllocator& allocator = *objectAllocators[untype(age)];
    if (size > GetLargeObjectThreshold()) {
        // ZObjectAllocator::PerAge::alloc_large_object. This runtime has no
        // medium page class; objects above its small limit use dedicated pages.
        const size_t units = AlignUp(size, RegionInfo::UNIT_SIZE) / RegionInfo::UNIT_SIZE;
        RegionInfo* page = AllocateSharedPage(units, RegionInfo::UnitRole::LARGE_SIZED_UNITS,
                                               allocator.age, nonBlocking);
        return page == nullptr ? 0 : page->Alloc(size);
    }
    // ZObjectAllocator::PerAge::shared_small_page_addr / alloc_small_object.
    // Keep this stable slot address across refill; a safepoint can retire its
    // value, and the compare-exchange below explicitly handles that case.
    auto& shared = allocator.smallPages[CurrentSharedPageCPU()].page;
    RegionInfo* page = shared.load(std::memory_order_acquire);
    uintptr_t addr = page == nullptr ? 0 : page->AtomicAlloc(size);
    if (addr != 0) { return addr; }

    // zObjectAllocator.cpp:78-116: allocate before publishing the candidate,
    // retry after retirement or exhaustion, and undo a losing page allocation.
    RegionInfo* fresh = AllocateSharedPage(maxUnitCountPerRegion, RegionInfo::UnitRole::SMALL_SIZED_UNITS,
                                           allocator.age, nonBlocking);
    if (fresh == nullptr) { return 0; }
    addr = fresh->Alloc(size);
    CHECK(addr != 0);
    for (;;) {
        if (shared.compare_exchange_strong(page, fresh, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            return addr;
        }
        if (page == nullptr) { continue; }
        const uintptr_t previous = page->AtomicAlloc(size);
        if (previous == 0) { continue; }
        UndoSharedPage(fresh);
        return previous;
    }
}

// ZObjectAllocator::retire_pages / PerAge::retire_pages (cpp:208-237).
// Called in the corresponding generation's mark-start pause. The lifecycle
// lists retain pages; retirement only removes allocation shortcuts.
void RegionManager::RetireSharedPages(PageAgeRange ages)
{
    for (PageAge age : ages) {
        for (size_t cpu = 0; cpu < SharedPageCPUCount(); ++cpu) {
            objectAllocators[untype(age)]->smallPages[cpu].page.store(nullptr, std::memory_order_release);
        }
    }
}

RegionInfo* RegionManager::AllocateThreadLocalRegion(size_t size, bool expectPhysicalMem, bool youngRegion,
                                                   bool allowSaferegion)
{
    // ZHeap::max_tlab_size / unsafe_max_tlab_alloc (zHeap.cpp:144-160):
    // the caller computes the refill size; the allocator enforces its extent.
    if (size == 0 || size > GetThreadLocalRegionSize()) {
        return nullptr;
    }
    const size_t units = AlignUp(size, RegionInfo::UNIT_SIZE) / RegionInfo::UNIT_SIZE;
    RegionInfo* region = TakeRegion(units, RegionInfo::UnitRole::SMALL_SIZED_UNITS, expectPhysicalMem,
                                    allowSaferegion, true, youngRegion ? PageAge::eden : PageAge::old);
    if (region != nullptr) {
        {
            region->SetYoungRegionFlag(youngRegion ? 1 : 0);
            if (youngRegion) {
                // zHeap.cpp:233: charge the backing extent even before a
                // prepared region is installed as a thread's current TLAB.
                tlabUsed.fetch_add(region->GetRegionSize(), std::memory_order_relaxed);
            }
            region->SetYoungAge(0);
            GCPhase phase = Heap::GetHeap().GetGCPhase(region->IsYoungRegion() ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD);
            if (phase == GC_PHASE_TRACE || phase == GC_PHASE_CLEAR_SATB_BUFFER) {
                region->SetTraceRegionFlag(1);
            }
            // twoflags: POST_TRACE+ only (TRACE uses isTraceRegion). No CLEAR_SATB.
            if (phase == GC_PHASE_POST_TRACE || phase == GC_PHASE_PREFORWARD ||
                phase == GC_PHASE_FORWARD) {
                region->SetNotRelocatableThisCycle(1);
            }
            tlRegionList.PrependRegion(region, RegionInfo::RegionType::THREAD_LOCAL_REGION);
            DLOG(REGION, "alloc tl-region %p @[0x%zx+%zu, 0x%zx) units[%zu+%zu, %zu) type %u",
                region, region->GetRegionStart(), region->GetRegionSize(), region->GetRegionEnd(),
                region->GetUnitIdx(), region->GetUnitCount(), region->GetUnitIdx() + region->GetUnitCount(),
                region->GetRegionType());
        }
    }

    return region;
}

// ZHeap::undo_alloc_page (zHeap.cpp:270): only a failed publication of
// a newly allocated, unused backing region cancels its allocation charge.
// GC retirement/reclamation is not an undo and must retain that cycle's usage.
void RegionManager::UndoThreadLocalRegionAllocation(RegionInfo* region)
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
    GCStats& gcstats = heap.GetCollector().GetGCStats();
    size_t allocatedBytes = GetAllocatedSize() - gcstats.liveBytesAfterGC;
    constexpr double pi = 3.14;
    size_t availableBytesAfterGC = heap.GetMaxCapacity() - gcstats.liveBytesAfterGC;
    double heuAllocRate = std::cos((pi / 2.0) * allocatedBytes / availableBytesAfterGC) * gcstats.collectionRate;
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
        return regionManager.AllocPinned(allocSize);
    }
    if (UNLIKELY(allocSize >= regionManager.GetLargeObjectThreshold())) {
        return regionManager.AllocLarge(
            allocSize, allocType != AllocType::MOVEABLE_OBJECT_SEGMENTED_CLEAR);
    }
    CHECK_DETAIL(allocType != AllocType::MOVEABLE_OBJECT_SEGMENTED_CLEAR,
                 "segmented-clear allocation must be a large object: size=%zu threshold=%zu",
                 allocSize, regionManager.GetLargeObjectThreshold());
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
        regionManager.DumpRegionStats("region statistics when gc ends");
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
        : freeRegionManager(*this), tlRegionList("thread local regions"), recentFullRegionList("recent full regions"),
          fullTraceRegions("full trace regions"), fromRegionList("from regions"),
          ghostFromRegionList("ghost from regions"), unmovableFromRegionList("escaped from regions"),
          garbageRegionList("garbage regions"), recentPinnedRegionList("recent pinned regions"),
          oldPinnedRegionList("old pinned regions"), rawPointerPinnedRegionList("raw pointer pinned regions"),
          oldLargeRegionList("old large regions"), recentLargeRegionList("recent large regions"),
          largeTraceRegions("large trace regions")
    {
        for (PageAge age : kPageAgeRangeAll) {
            objectAllocators[untype(age)] = std::make_unique<PerAgeObjectAllocator>(age);
        }
        tlabAllocatingThreads.Sample(1);
        tlabRequestedFraction.Sample(0.1);
    }
}
