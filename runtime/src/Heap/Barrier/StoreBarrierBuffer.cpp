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
thread_local bool g_satbNodeUnavailable = false;

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
    const ForwardingProvenance provenance{
        ForwardingHolderKind::StoreBuffer,
        entry.pBase,
        reinterpret_cast<const void*>(entry.p)
    };
    BaseObject* const remappedBase =
        Heap::GetHeap().GetCollector().ResolveStoreValue(entry.pBase, provenance);
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

void StoreBarrierBuffer::SetSatbNodeUnavailableForTest(bool unavailable)
{
    g_satbNodeUnavailable = unavailable;
}
#endif

StoreBarrierInstallState StoreBarrierBuffer::CaptureInstallState()
{
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    const CycleSnapshot snapshot = resources.GetCycleSnapshot();
    StoreBarrierInstallState installed;
    installed.phase = static_cast<uint8_t>(snapshot.Phase(resources.GetExecutionContext().generation));
    installed.youngMark = snapshot.Marking(CycleGeneration::Young);
    installed.storeGood = static_cast<uintptr_t>(::g_cjStoreGoodMask);
    for (CycleGeneration generation : { CycleGeneration::Young, CycleGeneration::Old }) {
        if (snapshot.Marking(generation)) {
            installed.markingOwners |= 1u << CycleSlot(generation);
            installed.cycles[CycleSlot(generation)] =
                resources.GetCycleContext(generation).sequence.load(std::memory_order_acquire);
        }
    }
    return installed;
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

uint8_t StoreBarrierBuffer::InstalledDuringCurrentMark(const StoreBarrierEntry& entry)
{
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    const CycleSnapshot snapshot = resources.GetCycleSnapshot();
    uint8_t owners = 0;
    for (CycleGeneration generation : { CycleGeneration::Young, CycleGeneration::Old }) {
        const size_t slot = CycleSlot(generation);
        const uintptr_t epochMask = generation == CycleGeneration::Young ? MARKED_YOUNG_MASK : MARKED_OLD_MASK;
        if ((entry.installed.markingOwners & (1u << slot)) != 0 && snapshot.Active(generation) &&
            entry.installed.cycles[slot] == resources.GetCycleContext(generation).sequence.load(std::memory_order_acquire) &&
            (entry.installed.storeGood & epochMask) == (static_cast<uintptr_t>(::g_cjStoreGoodMask) & epochMask)) {
            owners |= 1u << slot;
        }
    }
    return owners;
}

StoreBarrierBuffer::PreviousRetirement StoreBarrierBuffer::RetirePrevious(const StoreBarrierEntry& entry,
                                                                          Collector& collector)
{
    const uint8_t owners = InstalledDuringCurrentMark(entry);
    if (is_null(entry.prev) || owners == 0) {
        return PreviousRetirement::NOT_REQUIRED;
    }
    RefField<> previous(entry.prev);
    const ForwardingProvenance provenance{
        ForwardingHolderKind::StoreBuffer, entry.pBase, reinterpret_cast<const void*>(entry.p)
    };
    BaseObject* const resolved = collector.make_load_good(previous, provenance);
    if (resolved == nullptr || !Heap::IsHeapAddress(resolved)) {
        return PreviousRetirement::INVALID_PREVIOUS;
    }
    for (CycleGeneration generation : { CycleGeneration::Young, CycleGeneration::Old }) {
        if ((owners & (1u << CycleSlot(generation))) == 0) {
            continue;
        }
        SatbBuffer::Node* node = nullptr;
        SatbBuffer& satb = SatbBuffer::Instance(generation);
#if defined(MRT_GC_UNIT_TESTS)
        satb.EnsureGoodNode(node, !g_satbNodeUnavailable);
#else
        satb.EnsureGoodNode(node);
#endif
        if (node == nullptr) {
            return PreviousRetirement::RESOURCE_UNAVAILABLE;
        }
        (void)node->Push(resolved, Collector::TryRecoverInteriorBase(resolved));
        satb.FlushQueue(node);
    }
    return PreviousRetirement::RETIRED;
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
        const PreviousRetirement retirement = RetirePrevious(entry, collector);
        CHECK_DETAIL(retirement != PreviousRetirement::RESOURCE_UNAVAILABLE,
                     "store-buffer SATB publication unavailable slot=%#zx prev=%#zx installed_phase=%u "
                     "installed_store_good=%#zx current=%zu index=%zu",
                     entry.p, static_cast<size_t>(raw(entry.prev)), static_cast<unsigned>(entry.installed.phase),
                     static_cast<size_t>(entry.installed.storeGood), current, i);
        if (retirement == PreviousRetirement::RETIRED) {
#if defined(MRT_GC_UNIT_TESTS)
            NotifyFlushObserver(StoreBarrierFlushEvent::PREVIOUS_RETIRED, entry);
#endif
        }
#if defined(MRT_GC_UNIT_TESTS)
        if (retirement == PreviousRetirement::INVALID_PREVIOUS) {
            NotifyFlushObserver(StoreBarrierFlushEvent::PREVIOUS_INVALID, entry);
        }
#endif
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
