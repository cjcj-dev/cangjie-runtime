// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Allocator/RegionSpace.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "Collector/Collector.h"
#include "Collector/CollectorResources.h"
#include "Collector/GcTrigger.h"
#include "Collector/Uncommitter.h"
#include "Base/TimeUtils.h"
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Common/ScopedObjectAccess.h"
#include "Common/ColourEncoding.h"
#include "Heap.h"
#include "Heap/Verify/AllocPhaseDiag.h"
#include "Heap/Verify/MinorGCALot.h"
#include "Heap/Verify/Zap.h"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
namespace {
std::atomic<size_t> g_allocIntoCSetCount{ 0 };
std::atomic<size_t> g_allocIntoCSetRetired{ 0 };

bool RegionIsInRelocationSet(const RegionInfo* reg)
{
    if (reg == nullptr || reg == RegionInfo::NullRegion()) {
        return false;
    }
    if (reg->IsFromRegion() || reg->IsLoneFromRegion()) {
        return true;
    }
    RegionInfo::RouteState rs = reg->GetRouteState();
    return rs == RegionInfo::RouteState::FORWARDABLE || rs == RegionInfo::RouteState::ROUTING ||
        rs == RegionInfo::RouteState::ROUTED;
}

void NoteAllocIntoCSet(RegionInfo* reg, const char* where)
{
    (void)reg;
    (void)where;
    g_allocIntoCSetCount.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

size_t RegionSpace::AllocIntoCSetCount()
{
    return g_allocIntoCSetCount.load(std::memory_order_relaxed);
}

size_t RegionSpace::AllocIntoCSetRetiredCount()
{
    return g_allocIntoCSetRetired.load(std::memory_order_relaxed);
}
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
        // A mutator creates exactly one request for this blocking allocation.
        // The allocator queue owns GC triggering and directed satisfaction;
        // there is no reschedule/attempt counter loop on this path.
        const size_t claimedUnits = regionManager.StallAllocation(allocSize);
        if (claimedUnits == 0) {
            regionManager.DumpRegionStats("region statistics when gc ends", true);
            VLOG(REPORT, "Cannot allocate memory of %zu(B), throw an OutOfMemory exception", size);
            LOG(RTLOG_ERROR, "Cannot allocate memory of %zu(B), throw an OutOfMemory exception", size);
            ExceptionManager::OutOfMemory();
            return 0;
        }
        internalAddr = TryAllocateOnce(allocSize, allocType);
        regionManager.FinishStalledAllocation(claimedUnits);
    }
    if (internalAddr == 0) {
        VLOG(REPORT, "Allocation request was satisfied but allocation still failed: size=%zu", size);
        return 0;
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanAllocObject(reinterpret_cast<void *>(internalAddr), allocSize);
#endif
    return internalAddr + HEADER_SIZE;
}

size_t RegionSpace::UncommitIdleMemory()
{
    if (!Uncommitter::Enabled()) {
        return 0;
    }
    if (Heap::GetHeap().IsGcStarted() || Uncommitter::ShouldStopUncommit()) {
        return 0;
    }
    const GCPhase startPhase = Heap::GetHeap().GetGCPhase();
    if (startPhase == GCPhase::GC_PHASE_POST_TRACE || startPhase == GCPhase::GC_PHASE_PREFORWARD ||
        startPhase == GCPhase::GC_PHASE_FORWARD) {
        return 0;
    }
    uint64_t delayNs = Uncommitter::DelayNs();
    uint64_t now = TimeUtil::NanoSeconds();
    if (now < delayNs) {
        return 0;
    }
    Uncommitter::ActivateCycle();
    size_t total = 0;
    const uint64_t idleBefore = now - delayNs;
    while (!Heap::GetHeap().IsGcStarted() && !Uncommitter::ShouldStopUncommit()) {
        const GCPhase tickPhase = Heap::GetHeap().GetGCPhase();
        if (tickPhase == GCPhase::GC_PHASE_POST_TRACE || tickPhase == GCPhase::GC_PHASE_PREFORWARD ||
            tickPhase == GCPhase::GC_PHASE_FORWARD) {
            break;
        }
        size_t usedBytes = regionManager.GetUsedRegionSize();
        size_t dirtyBytes = regionManager.GetDirtyUnitCount() * RegionInfo::UNIT_SIZE;
        size_t releasedBytes = regionManager.GetReleasedUnitCount() * RegionInfo::UNIT_SIZE;
        size_t garbageBytes = regionManager.GetGarbageUnitCount() * RegionInfo::UNIT_SIZE;
        size_t minCapacity = Uncommitter::MinCapacity(usedBytes, kGcTriggerYoungFixedBytes);
        size_t chunk = Uncommitter::ChunkLimit(GetMaxCapacity());
        size_t flush = Uncommitter::FlushBytes(usedBytes, releasedBytes, minCapacity, chunk);
        LOG(RTLOG_INFO,
            "Uncommit: tick used=%zu dirty=%zu released=%zu garbage=%zu min=%zu flush=%zu",
            usedBytes, dirtyBytes, releasedBytes, garbageBytes, minCapacity, flush);
        if (flush == 0) {
            break;
        }
        size_t n = regionManager.UncommitIdleUnits(flush, idleBefore);
        if (n == 0) {
            break;
        }
        total += n;
    }
    return total;
}

size_t RegionSpace::DrainUncommitIdleMemory()
{
    size_t usedBytes = regionManager.GetUsedRegionSize();
    size_t releasedBytes = regionManager.GetReleasedUnitCount() * RegionInfo::UNIT_SIZE;
    size_t minCapacity = Uncommitter::MinCapacity(usedBytes, kGcTriggerYoungFixedBytes);
    size_t chunk = Uncommitter::ChunkLimit(GetMaxCapacity());
    size_t flush = Uncommitter::FlushBytes(usedBytes, releasedBytes, minCapacity, chunk);
    if (flush == 0) {
        flush = releasedBytes;
    }
    if (flush == 0) {
        return 0;
    }
    return regionManager.UncommitIdleUnits(flush, static_cast<uint64_t>(-1), false);
}

void RegionSpace::Init(const HeapParam& vmHeapParam)
{
    MemMap::Option opt = MemMap::DEFAULT_OPTIONS;
    opt.tag = "cangjie_heap";
    size_t heapSize = 0;
    CHECK_DETAIL(CheckedMulSize(vmHeapParam.heapSize, size_t{1024}, heapSize),
                 "heap size overflows bytes before reservation: heapSizeKB=%zu", vmHeapParam.heapSize);
    size_t totalSize = RegionManager::GetHeapMemorySize(heapSize);
    size_t unitNum = RegionManager::GetHeapUnitCount(heapSize);
    size_t metadataSize = RegionManager::GetMetadataSize(unitNum);
    // Seal both process inputs before the first mmap. The values remain fixed
    // for the complete reservation and physical-page lifetime.
    const AddressSpaceBudget addressBudget = AddressSpaceBudget::SealProcessBudget();
    const NumaTopology numaTopology = NumaTopology::SealProcessTopology();
#if defined(CANGJIE_ASAN_SUPPORT)
    // asan's memory alias technique needs a shareable page
    opt.flags &= ~MAP_PRIVATE;
    opt.flags |= MAP_SHARED;
    DLOG(SANITIZER, "mmap flags set to 0x%x", opt.flags);
#endif
    // this must succeed otherwise it won't return
    map = MemMap::MapMemory(totalSize, metadataSize, opt, addressBudget, numaTopology);
    const uintptr_t reservationStart = reinterpret_cast<uintptr_t>(map->GetBaseAddr());
    uintptr_t reservationEnd = 0;
    if (IsRepresentableLow48Range(reservationStart, totalSize)) {
        reservationEnd = reservationStart + totalSize;
    }
    CHECK_DETAIL(reservationEnd != 0,
                 "heap reservation exceeds the 48-bit HeapSlot address carrier: start=%#zx end=%#zx size=%zu",
                 static_cast<size_t>(reservationStart), static_cast<size_t>(reservationEnd), totalSize);
    // RegionManager indexes units as heapStart + index * UNIT_SIZE. Until that
    // caller is segment-aware, never reinterpret holes between reservations as
    // allocatable units.
    CHECK_DETAIL(map->GetReservationRegistry().Contains(reinterpret_cast<uintptr_t>(map->GetBaseAddr()), totalSize),
                 "RegionSpace requires a contiguous heap reservation");
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
    Sanitizer::OnHeapAllocated(map->GetBaseAddr(), map->GetMappedSize());
#endif

    Logger::GetLogger().SetMinimumLogLevel(CangjieRuntime::GetLogParam().logLevel);
    MAddress metadata = reinterpret_cast<MAddress>(map->GetBaseAddr());
    regionManager.Initialize(unitNum, metadata, *map, vmHeapParam,
                             CangjieRuntime::GetGCParam().garbageThreshold);
    reservedStart = regionManager.GetRegionHeapStart();
    reservedEnd = reinterpret_cast<MAddress>(map->GetMappedEndAddr());
#if defined(MRT_DUMP_ADDRESS)
    VLOG(REPORT, "region metadata@%zx, heap @[0x%zx+%zu, 0x%zx)", metadata, reservedStart, reservedEnd - reservedStart,
         reservedEnd);
#endif
    Heap::OnHeapCreated(reservedStart);
    Heap::OnHeapExtended(reservedEnd);
}

AllocBuffer* AllocBuffer::GetOrCreateAllocBuffer()
{
    auto* buffer = AllocBuffer::GetAllocBuffer();
    if (buffer == nullptr) {
        buffer = new (std::nothrow) AllocBuffer();
        CHECK_DETAIL(buffer != nullptr, "new region alloc buffer fail");
        buffer->Init();
        ThreadLocal::SetAllocBuffer(buffer);
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
    Heap::GetHeap().RegisterAllocBuffer(*this);
}

void AllocBuffer::Fini()
{
    storeBarrierBuffer.Flush(Heap::GetHeap().GetRememberedSet());
    Heap::GetHeap().RemoveAllocBuffer(*this);
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
        tlRegion = RegionInfo::NullRegion();
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
        RegionInfo* reg = nullptr;
        if (tlRegion != nullptr && tlRegion != RegionInfo::NullRegion()) {
            reg = tlRegion;
        } else {
            reg = RegionInfo::TryGetRegionInfoAt(addr);
        }
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
                        bool already = reg->GetOrAllocMarkBitmap(view)->MarkBits(offset, totalSize, regionSize);
                        if (!already) {
                            reg->AddLiveByteCount(totalSize);
                            reg->PublishCurrentMarkFace();
                        }
                        LiveInfo* ghost = reg->GetLiveInfo0ForProbe();
                        RegionBitmap* ghostBitmap = ghost == nullptr ? nullptr : reg->GetOwnerMarkBitmap(ghost);
                        if (ghost != nullptr && ghostBitmap != nullptr) {

                            (void)ghostBitmap->MarkBits(offset, totalSize, regionSize);
                        }
                        // Paint claims the mark bit, so publish an explicit Follow
                        // receipt into the same termination domain as barrier work.
                        // The local ledger is retained only until mark-end cleanup;
                        // it is no longer a pause-local discovery authority.
                        BaseObject* allocated = reinterpret_cast<BaseObject*>(addr);
                        PushYoungAllocBlack(allocated);
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

    // allocate from thread local region
    if (LIKELY(tlRegion != RegionInfo::NullRegion())) {
        if (UNLIKELY(RegionIsInRelocationSet(tlRegion))) {
            NoteAllocIntoCSet(tlRegion, "impl-retire");
            g_allocIntoCSetRetired.fetch_add(1, std::memory_order_relaxed);
            if (tlRegion->IsThreadLocalRegion()) {
                manager.RemoveThreadLocalRegion(tlRegion);
                manager.EnlistFullThreadLocalRegion(tlRegion);
            }
            tlRegion = RegionInfo::NullRegion();
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
                tlRegion = RegionInfo::NullRegion();
            }
        }
    }

    // now region must be null. If a region has been ready, then use it and tell gc-assitant thread to prepare
    // a new region, or take a new one.
    RegionInfo* r  = preparedRegion.load(std::memory_order_acquire);
    if (r != nullptr) {
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
            tlRegion = r;
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
    r = manager.AllocateThreadLocalRegion();
    CJThreadPreemptOffCntSub();
    if (UNLIKELY(r == nullptr)) {
        return 0;
    }
    // tlRegion may be set in PreforwardPhase handler while allocating region.
    // Null region means tlRegion is not set.
    if (tlRegion == RegionInfo::NullRegion()) {
        tlRegion = r;
        return r->Alloc(totalSize);
    }
    // tlRegion has been set in preforward phase.
    MAddress addr = tlRegion->Alloc(totalSize);
    if (addr != 0) {
        if (!SetPreparedRegion(r)) {
            // r is inserted in thread-local region list when allocated, we need to remove it from the list.
            manager.RemoveThreadLocalRegion(r);
            manager.ReclaimRegion(r);
        }
        return addr;
    }
    // tlRegion is not enough for allocation, so we use r.
    manager.RemoveThreadLocalRegion(tlRegion);
    manager.EnlistFullThreadLocalRegion(tlRegion);
    tlRegion = r;
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

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
bool RegionSpace::IsHeapObject(MAddress addr) const
{
    return IsHeapAddress(addr);
}
#endif
void RegionSpace::FeedHungryBuffers()
{
    ScopedObjectAccess soa;
    AllocBufferManager::HungryBuffers hungryBuffers;
    allocBufferManager->SwapHungryBuffers(hungryBuffers);
    RegionInfo* region = nullptr;
    for (auto* buffer : hungryBuffers) {
        if (buffer->GetPreparedRegion() != nullptr) { continue; }
        if (region == nullptr) {
            region = regionManager.AllocateThreadLocalRegion(true);
            if (region == nullptr) { return; }
        }
        if (buffer->SetPreparedRegion(region)) {
            region = nullptr;
        }
    }
    if (region != nullptr) {
        // The region is inserted in thread-local region list when allocated, we need to remove it from the list.
        regionManager.RemoveThreadLocalRegion(region);
        regionManager.CollectRegion<Generation::Old>(region);
    }
}

void AllocBuffer::FlushRegion()
{
    if (LIKELY(tlRegion != RegionInfo::NullRegion()) && tlRegion != nullptr) {
        RegionSpace& theAllocator = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
        RegionManager& manager = theAllocator.GetRegionManager();
        manager.RemoveThreadLocalRegion(tlRegion);
        manager.EnlistFullThreadLocalRegion(tlRegion);
        tlRegion = RegionInfo::NullRegion();
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
} // namespace MapleRuntime
