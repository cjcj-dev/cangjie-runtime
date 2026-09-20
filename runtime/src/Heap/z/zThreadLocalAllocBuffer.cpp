
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

namespace {


bool RegionIsInRelocationSet(const ZPage* reg)
{
    if (reg == nullptr || reg == ZPage::NullRegion()) {
        return false;
    }
    if (reg->IsFromRegion() || reg->IsLoneFromRegion()) {
        return true;
    }
    return forwarding_for_page(reg) != nullptr && !reg->IsForwardingDone();
}

} // namespace


}

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
AllocBuffer* AllocBuffer::GetOrCreateAllocBuffer()
{
    auto* buffer = AllocBuffer::GetAllocBuffer();
    if (buffer == nullptr) {
        buffer = new (std::nothrow) AllocBuffer();
        CHECK_DETAIL(buffer != nullptr, "new region alloc buffer fail");
        buffer->Init();
        ThreadLocal::SetAllocBuffer(buffer);
        RegisterCurrentMarkFlushThread();
    }
    return buffer;
}

AllocBuffer* AllocBuffer::GetAllocBuffer() { return ThreadLocal::GetAllocBuffer(); }

AllocBuffer::~AllocBuffer()
{
    FlushRegion();
}

void AllocBuffer::Init()
{
    static_assert(offsetof(AllocBuffer, tlRegion) == 0,
                  "need to modify the offset of this value in llvm-project at the same time");
    tlRegion = ZPage::NullRegion();
    ThreadLocal::InitializeCleaner();
    auto& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    manager.InitializeTLAB(*this);
    Heap::GetHeap().RegisterAllocBuffer(*this);
}

void AllocBuffer::Fini()
{
    // Finish allocation publications before releasing the current context.
    // Mark stacks and SBB remain owned by the OS thread until its detach.
    if (ThreadLocal::GetAllocBuffer() == this) {
        ThreadLocal::FlushCurrentThreadMarkStacks();
    }
    FlushRegion();
    auto& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    manager.RetireTLABStatistics(*this);
    Heap::GetHeap().RemoveAllocBuffer(*this);
}

// ThreadLocalAllocBuffer::fill (threadLocalAllocBuffer.cpp:201).
void AllocBuffer::SetRegion(ZPage* region)
{
    RetireTLAB(false);
    tlRegion = region;
    if (region == nullptr || region == ZPage::NullRegion()) {
        return;
    }
    const size_t capacity = region->GetAvailableSize();
    tlabStatistics.allocatedSize += capacity;
    tlabRefills.fetch_add(1, std::memory_order_relaxed);
}

// The region top also includes compiler-generated bump allocations. Read it
// before returning the region, rather than counting only the C++ slow path.
void AllocBuffer::RetireTLAB(bool gcWaste)
{
    if (tlRegion == nullptr || tlRegion == ZPage::NullRegion()) {
        return;
    }
    const size_t waste = tlRegion->GetAvailableSize();
    if (gcWaste) {
        tlabStatistics.gcWaste += waste;
    } else {
        tlabStatistics.refillWaste += waste;
    }
    tlRegion = ZPage::NullRegion();
}

void AllocBuffer::ClearRegion()
{
    RetireTLAB(true);
    tlRegion = ZPage::NullRegion();
}

// ThreadLocalAllocBuffer::compute_size (threadLocalAllocBuffer.inline.hpp:57).
// The page allocator returns whole units; its limit is also unit-aligned.
size_t AllocBuffer::ComputeTLABSize(size_t objectSize, size_t maxSize) const
{
    if (objectSize > maxSize || maxSize < ZGranuleSize) {
        return 0;
    }
    constexpr size_t targetRefills = 50; // 100 / (2 * TLABWasteTargetPercent)
    const size_t refills = tlabRefills.load(std::memory_order_relaxed);
    const size_t steps = refills > targetRefills ? std::min((refills - targetRefills) / 8, size_t{4}) : 0;
    size_t desired = desiredTLABSize.load(std::memory_order_relaxed);
    desired = desired > (maxSize >> steps) ? maxSize : desired << steps;
    const size_t size = desired > maxSize - objectSize ? maxSize : desired + objectSize;
    return AlignUp(std::max(size, ZGranuleSize), ZGranuleSize);
}

// ThreadLocalAllocBuffer::accumulate_and_reset_statistics (cpp:78).
void AllocBuffer::ResizeTLAB(size_t capacity, double fallbackFraction, size_t maxSize)
{
    constexpr size_t targetRefills = 50;
    double fraction = tlabAllocationFraction.Average();
    if (fraction == 0) {
        fraction = fallbackFraction;
    }
    const size_t allocation = static_cast<size_t>(fraction * capacity);
    const size_t desired = std::min(std::max(allocation / targetRefills, ZGranuleSize), maxSize);
    desiredTLABSize.store(AlignUp(desired, ZGranuleSize), std::memory_order_relaxed);
}

MAddress AllocBuffer::Allocate(size_t totalSize, AllocType allocType)
{
    // a hoisted specific fast path which can be inlined
    MAddress addr = 0;
    if (UNLIKELY(allocType == AllocType::RAW_POINTER_OBJECT)) {
        return AllocateRawPointerObject(totalSize);
    }

    // csetalloc: never bump into a region already in the relocation set.
    // Mirror pin path's "no reuse after POST_TRACE" rule (RegionManager.cpp free-list).
    // If tlRegion was reclassified to FROM while we still hold it, retire and slow-path.
    if (UNLIKELY(tlRegion != ZPage::NullRegion() && RegionIsInRelocationSet(tlRegion))) {
        // FROM/LONE_FROM already lost the thread-local role — only drop the local shortcut.
        // Still-THREAD_LOCAL but routing: flush to recentFull so it can be handled by GC lists.
        if (tlRegion->IsThreadLocalRegion()) {
            RegionSpace& theAllocator = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
            RegionManager& manager = theAllocator.GetRegionManager();
            manager.RemoveThreadLocalRegion(tlRegion);
            manager.EnlistFullThreadLocalRegion(tlRegion);
        }
        ClearRegion();
    }

    if (LIKELY(tlRegion != ZPage::NullRegion())) {
        addr = tlRegion->alloc_object(totalSize);
    }

    if (UNLIKELY(addr == 0)) {
        addr = AllocateImpl(totalSize, allocType);
    }

    if (addr != 0) {
        // The slow path can allocate outside the TLAB in a shared CPU page.
        ZPage* reg = Heap::page(addr);
        // twoflags: POST_TRACE+ allocs have no mark/isTrace coverage — stamp CSet exclusion.
        // TRACE-phase new regions already get isTraceRegion (implicit black). Do not stamp
        // TRACE (would exclude most young regions until next major → minor starvation).
        // ⛔ No CLEAR_SATB (minor shares it). Orthogonal to isTraceRegion / ShouldEnqueue.
        (void)reg;
    }
    DLOG(ALLOC, "alloc 0x%zx(%zu)", addr, totalSize);
    return addr;
}

// try an allocation but do not handle failure
MAddress AllocBuffer::AllocateImpl(size_t totalSize, AllocType allocType)
{
    RegionSpace& theAllocator = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RegionManager& manager = theAllocator.GetRegionManager();

    // MemAllocator::mem_allocate_inside_tlab_slow / outside_tlab: an
    // unrepresentable TLAB request belongs to the object allocator (eden).
    const size_t tlabSize = ComputeTLABSize(totalSize, manager.GetThreadLocalRegionSize());
    if (tlabSize == 0) {
        return Heap::GetHeap().object_allocator().alloc(totalSize, PageAge::eden);
    }

    // allocate from thread local region
    if (LIKELY(tlRegion != ZPage::NullRegion())) {
        if (UNLIKELY(RegionIsInRelocationSet(tlRegion))) {
            if (tlRegion->IsThreadLocalRegion()) {
                manager.RemoveThreadLocalRegion(tlRegion);
                manager.EnlistFullThreadLocalRegion(tlRegion);
            }
            ClearRegion();
        } else {
            MAddress addr = tlRegion->alloc_object(totalSize);
            if (addr != 0) {
                return addr;
            }

            // allocation failed because region is full.
            CHECK(tlRegion->IsThreadLocalRegion());
            {
                manager.RemoveThreadLocalRegion(tlRegion);
                manager.EnlistFullThreadLocalRegion(tlRegion);
                RetireTLAB(false);
            }
        }
    }

    // now region must be null. If a region has been ready, then use it and tell gc-assitant thread to prepare
    // a new region, or take a new one.
    ZPage* r  = preparedRegion.load(std::memory_order_acquire);
    if (r != nullptr && r->GetAvailableSize() >= totalSize) {
        preparedRegion.store(nullptr, std::memory_order_release);
        if (UNLIKELY(RegionIsInRelocationSet(r))) {
            // prepared region must not be a CSet member; reclaim path via flush semantics.
            if (r->IsThreadLocalRegion()) {
                manager.RemoveThreadLocalRegion(r);
            }
            manager.ReclaimRegion(r);
            r = nullptr;
        } else {
            SetRegion(r);
            if (theAllocator.IsAsyncAllocationEnable()) {
                theAllocator.AddHungryBuffer(*this);
                Heap::GetHeap().GetFinalizerProcessor().NotifyToFeedAllocBuffers();
            }
            return r->alloc_object(totalSize);
        }
    }
    // AllocateThreadLocalRegion is a safepoint, in which cj thread rescheule may happen.
    // tlRegion is bound to specific thread, so we need to forbid reschedule.
    CJThreadPreemptOffCntAdd();
    r = manager.AllocateThreadLocalRegion(tlabSize);
    CJThreadPreemptOffCntSub();
    if (UNLIKELY(r == nullptr)) {
        return Heap::GetHeap().object_allocator().alloc(totalSize, PageAge::eden);
    }
    // tlRegion may be set in PreforwardPhase handler while allocating region.
    // Null region means tlRegion is not set.
    if (tlRegion == ZPage::NullRegion()) {
        SetRegion(r);
        return r->alloc_object(totalSize);
    }
    // tlRegion has been set in preforward phase.
    MAddress addr = tlRegion->alloc_object(totalSize);
    if (addr != 0) {
        if (!SetPreparedRegion(r)) {
            manager.UndoThreadLocalRegionAllocation(r);
        }
        return addr;
    }
    // tlRegion is not enough for allocation, so we use r.
    manager.RemoveThreadLocalRegion(tlRegion);
    manager.EnlistFullThreadLocalRegion(tlRegion);
    SetRegion(r);
    return r->alloc_object(totalSize);
}

MAddress AllocBuffer::AllocateRawPointerObject(size_t totalSize)
{
    ZPage* region = tlRawPointerRegions.empty() ? nullptr : tlRawPointerRegions.back();
    if (region != nullptr) {
        MAddress allocAddr = region->alloc_object(totalSize);
        if (allocAddr != 0) {
            return allocAddr;
        }
    }
    RegionManager& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    size_t pageSize = AlignUp(totalSize, ZGranuleSize);
    if (totalSize <= manager.GetThreadLocalRegionSize()) {
        region = Heap::alloc_page(pageSize, ZPageType::small);
        if (region == nullptr) {
            return 0;
        }
        region->SetRegionRole(ZPageRole::RawPointerStaging);
        tlRawPointerRegions.push_back(region);
    } else {
        region = Heap::alloc_page(pageSize, ZPageType::large);
        if (region == nullptr) {
            return 0;
        }
        region->SetRegionRole(ZPageRole::RawPointerStaging);
        tlLargeRawPointerRegions.push_back(region);
    }

    // region is enough for totalSize.
    MAddress allocAddr = region->alloc_object(totalSize);
    MRT_ASSERT(allocAddr != 0, "allocation failure");
    return allocAddr;
}

void AllocBuffer::CommitRawPointerRegions()
{
    RegionManager& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    manager.MergeRawPointerRegions(tlRawPointerRegions, tlLargeRawPointerRegions);
}

}

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
void AllocBuffer::FlushRegion()
{
    if (LIKELY(tlRegion != ZPage::NullRegion()) && tlRegion != nullptr) {
        RegionSpace& theAllocator = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
        RegionManager& manager = theAllocator.GetRegionManager();
        manager.RemoveThreadLocalRegion(tlRegion);
        manager.EnlistFullThreadLocalRegion(tlRegion);
        ClearRegion();
    }
    ZPage* prepared = preparedRegion.load();
    if (LIKELY(prepared != ZPage::NullRegion()) && prepared != nullptr) {
        RegionSpace& theAllocator = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
        RegionManager& manager = theAllocator.GetRegionManager();
        manager.RemoveThreadLocalRegion(prepared);
        if (prepared->IsEmpty()) {
            manager.ReclaimRegion(prepared);
        } else {
            manager.EnlistFullThreadLocalRegion(prepared);
        }
        preparedRegion.store(nullptr, std::memory_order_release);
    }
}
}
