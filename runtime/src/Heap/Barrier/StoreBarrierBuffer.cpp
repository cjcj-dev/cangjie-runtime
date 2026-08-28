// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "StoreBarrierBuffer.h"

#include "Heap/Allocator/AllocBuffer.h"
#include "Heap/Allocator/Allocator.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Heap.h"
#include "Mutator/SatbBuffer.h"
#include "ObjectModel/RefField.h"
#include "RememberedSet.h"

namespace MapleRuntime {

#if defined(MRT_GC_UNIT_TESTS)
namespace {
thread_local StoreBarrierFlushObserver g_flushObserver = nullptr;

void NotifyFlushObserver(StoreBarrierFlushEvent event, const StoreBarrierEntry& entry)
{
    if (g_flushObserver != nullptr) {
        g_flushObserver(event, entry);
    }
}
} // namespace

void StoreBarrierBuffer::SetFlushObserverForTest(StoreBarrierFlushObserver observer)
{
    g_flushObserver = observer;
}
#endif

StoreBarrierInstallState StoreBarrierBuffer::CaptureInstallState()
{
    Heap& heap = Heap::GetHeap();
    const bool started = heap.IsGcStarted();
    const GCPhase phase = started ? heap.GetGCPhase() : GCPhase::GC_PHASE_IDLE;
    bool youngMark = false;
    if (started) {
        youngMark = heap.GetCollectorResources().GetGCStats().reason == GC_REASON_YOUNG;
    }
    return StoreBarrierInstallState { static_cast<uint8_t>(phase), youngMark,
                                      static_cast<uintptr_t>(::g_cjStoreGoodMask) };
}

void StoreBarrierBuffer::Add(MAddress fieldAddress, zpointer prev, RememberedSet& rs)
{
    Add(fieldAddress, prev, CaptureInstallState(), rs);
}

void StoreBarrierBuffer::Add(MAddress fieldAddress, zpointer prev, StoreBarrierInstallState installed,
                             RememberedSet& rs)
{
    if (current == 0) {
        Flush(rs);
    }
    --current;
    buffer[current] = StoreBarrierEntry { fieldAddress, prev, installed };
}

bool StoreBarrierBuffer::InstalledDuringCurrentMark(const StoreBarrierEntry& entry)
{
    const GCPhase phase = static_cast<GCPhase>(entry.installed.phase);
    if (phase != GCPhase::GC_PHASE_ENUM && phase != GCPhase::GC_PHASE_TRACE &&
        phase != GCPhase::GC_PHASE_CLEAR_SATB_BUFFER) {
        return false;
    }

    // ZGC processes each pending entry with the buffer's last processed
    // colour (zStoreBarrierBuffer.cpp:194-218).  Comparing only the generation
    // whose mark owned this entry preserves it across an unrelated-generation
    // flip, but rejects it after its own mark epoch has changed.
    const uintptr_t epochMask = entry.installed.youngMark ? MARKED_YOUNG_MASK : MARKED_OLD_MASK;
    return (entry.installed.storeGood & epochMask) ==
        (static_cast<uintptr_t>(::g_cjStoreGoodMask) & epochMask);
}

bool StoreBarrierBuffer::RetirePrevious(const StoreBarrierEntry& entry, Collector& collector)
{
    if (is_null(entry.prev) || !InstalledDuringCurrentMark(entry)) {
        return false;
    }

    RefField<> previous(entry.prev);
    BaseObject* const resolved = collector.make_load_good(previous);
    if (resolved == nullptr || !Heap::IsHeapAddress(resolved)) {
        return false;
    }

    SatbBuffer::Node* node = nullptr;
    SatbBuffer& satb = SatbBuffer::Instance();
    satb.EnsureGoodNode(node);
    if (node == nullptr) {
        return false;
    }
    (void)node->Push(resolved, Collector::TryRecoverInteriorBase(resolved));
    satb.FlushQueue(node);
    return true;
}

void StoreBarrierBuffer::MarkAndRemember(const StoreBarrierEntry& entry, RememberedSet& rs)
{
    // ZBarrier::remember only publishes old-holder slots.  Young-holder
    // entries still carry a valid SATB half, but putting their addresses in
    // the old-to-young remembered set lets the slot outlive its young region.
    if (!Heap::IsHeapAddress(entry.p) || RegionInfo::GetRegionInfoAt(entry.p)->IsYoungRegion()) {
        return;
    }
    rs.Record(entry.p, true);
}

void StoreBarrierBuffer::Flush(RememberedSet& rs, Collector& collector)
{
    for (size_t i = current; i < BufferLength; ++i) {
        const StoreBarrierEntry& entry = buffer[i];
        // ZGC flush order: make_load_good(prev) and retain it first, then
        // mark_and_remember(p) (zStoreBarrierBuffer.cpp:278-282).
        if (RetirePrevious(entry, collector)) {
#if defined(MRT_GC_UNIT_TESTS)
            NotifyFlushObserver(StoreBarrierFlushEvent::PREVIOUS_RETIRED, entry);
#endif
        }
        MarkAndRemember(entry, rs);
#if defined(MRT_GC_UNIT_TESTS)
        NotifyFlushObserver(StoreBarrierFlushEvent::SLOT_REMEMBERED, entry);
#endif
        buffer[i] = {};
    }
    current = BufferLength;
}

void StoreBarrierBuffer::Flush(RememberedSet& rs)
{
    for (size_t i = current; i < BufferLength; ++i) {
        if (!is_null(buffer[i].prev) && InstalledDuringCurrentMark(buffer[i])) {
            Flush(rs, Heap::GetHeap().GetCollector());
            return;
        }
    }

    // Idle entries have no SATB half. Avoid asking CollectorProxy for a current
    // collector before Heap::Init; gc_unit exercises this same product path.
    for (size_t i = current; i < BufferLength; ++i) {
        MarkAndRemember(buffer[i], rs);
        buffer[i] = {};
    }
    current = BufferLength;
}

void StoreBarrierBuffer::FlushAll(RememberedSet& rs)
{
    Heap::GetHeap().GetAllocator().VisitAllocBuffers([&rs](AllocBuffer& alloc) {
        alloc.GetStoreBarrierBuffer().Flush(rs);
    });
}

} // namespace MapleRuntime
