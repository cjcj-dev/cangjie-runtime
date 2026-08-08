// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Allocator/RegionSpace.h"

#include "Collector/Collector.h"
#include "Collector/CollectorResources.h"
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Common/ScopedObjectAccess.h"
#include "Heap.h"
#include "Heap/Verify/AllocPhaseDiag.h"
#include "Heap/Verify/MinorGCALot.h"
#include "Heap/Verify/Zap.h"
#include "Mutator/Mutator.h"

namespace MapleRuntime {
MAddress RegionSpace::TryAllocateOnce(size_t allocSize, AllocType allocType)
{
    if (UNLIKELY(allocType == AllocType::PINNED_OBJECT)) {
        return regionManager.AllocPinned(allocSize);
    }
    if (UNLIKELY(allocSize >= regionManager.GetLargeObjectThreshold())) {
        return regionManager.AllocLarge(allocSize);
    }
    AllocBuffer* allocBuffer = AllocBuffer::GetOrCreateAllocBuffer();
    return allocBuffer->Allocate(allocSize, allocType);
}

bool RegionSpace::ShouldRetryAllocation(size_t& tryTimes, size_t size) const
{
    if (!IsRuntimeThread() && tryTimes <= static_cast<size_t>(TryAllocationThreshold::RESCHEDULE)) {
        CJThreadResched(); // reschedule this thread for throughput.
        return true;
    }
    if (tryTimes < static_cast<size_t>(TryAllocationThreshold::TRIGGER_OOM)) {
        if (Heap::GetHeap().IsGcStarted()) {
            ScopedEnterSaferegion enterSaferegion(false);
            Heap::GetHeap().GetCollectorResources().WaitForGCFinish();
        } else {
            Heap::GetHeap().GetCollector().RequestGC(GC_REASON_HEU, false);
        }
        return true;
    }
    if (tryTimes == static_cast<size_t>(TryAllocationThreshold::TRIGGER_OOM)) {
        if (!Heap::GetHeap().IsGcStarted()) {
            VLOG(REPORT, "gc is triggered for OOM");
            Heap::GetHeap().GetCollector().RequestGC(GC_REASON_OOM, false);
        } else {
            ScopedEnterSaferegion enterSaferegion(false);
            Heap::GetHeap().GetCollectorResources().WaitForGCFinish();
            tryTimes--;
        }
        return true;
    }
    regionManager.DumpRegionStats("region statistics when gc ends", true);
    VLOG(REPORT, "Cannot allocate memory of %zu(B), throw an OutOfMemory exception", size);
    LOG(RTLOG_ERROR, "Cannot allocate memory of %zu(B), throw an OutOfMemory exception", size);
    ExceptionManager::OutOfMemory();
    return false;
}

MAddress RegionSpace::Allocate(size_t size, AllocType allocType)
{
    size_t tryTimes = 0;
    uintptr_t internalAddr = 0;
    size_t allocSize = ToAllocSize(size);
    do {
        tryTimes++;
        internalAddr = TryAllocateOnce(allocSize, allocType);
        if (LIKELY(internalAddr != 0)) {
            break;
        }
        if (IsGcThread()) {
            return 0; // it means gc doesn't have enough space to move this object.
        }
        if (!ShouldRetryAllocation(tryTimes, size)) {
            break;
        }
        (void)sched_yield();
    } while (true);
    if (internalAddr == 0) {
        return 0;
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    Sanitizer::TsanAllocObject(reinterpret_cast<void *>(internalAddr), allocSize);
#endif
    return internalAddr + HEADER_SIZE;
}
void RegionSpace::Init(const HeapParam& vmHeapParam)
{
    MemMap::Option opt = MemMap::DEFAULT_OPTIONS;
    opt.tag = "cangjie_heap";
    size_t heapSize = vmHeapParam.heapSize * 1024;
    size_t totalSize = RegionManager::GetHeapMemorySize(heapSize);
    size_t unitNum = RegionManager::GetHeapUnitCount(heapSize);
#if defined(CANGJIE_ASAN_SUPPORT)
    // asan's memory alias technique needs a shareable page
    opt.flags &= ~MAP_PRIVATE;
    opt.flags |= MAP_SHARED;
    DLOG(SANITIZER, "mmap flags set to 0x%x", opt.flags);
#endif
    // this must succeed otherwise it won't return
    map = MemMap::MapMemory(totalSize, totalSize, opt);
#if defined(CANGJIE_SANITIZER_SUPPORT) || defined(CANGJIE_GWPASAN_SUPPORT)
    Sanitizer::OnHeapAllocated(map->GetBaseAddr(), map->GetMappedSize());
#endif

    Logger::GetLogger().SetMinimumLogLevel(CangjieRuntime::GetLogParam().logLevel);
    MAddress metadata = reinterpret_cast<MAddress>(map->GetBaseAddr());
    regionManager.Initialize(unitNum, metadata);
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
    Heap::GetHeap().RemoveAllocBuffer(*this);
}

MAddress AllocBuffer::Allocate(size_t totalSize, AllocType allocType)
{
    // a hoisted specific fast path which can be inlined
    MAddress addr = 0;
    if (UNLIKELY(allocType == AllocType::RAW_POINTER_OBJECT)) {
        return AllocateRawPointerObject(totalSize);
    }

    // posttrace 乙: once TRACE..FORWARD is open, stamp current TL isTraceRegion so
    // PrepareYoung/Assemble exclude it from this cycle's route domain (peer of pin
    // free-list POST_TRACE refuse). In-place stamp only — no list surgery on hot path.
    if (LIKELY(tlRegion != RegionInfo::NullRegion())) {
        GCPhase heapP = Heap::GetHeap().GetGCPhase();
        if ((heapP == GCPhase::GC_PHASE_TRACE || heapP == GCPhase::GC_PHASE_CLEAR_SATB_BUFFER ||
             heapP == GCPhase::GC_PHASE_POST_TRACE || heapP == GCPhase::GC_PHASE_PREFORWARD ||
             heapP == GCPhase::GC_PHASE_FORWARD) &&
            !tlRegion->IsTraceRegion()) {
            tlRegion->SetTraceRegionFlag(1);
        }
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
        // blackmark 甲: allocation-black = truly set mark bits (not only skip enqueue).
        // Ordinary MOVEABLE alloc never called MarkNewObject; pin reuse did MarkObject.
        // GetRoute reads ghost liveInfo0 (PrepareForwardable snapshot). Marks written only to
        // current liveInfo after that snapshot are invisible to GetRoute — also mark ghost
        // when present. Use max(mutator,heap) phase so lagging mutator still covers.
        // Default OFF: 甲 paints bits but young PrepareYoung ClearLiveInfo wipes them
        // before mark; incomplete fix regressed default arm 3/10 (see REPORT-blackmark).
        // MRT_GCV2_ALLOC_BLACK=1 enables experimental paint (+ ghost liveInfo0).
        {
            static const bool allocBlackOn = []() {
                const char* v = std::getenv("MRT_GCV2_ALLOC_BLACK");
                return v != nullptr && v[0] == '1' && v[1] == '\0';
            }();
            if (allocBlackOn && reg != nullptr && !reg->IsLargeRegion()) {
                GCPhase mutP = GCPhase::GC_PHASE_UNDEF;
                Mutator* m = Mutator::GetMutator();
                if (m != nullptr) {
                    mutP = m->GetMutatorPhase();
                }
                GCPhase heapP = Heap::GetHeap().GetGCPhase();
                auto phaseNeedsBlack = [](GCPhase p) {
                    return p == GCPhase::GC_PHASE_ENUM || p == GCPhase::GC_PHASE_TRACE ||
                        p == GCPhase::GC_PHASE_CLEAR_SATB_BUFFER || p == GCPhase::GC_PHASE_POST_TRACE ||
                        p == GCPhase::GC_PHASE_PREFORWARD || p == GCPhase::GC_PHASE_FORWARD;
                };
                // Also: isTraceRegion alone means "implicit black" in ShouldEnqueue — unify.
                bool needBlack = phaseNeedsBlack(mutP) || phaseNeedsBlack(heapP) || reg->IsTraceRegion();
                if (needBlack) {
                    MAddress regionStart = reg->GetRegionStart();
                    MAddress regionEnd = reg->GetRegionEnd();
                    size_t offset = static_cast<size_t>(addr - regionStart);
                    size_t regionSize = static_cast<size_t>(regionEnd - regionStart);
                    if (totalSize > 0 && (totalSize % 8) == 0 && offset + totalSize <= regionSize) {
                        bool already = reg->GetOrAllocMarkBitmap()->MarkBits(offset, totalSize, regionSize);
                        if (!already) {
                            reg->AddLiveByteCount(totalSize);
                        }
                        // Ghost route domain (liveInfo0) may already be snapshotted.
                        LiveInfo* ghost = reg->GetLiveInfo0ForProbe();
                        if (ghost != nullptr && ghost->markBitmap != nullptr &&
                            reinterpret_cast<uintptr_t>(ghost->markBitmap) != LiveInfo::TEMPORARY_PTR) {
                            (void)ghost->markBitmap->MarkBits(offset, totalSize, regionSize);
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
        GCPhase heapP = Heap::GetHeap().GetGCPhase();
        if ((heapP == GCPhase::GC_PHASE_TRACE || heapP == GCPhase::GC_PHASE_CLEAR_SATB_BUFFER ||
             heapP == GCPhase::GC_PHASE_POST_TRACE || heapP == GCPhase::GC_PHASE_PREFORWARD ||
             heapP == GCPhase::GC_PHASE_FORWARD) &&
            !tlRegion->IsTraceRegion()) {
            tlRegion->SetTraceRegionFlag(1);
        }
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

    // now region must be null. If a region has been ready, then use it and tell gc-assitant thread to prepare
    // a new region, or take a new one.
    RegionInfo* r  = preparedRegion.load(std::memory_order_acquire);
    if (r != nullptr) {
        preparedRegion.store(nullptr, std::memory_order_release);
        tlRegion = r;
        if (theAllocator.IsAsyncAllocationEnable()) {
            theAllocator.AddHungryBuffer(*this);
            Heap::GetHeap().GetFinalizerProcessor().NotifyToFeedAllocBuffers();
        }
        return r->Alloc(totalSize);
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
        regionManager.CollectRegion(region);
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
