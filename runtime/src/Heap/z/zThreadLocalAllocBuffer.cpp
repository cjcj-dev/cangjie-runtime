
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
#include "Heap/Verify/AllocPhaseDiag.h"
#include "Heap/Verify/MinorGCALot.h"
#include "Heap/Verify/Zap.h"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
extern std::atomic<size_t> g_allocIntoCSetRetired;
void NoteAllocIntoCSet(RegionInfo* reg, const char* where);

namespace {


bool RegionIsInRelocationSet(const RegionInfo* reg)
{
    if (reg == nullptr || reg == RegionInfo::NullRegion()) {
        return false;
    }
    if (reg->IsFromRegion() || reg->IsLoneFromRegion()) {
        return true;
    }
    return ForwardingTable::RetainPageOwner(reg).get() != nullptr && !reg->IsForwardingDone();
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
#include "Heap/Verify/AllocPhaseDiag.h"
#include "Heap/Verify/MinorGCALot.h"
#include "Heap/Verify/Zap.h"
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
    tlRegion = RegionInfo::NullRegion();
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
void AllocBuffer::SetRegion(RegionInfo* region)
{
    RetireTLAB(false);
    tlRegion = region;
    if (region == nullptr || region == RegionInfo::NullRegion()) {
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
    if (tlRegion == nullptr || tlRegion == RegionInfo::NullRegion()) {
        return;
    }
    const size_t waste = tlRegion->GetAvailableSize();
    if (gcWaste) {
        tlabStatistics.gcWaste += waste;
    } else {
        tlabStatistics.refillWaste += waste;
    }
    tlRegion = RegionInfo::NullRegion();
}

void AllocBuffer::ClearRegion()
{
    RetireTLAB(true);
}

// ThreadLocalAllocBuffer::compute_size (threadLocalAllocBuffer.inline.hpp:57).
// The page allocator returns whole units; its limit is also unit-aligned.
size_t AllocBuffer::ComputeTLABSize(size_t objectSize, size_t maxSize) const
{
    if (objectSize > maxSize || maxSize < RegionInfo::UNIT_SIZE) {
        return 0;
    }
    constexpr size_t targetRefills = 50; // 100 / (2 * TLABWasteTargetPercent)
    const size_t refills = tlabRefills.load(std::memory_order_relaxed);
    const size_t steps = refills > targetRefills ? std::min((refills - targetRefills) / 8, size_t{4}) : 0;
    size_t desired = desiredTLABSize.load(std::memory_order_relaxed);
    desired = desired > (maxSize >> steps) ? maxSize : desired << steps;
    const size_t size = desired > maxSize - objectSize ? maxSize : desired + objectSize;
    return AlignUp(std::max(size, RegionInfo::UNIT_SIZE), RegionInfo::UNIT_SIZE);
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
    const size_t desired = std::min(std::max(allocation / targetRefills, RegionInfo::UNIT_SIZE), maxSize);
    desiredTLABSize.store(AlignUp(desired, RegionInfo::UNIT_SIZE), std::memory_order_relaxed);
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
    if (UNLIKELY(tlRegion != RegionInfo::NullRegion() && RegionIsInRelocationSet(tlRegion))) {
        NoteAllocIntoCSet(tlRegion, "fast-retire");
        g_allocIntoCSetRetired.fetch_add(1, std::memory_order_relaxed);
        // FROM/LONE_FROM are already off tlRegionList — only drop the local shortcut.
        // Still-THREAD_LOCAL but routing: flush to recentFull so it can be handled by GC lists.
        if (tlRegion->IsThreadLocalRegion()) {
            RegionSpace& theAllocator = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
            RegionManager& manager = theAllocator.GetRegionManager();
            manager.RemoveThreadLocalRegion(tlRegion);
            manager.EnlistFullThreadLocalRegion(tlRegion);
        }
        ClearRegion();
    }

    if (LIKELY(tlRegion != RegionInfo::NullRegion())) {
        addr = tlRegion->Alloc(totalSize);
    }

    if (UNLIKELY(addr == 0)) {
        addr = AllocateImpl(totalSize, allocType);
    }

    // gcvroot Z3: poison new object bytes before header install (MRT_GCV2_ZAP_ALLOC=1).
    if (addr != 0) {
        HeapZap::ZapAllocated(addr, totalSize);
        // The slow path can allocate outside the TLAB in a shared CPU page.
        RegionInfo* reg = RegionInfo::TryGetRegionInfoAt(addr);
        // twoflags: POST_TRACE+ allocs have no mark/isTrace coverage — stamp CSet exclusion.
        // TRACE-phase new regions already get isTraceRegion (implicit black). Do not stamp
        // TRACE (would exclude most young regions until next major → minor starvation).
        // ⛔ No CLEAR_SATB (minor shares it). Orthogonal to isTraceRegion / ShouldEnqueue.
        if (reg != nullptr && !reg->IsNotRelocatableThisCycle()) {
            GCPhase heapP = Heap::GetHeap().GetGCPhase();
            if (heapP == GCPhase::GC_PHASE_POST_TRACE || heapP == GCPhase::GC_PHASE_PREFORWARD ||
                heapP == GCPhase::GC_PHASE_FORWARD) {
                reg->SetNotRelocatableThisCycle(1);
            }
        }
        // marklate: per-region last-alloc phase (NULLROUTE_DIAG only; no TLS).
        // blackmark: also stamp isTraceRegion at alloc for H3.
        if (AllocPhaseDiag::Enabled()) {
            uint8_t mutP = static_cast<uint8_t>(GCPhase::GC_PHASE_UNDEF);
            Mutator* m = Mutator::GetMutator();
            if (m != nullptr) {
                mutP = static_cast<uint8_t>(m->GetMutatorPhase());
            }
            uint8_t heapP = static_cast<uint8_t>(Heap::GetHeap().GetGCPhase());
            uintptr_t regionStart = 0;
            uintptr_t regionEnd = 0;
            uint8_t isTrace = 0;
            if (reg != nullptr) {
                regionStart = reg->GetRegionStart();
                regionEnd = reg->GetRegionEnd();
                isTrace = reg->IsTraceRegion() ? 1 : 0;
            }
            AllocPhaseDiag::Record(reinterpret_cast<void*>(addr), regionStart, regionEnd, mutP, heapP, isTrace);
        }
        // youngconc allocate-black: paint mark bits + grey-list for TRACE/CLEAR
        // window young allocs. Ordinary MOVEABLE alloc never MarkNewObject; pin reuse did
        // MarkObject. GetRoute reads ghost liveInfo0, so also mark that face when present.
        // The Follow receipt below makes the object part of mark termination rather than
        // relying on a pause-local post-mark scan.
        // isTraceRegion alone makes ShouldEnqueue skip SATB; without
        // paint those objects stay live0Surv=0 at route under concurrent young mark.
        {
            if (reg != nullptr && !reg->IsLargeRegion()) {
                GCPhase mutP = GCPhase::GC_PHASE_UNDEF;
                Mutator* m = Mutator::GetMutator();
                if (m != nullptr) {
                    mutP = m->GetMutatorPhase();
                }
                GCPhase heapP = Heap::GetHeap().GetGCPhase();
                // concurrent mark window (TRACE/CLEAR) + young region.
                // Also paint when isTraceRegion (ShouldEnqueue skip) even if mutator phase lags.
                // Do not paint POST_TRACE/FORWARD (evacuate STW; csetalloc owns that surface).
                const bool inConcMark = (heapP == GCPhase::GC_PHASE_TRACE ||
                                         heapP == GCPhase::GC_PHASE_CLEAR_SATB_BUFFER ||
                                         mutP == GCPhase::GC_PHASE_TRACE ||
                                         mutP == GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
                const bool needBlack = reg->IsYoungRegion() && (inConcMark || reg->IsTraceRegion());
                if (needBlack) {
                    MAddress regionStart = reg->GetRegionStart();
                    MAddress regionEnd = reg->GetRegionEnd();
                    size_t offset = static_cast<size_t>(addr - regionStart);
                    size_t regionSize = static_cast<size_t>(regionEnd - regionStart);
                    if (totalSize > 0 && (totalSize % 8) == 0 && offset + totalSize <= regionSize) {

                        MarkView<Generation::Young> view = reg->GetMarkView<Generation::Young>();
                        reg->VerifyMarkFaceOwner<Generation::Young>(
                            reinterpret_cast<BaseObject*>(addr), "RegionSpace::AllocBlack.live");
                        bool incLive = false;
                        (void)reg->GetOrAllocMarkBitmap(view)->MarkBits(offset, totalSize, regionSize, incLive);
                        if (incLive) {
                            reg->AddLiveCounts(1, totalSize);
                        }
                        LiveInfo* ghost = reg->GetLiveInfo0ForProbe();
                        RegionBitmap* ghostBitmap = ghost == nullptr ? nullptr : reg->GetOwnerMarkBitmap(ghost);
                        if (ghost != nullptr && ghostBitmap != nullptr) {

                            (void)ghostBitmap->MarkBits(offset, totalSize, regionSize);
                        }
                        // Paint claims the mark bit, so publish an explicit Follow
                        // receipt into the same termination domain as barrier work.
                        BaseObject* allocated = reinterpret_cast<BaseObject*>(addr);
                        if (m != nullptr && m->IsManagedContext()) {
                            m->PublishYoungAllocBlack(allocated);
                        }
                    }
                }
            }
        }
        // MinorGCALot: every N mutator allocs force young GC (HotSpot ScavengeALot intent).
        // Safe: mutator path only; async RequestGC(YOUNG); same surface as TakeRegion heuristic.
        MinorGCALot::AfterSuccessfulAlloc(totalSize);
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
        return manager.AllocSharedObject(totalSize, PageAge::eden);
    }

    // allocate from thread local region
    if (LIKELY(tlRegion != RegionInfo::NullRegion())) {
        if (UNLIKELY(RegionIsInRelocationSet(tlRegion))) {
            NoteAllocIntoCSet(tlRegion, "impl-retire");
            g_allocIntoCSetRetired.fetch_add(1, std::memory_order_relaxed);
            if (tlRegion->IsThreadLocalRegion()) {
                manager.RemoveThreadLocalRegion(tlRegion);
                manager.EnlistFullThreadLocalRegion(tlRegion);
            }
            ClearRegion();
        } else {
            MAddress addr = tlRegion->Alloc(totalSize);
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
    RegionInfo* r  = preparedRegion.load(std::memory_order_acquire);
    if (r != nullptr && r->GetAvailableSize() >= totalSize) {
        preparedRegion.store(nullptr, std::memory_order_release);
        if (UNLIKELY(RegionIsInRelocationSet(r))) {
            NoteAllocIntoCSet(r, "prepared-reject");
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
            return r->Alloc(totalSize);
        }
    }
    // AllocateThreadLocalRegion is a safepoint, in which cj thread rescheule may happen.
    // tlRegion is bound to specific thread, so we need to forbid reschedule.
    CJThreadPreemptOffCntAdd();
    r = manager.AllocateThreadLocalRegion(tlabSize);
    CJThreadPreemptOffCntSub();
    if (UNLIKELY(r == nullptr)) {
        return manager.AllocSharedObject(totalSize, PageAge::eden);
    }
    // tlRegion may be set in PreforwardPhase handler while allocating region.
    // Null region means tlRegion is not set.
    if (tlRegion == RegionInfo::NullRegion()) {
        SetRegion(r);
        return r->Alloc(totalSize);
    }
    // tlRegion has been set in preforward phase.
    MAddress addr = tlRegion->Alloc(totalSize);
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
    return r->Alloc(totalSize);
}

MAddress AllocBuffer::AllocateRawPointerObject(size_t totalSize)
{
    RegionInfo* region = tlRawPointerRegions.GetHeadRegion();
    if (region != nullptr) {
        MAddress allocAddr = region->Alloc(totalSize);
        if (allocAddr != 0) {
            return allocAddr;
        }
    }
    RegionManager& manager = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator()).GetRegionManager();
    size_t needUnitNum = AlignUp(totalSize, RegionInfo::UNIT_SIZE) / RegionInfo::UNIT_SIZE;
    if (totalSize <= manager.GetThreadLocalRegionSize()) {
        region = manager.TakeRegion(needUnitNum, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        if (region == nullptr) {
            return 0;
        }
        tlRawPointerRegions.PrependRegion(region, RegionInfo::RegionType::TL_RAW_POINTER_REGION);
    } else {
        region = manager.TakeRegion(needUnitNum, RegionInfo::UnitRole::LARGE_SIZED_UNITS);
        if (region == nullptr) {
            return 0;
        }
        tlLargeRawPointerRegions.PrependRegion(region, RegionInfo::RegionType::TL_LARGE_RAW_POINTER_REGION);
    }

    // region is enough for totalSize.
    MAddress allocAddr = region->Alloc(totalSize);
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
#include "Heap/Verify/AllocPhaseDiag.h"
#include "Heap/Verify/MinorGCALot.h"
#include "Heap/Verify/Zap.h"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
void AllocBuffer::FlushRegion()
{
    if (LIKELY(tlRegion != RegionInfo::NullRegion()) && tlRegion != nullptr) {
        RegionSpace& theAllocator = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
        RegionManager& manager = theAllocator.GetRegionManager();
        manager.RemoveThreadLocalRegion(tlRegion);
        manager.EnlistFullThreadLocalRegion(tlRegion);
        ClearRegion();
    }
    RegionInfo* prepared = preparedRegion.load();
    if (LIKELY(prepared != RegionInfo::NullRegion()) && prepared != nullptr) {
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
