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

MAddress RemapPendingField(const StoreBarrierEntry& entry, uintptr_t color)
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
    RefField<> coloredBase(to_zpointer(reinterpret_cast<uintptr_t>(entry.pBase) | color));
    BaseObject* const remappedBase =
        Heap::GetHeap().GetCollector().make_load_good(coloredBase, provenance);
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

StoreBarrierBuffer::StoreBarrierBuffer()
    : current(kStoreBarrierBufferLength), lastProcessedColor(::g_cjStoreGoodMask) {}

void StoreBarrierBuffer::Add(MAddress fieldAddress, BaseObject* fieldBase, RememberedSet& rs)
{
    Add(fieldAddress, fieldBase, zpointer::null, rs);
}

void StoreBarrierBuffer::Add(MAddress fieldAddress, zpointer prev, RememberedSet& rs)
{
    Add(fieldAddress, nullptr, prev, rs);
}

void StoreBarrierBuffer::Add(MAddress fieldAddress, BaseObject* fieldBase, zpointer prev, RememberedSet& rs)
{
    // One per-thread buffer and one processed color, as in ZStoreBarrierBuffer.
    // Consume an earlier phase before appending entries from the new phase.
    if (current == 0 || lastProcessedColor != static_cast<uintptr_t>(::g_cjStoreGoodMask)) {
        Flush(rs);
    }
    CHECK_DETAIL(fieldBase == nullptr || fieldAddress >= reinterpret_cast<MAddress>(fieldBase),
                 "store-buffer field precedes holder slot=%#zx holder=%p", fieldAddress, fieldBase);
    --current;
    buffer[current] = { fieldAddress, fieldBase,
        fieldBase == nullptr ? 0 : fieldAddress - reinterpret_cast<MAddress>(fieldBase), prev };
}

void StoreBarrierBuffer::MarkAndRemember(const StoreBarrierEntry& entry, RememberedSet& rs,
                                         Collector& collector, bool phaseChanged)
{
    StoreBarrierEntry remapped = entry;
    remapped.p = RemapPendingField(entry, lastProcessedColor);
    const bool oldSlot = Heap::IsHeapAddress(remapped.p) &&
        !RegionInfo::GetRegionInfoAt(remapped.p)->IsYoungRegion();
    const GCCycleSnapshot old = collector.GetCycleSnapshot(GCCycleGeneration::OLD);
    const bool oldMark = old.active && (old.phase == GC_PHASE_ENUM || old.phase == GC_PHASE_TRACE ||
                                       old.phase == GC_PHASE_CLEAR_SATB_BUFFER);
    const uintptr_t colors = ::g_cjStoreGoodMask;
    // zStoreBarrierBuffer.cpp:199-222: at a phase change, only stores made
    // during this old mark and through an old location belong to the snapshot.
    const bool storedDuringOldMark = (lastProcessedColor & MARKED_OLD_MASK) == (colors & MARKED_OLD_MASK);
    if (!is_null(entry.prev) && (!phaseChanged || (oldSlot && oldMark && storedDuringOldMark))) {
        RefField<> previous(entry.prev);
        const ForwardingProvenance provenance{
            ForwardingHolderKind::StoreBuffer, entry.pBase, reinterpret_cast<const void*>(remapped.p)
        };
        BaseObject* object = collector.make_load_good(previous, provenance);
        if (object != nullptr && Heap::IsHeapAddress(object)) {
            collector.MarkObjectIfActive(object);
#if defined(MRT_GC_UNIT_TESTS)
            NotifyFlushObserver(StoreBarrierFlushEvent::PREVIOUS_RETIRED, entry);
#endif
        } else {
#if defined(MRT_GC_UNIT_TESTS)
            NotifyFlushObserver(StoreBarrierFlushEvent::PREVIOUS_INVALID, entry);
#endif
        }
    }
    if (oldSlot) {
        // zStoreBarrierBuffer.cpp:161-185: a young flip makes the previous
        // remembered face read-only; scan the current field into young mark.
        if (phaseChanged && (lastProcessedColor & MARKED_YOUNG_MASK) != (colors & MARKED_YOUNG_MASK)) {
            RefField<> field(HeapSlotAt<>(remapped.p));
            const ForwardingProvenance provenance{
                ForwardingHolderKind::StoreBuffer, entry.pBase, reinterpret_cast<const void*>(remapped.p)
            };
            BaseObject* object = collector.make_load_good(field, provenance);
            if (object != nullptr && Heap::IsHeapAddress(object) &&
                RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object))->IsYoungRegion()) {
                collector.MarkYoungObjectIfActive(object);
            }
        }
        rs.Record(remapped.p, true);
#if defined(MRT_GC_UNIT_TESTS)
        NotifyFlushObserver(StoreBarrierFlushEvent::SLOT_REMEMBERED, entry);
#endif
    }
}

void StoreBarrierBuffer::Flush(RememberedSet& rs, Collector& collector)
{
    const bool phaseChanged = lastProcessedColor != static_cast<uintptr_t>(::g_cjStoreGoodMask);
    for (size_t i = current; i < kStoreBarrierBufferLength; ++i) {
        MarkAndRemember(buffer[i], rs, collector, phaseChanged);
        buffer[i] = {};
    }
    current = kStoreBarrierBufferLength;
    lastProcessedColor = ::g_cjStoreGoodMask;
}

void StoreBarrierBuffer::Flush(RememberedSet& rs)
{
    Flush(rs, Heap::GetHeap().GetCollector());
}

void StoreBarrierBuffer::Discard()
{
    current = kStoreBarrierBufferLength;
}

} // namespace MapleRuntime
