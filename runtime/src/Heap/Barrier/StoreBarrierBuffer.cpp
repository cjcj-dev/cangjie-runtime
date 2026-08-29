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

namespace {
#if defined(MRT_GC_UNIT_TESTS)
thread_local StoreBarrierFlushObserver g_flushObserver = nullptr;

void NotifyFlushObserver(StoreBarrierFlushEvent event, const StoreBarrierEntry& entry)
{
    if (g_flushObserver != nullptr) {
        g_flushObserver(event, entry);
    }
}
#endif

MAddress RemapPendingField(const StoreBarrierEntry& entry)
{
    if (entry.pBase == nullptr) {
        return entry.p;
    }
    const GCPhase phase = Heap::GetHeap().GetGCPhase();
    if (phase != GCPhase::GC_PHASE_PREFORWARD && phase != GCPhase::GC_PHASE_FORWARD) {
        return entry.p;
    }

    // ZStoreBarrierBuffer::on_new_phase_relocate: make the base load-good,
    // then reconstruct p with the offset captured while the from object was
    // still readable (zStoreBarrierBuffer.cpp:130-153).
    BaseObject* const remappedBase = Heap::GetHeap().GetCollector().ResolveStoreValue(entry.pBase);
    CHECK_DETAIL(remappedBase != nullptr && Heap::IsHeapAddress(remappedBase),
                 "store-buffer holder did not resolve base=%p slot=%#zx phase=%u",
                 entry.pBase, entry.p, static_cast<unsigned>(phase));
    return entry.Remap(remappedBase);
}
} // namespace

#if defined(MRT_GC_UNIT_TESTS)
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

void StoreBarrierBuffer::Add(MAddress fieldAddress, BaseObject* fieldBase, RememberedSet& rs)
{
    Add(fieldAddress, fieldBase, zpointer::null, CaptureInstallState(), rs);
}

void StoreBarrierBuffer::Add(MAddress fieldAddress, zpointer prev, RememberedSet& rs)
{
    Add(fieldAddress, nullptr, prev, CaptureInstallState(), rs);
}

void StoreBarrierBuffer::Add(MAddress fieldAddress, BaseObject* fieldBase, zpointer prev, RememberedSet& rs)
{
    Add(fieldAddress, fieldBase, prev, CaptureInstallState(), rs);
}

void StoreBarrierBuffer::Add(MAddress fieldAddress, zpointer prev, StoreBarrierInstallState installed,
                             RememberedSet& rs)
{
    Add(fieldAddress, nullptr, prev, installed, rs);
}

void StoreBarrierBuffer::Add(MAddress fieldAddress, BaseObject* fieldBase, zpointer prev,
                             StoreBarrierInstallState installed, RememberedSet& rs)
{
    if (!kBufferStoreBarriers) {
        rs.Record(fieldAddress, true);
        return;
    }
    if (current == 0) {
        Flush(rs);
    }
    CHECK_DETAIL(fieldBase == nullptr || fieldAddress >= reinterpret_cast<MAddress>(fieldBase),
                 "store-buffer field precedes holder slot=%#zx holder=%p", fieldAddress, fieldBase);
    --current;
    buffer[current].p = fieldAddress;
    buffer[current].pBase = fieldBase;
    buffer[current].pOffset = fieldBase == nullptr ? 0 :
        static_cast<size_t>(fieldAddress - reinterpret_cast<MAddress>(fieldBase));
    buffer[current].prev = prev;
    buffer[current].installed = installed;
}

bool StoreBarrierBuffer::InstalledDuringCurrentMark(const StoreBarrierEntry& entry)
{
    const GCPhase phase = static_cast<GCPhase>(entry.installed.phase);
    if (phase != GCPhase::GC_PHASE_ENUM && phase != GCPhase::GC_PHASE_TRACE &&
        phase != GCPhase::GC_PHASE_CLEAR_SATB_BUFFER) {
        return false;
    }
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
    StoreBarrierEntry remapped = entry;
    remapped.p = RemapPendingField(entry);
    if (!Heap::IsHeapAddress(remapped.p) || RegionInfo::GetRegionInfoAt(remapped.p)->IsYoungRegion()) {
        return;
    }
    rs.Record(remapped.p, true);
}

void StoreBarrierBuffer::Flush(RememberedSet& rs, Collector& collector)
{
    if (!kBufferStoreBarriers) {
        return;
    }
    for (size_t i = current; i < kStoreBarrierBufferLength; ++i) {
        const StoreBarrierEntry& entry = buffer[i];
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
    current = kStoreBarrierBufferLength;
}

void StoreBarrierBuffer::Flush(RememberedSet& rs)
{
    if (!kBufferStoreBarriers) {
        return;
    }
    for (size_t i = current; i < kStoreBarrierBufferLength; ++i) {
        if (!is_null(buffer[i].prev) && InstalledDuringCurrentMark(buffer[i])) {
            Flush(rs, Heap::GetHeap().GetCollector());
            return;
        }
    }
    for (size_t i = current; i < kStoreBarrierBufferLength; ++i) {
        MarkAndRemember(buffer[i], rs);
        buffer[i] = {};
    }
    current = kStoreBarrierBufferLength;
}

void StoreBarrierBuffer::Discard()
{
    current = kStoreBarrierBufferLength;
}

void StoreBarrierBuffer::FlushAll(RememberedSet& rs)
{
    if (!kBufferStoreBarriers) {
        return;
    }
    Heap::GetHeap().GetAllocator().VisitAllocBuffers([&rs](AllocBuffer& alloc) {
        alloc.GetStoreBarrierBuffer().Flush(rs);
    });
}

} // namespace MapleRuntime
