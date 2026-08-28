// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <unordered_set>
#include <vector>

// Match the existing test_young_conc test-peer shape: this TU alone needs the
// CollectorProxy friendship to publish a real product TRACE phase.  Product
// libraries retain their configured macro set.
#ifndef MRT_TESTABLE_INTERNALS
#define MRT_TESTABLE_INTERNALS 1
#endif

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#define private public
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Barrier/StoreBarrierBuffer.h"
#undef private

#include "Heap/Collector/Collector.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Barrier/Barrier.h"
#include "Heap/WCollector/TraceBarrier.h"
#include "Heap/Allocator/AllocBuffer.h"
#include "Mutator/Mutator.h"
#include "Mutator/SatbBuffer.h"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/RefField.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

struct RelocationReceiptTestAccess {
    static void EnsureCollectorProxyBound(CollectorResources& resources)
    {
        CollectorProxy& proxy = resources.collectorProxy;
        if (proxy.currentCollector == nullptr) {
            proxy.currentCollector = &proxy.wCollector;
        }
    }
};

} // namespace MapleRuntime

namespace {

class StoreBufferCollector final : public Collector {
public:
    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    FindToVersionResult FindToVersion(BaseObject*) const override
    {
        return FindToVersionResult::NotForwarded();
    }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* obj) const override
    {
        return RefField<>(obj, ::g_cjStoreGoodMask);
    }
};

MAddress SlotAt(GcHeapFixture& fx, size_t i)
{
    return fx.heapStart + i * sizeof(void*);
}

#if defined(MRT_GC_UNIT_TESTS)
thread_local std::vector<StoreBarrierFlushEvent>* g_flushEvents = nullptr;

void RecordFlushEvent(StoreBarrierFlushEvent event, const StoreBarrierEntry&)
{
    if (g_flushEvents != nullptr) {
        g_flushEvents->push_back(event);
    }
}

class FlushObserverScope final {
public:
    explicit FlushObserverScope(std::vector<StoreBarrierFlushEvent>& events)
    {
        g_flushEvents = &events;
        StoreBarrierBuffer::SetFlushObserverForTest(RecordFlushEvent);
    }

    ~FlushObserverScope()
    {
        StoreBarrierBuffer::SetFlushObserverForTest(nullptr);
        g_flushEvents = nullptr;
    }
};
#endif

class AllocBufferScope final {
public:
    explicit AllocBufferScope(AllocBuffer& alloc) : alloc(alloc), saved(ThreadLocal::GetAllocBuffer())
    {
        ThreadLocal::SetAllocBuffer(&alloc);
    }

    ~AllocBufferScope()
    {
        ThreadLocal::SetAllocBuffer(saved);
        // The test TU and product SO each own an inline NullRegion sentinel.
        // Use the product destructor's other empty representation.
        alloc.SetRegion(nullptr);
    }

private:
    AllocBuffer& alloc;
    AllocBuffer* saved;
};

} // namespace

GC_TEST(StoreBuf, EntryCarriesPairedPrevAndInstallColour)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 8);
    const zpointer prev = RefField<>(fx.obj0, ::g_cjStoreGoodMask).GetFieldValue();
    const StoreBarrierInstallState installed { static_cast<uint8_t>(GCPhase::GC_PHASE_TRACE), false,
                                               static_cast<uintptr_t>(::g_cjStoreGoodMask) };

    buf.Add(slot, prev, installed, rs);

    const StoreBarrierEntry& entry = buf.buffer[buf.current];
    GC_EXPECT_EQ(entry.p, slot);
    GC_EXPECT_EQ(raw(entry.prev), raw(prev));
    GC_EXPECT_EQ(entry.installed.phase, static_cast<uint8_t>(GCPhase::GC_PHASE_TRACE));
    GC_EXPECT_EQ(entry.installed.storeGood, static_cast<uintptr_t>(::g_cjStoreGoodMask));
}

GC_TEST(StoreBuf, ProductWriteCarriesOldValueOnlyInPrevArm)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);

    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    TraceBarrier barrier(collector, rs);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);

    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const zpointer prev = RefField<>(fx.obj0, ::g_cjStoreGoodMask).GetFieldValue();
    field.StoreColoured(prev);

    Heap& heap = Heap::GetHeap();
    CollectorResources& resources = heap.GetCollectorResources();
    RelocationReceiptTestAccess::EnsureCollectorProxyBound(resources);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    const GCPhase phaseBefore = heap.GetGCPhase();
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_USER;
    heap.SetGCPhase(GCPhase::GC_PHASE_TRACE);

    Mutator mutator;
    mutator.SetMutatorPhase(GCPhase::GC_PHASE_TRACE);
    Mutator* const mutatorBefore = ThreadLocal::GetMutator();
    ThreadLocal::SetMutator(&mutator);
    barrier.WriteReference(fx.obj0, field, fx.obj1);
    ThreadLocal::SetMutator(mutatorBefore);
    heap.SetGCPhase(phaseBefore);
    resources.GetGCStats().reason = reasonBefore;
    resources.SetGcStarted(startedBefore);

    StoreBarrierBuffer& buf = alloc.GetStoreBarrierBuffer();
    const size_t pending = buf.Pending();
    std::vector<BaseObject*> retired;
    // The independently required new-value closure is still in the mutator's
    // direct node.  GetRetiredObjects therefore measures only the paired flush.
    SatbBuffer::Instance().GetRetiredObjects(retired);
    retired.clear();
    GC_EXPECT_EQ(pending, 1u);
    if (pending != 1u) {
        return;
    }
    const StoreBarrierEntry& entry = buf.buffer[buf.current];
    const bool pairMatches = raw(entry.prev) == raw(prev) && raw(entry.prev) != entry.p;
    GC_EXPECT_EQ(entry.p, reinterpret_cast<MAddress>(&field));
    GC_EXPECT_EQ(raw(entry.prev), raw(prev));
    GC_EXPECT_NE(raw(entry.prev), entry.p);
    if (!pairMatches) {
        buf.buffer[buf.current] = {};
        buf.current = StoreBarrierBuffer::Capacity();
        return;
    }
    buf.Flush(rs, collector);
    SatbBuffer::Instance().GetRetiredObjects(retired);
    // The product TraceBarrier path contributes exactly one SATB retirement;
    // its former direct enqueue was removed, leaving the paired flush as the
    // sole producer for this runtime-domain write.
    GC_EXPECT_EQ(retired.size(), 1u);
}

GC_TEST(StoreBuf, ProductPhaseFlushHandsPairedPrevToSatb)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);

    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    TraceBarrier barrier(collector, rs);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);

    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(RefField<>(fx.obj0, ::g_cjStoreGoodMask).GetFieldValue());

    Heap& heap = Heap::GetHeap();
    CollectorResources& resources = heap.GetCollectorResources();
    RelocationReceiptTestAccess::EnsureCollectorProxyBound(resources);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    const GCPhase phaseBefore = heap.GetGCPhase();
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_USER;
    heap.SetGCPhase(GCPhase::GC_PHASE_TRACE);

    std::vector<BaseObject*> retired;
    SatbBuffer::Instance().GetRetiredObjects(retired);
    retired.clear();
    Mutator mutator;
    mutator.SetMutatorPhase(GCPhase::GC_PHASE_TRACE);
    mutator.SetMarkFlushAllocBuffer(&alloc);
#if defined(MRT_TESTABLE_INTERNALS)
    mutator.SetStoreBarrierRememberedSetForTest(&rs);
#endif
    Mutator* const mutatorBefore = ThreadLocal::GetMutator();
    ThreadLocal::SetMutator(&mutator);
    barrier.WriteReference(fx.obj0, field, fx.obj1);
    GC_EXPECT_EQ(alloc.GetStoreBarrierBuffer().Pending(), 1u);
    mutator.TransitionToGCPhaseExclusive(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
    GC_EXPECT_TRUE(alloc.GetStoreBarrierBuffer().IsEmpty());
    ThreadLocal::SetMutator(mutatorBefore);
    heap.SetGCPhase(phaseBefore);
    resources.GetGCStats().reason = reasonBefore;
    resources.SetGcStarted(startedBefore);

    SatbBuffer::Instance().GetRetiredObjects(retired);
    size_t oldCount = 0;
    size_t newCount = 0;
    for (BaseObject* object : retired) {
        oldCount += object == fx.obj0 ? 1u : 0u;
        newCount += object == fx.obj1 ? 1u : 0u;
    }
    GC_EXPECT_EQ(oldCount, 1u);
    GC_EXPECT_TRUE(newCount >= 1u);
}

GC_TEST(StoreBuf, GcAssistedPhaseFlushDefersStoreBuffer)
{
    GcHeapFixture fx;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);

    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    TraceBarrier barrier(collector, rs);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);

    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(RefField<>(fx.obj0, ::g_cjStoreGoodMask).GetFieldValue());

    Heap& heap = Heap::GetHeap();
    CollectorResources& resources = heap.GetCollectorResources();
    RelocationReceiptTestAccess::EnsureCollectorProxyBound(resources);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    const GCPhase phaseBefore = heap.GetGCPhase();
    resources.SetGcStarted(true);
    resources.GetGCStats().reason = GC_REASON_USER;
    heap.SetGCPhase(GCPhase::GC_PHASE_TRACE);

    Mutator mutator;
    mutator.SetMutatorPhase(GCPhase::GC_PHASE_TRACE);
    mutator.SetMarkFlushAllocBuffer(&alloc);
#if defined(MRT_TESTABLE_INTERNALS)
    mutator.SetStoreBarrierRememberedSetForTest(&rs);
#endif
    Mutator* const mutatorBefore = ThreadLocal::GetMutator();
    ThreadLocal::SetMutator(&mutator);
    barrier.WriteReference(fx.obj0, field, fx.obj1);
    GC_EXPECT_EQ(alloc.GetStoreBarrierBuffer().Pending(), 1u);

    // A GC worker assisting a saferegion transition must not consume the
    // paired store entry: ZGC on_new_phase runs in the Java-thread flush.
    mutator.TransitionToGCPhaseExclusive(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER, false);
    GC_EXPECT_EQ(alloc.GetStoreBarrierBuffer().Pending(), 1u);
    // The mutator-side transition (or the next explicit safepoint) consumes it.
    mutator.TransitionToGCPhaseExclusive(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER, true);
    GC_EXPECT_TRUE(alloc.GetStoreBarrierBuffer().IsEmpty());

    ThreadLocal::SetMutator(mutatorBefore);
    heap.SetGCPhase(phaseBefore);
    resources.GetGCStats().reason = reasonBefore;
    resources.SetGcStarted(startedBefore);
}

GC_TEST(StoreBuf, NonNullPrevPublishesSatbBeforeRememberingSlot)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    StoreBarrierBuffer buf;
    std::vector<BaseObject*> retired;
    SatbBuffer::Instance().GetRetiredObjects(retired);
    retired.clear();

    const MAddress slot = SlotAt(fx, 8);
    const zpointer prev = RefField<>(fx.obj0, ::g_cjStoreGoodMask).GetFieldValue();
    const StoreBarrierInstallState installed { static_cast<uint8_t>(GCPhase::GC_PHASE_TRACE), false,
                                               static_cast<uintptr_t>(::g_cjStoreGoodMask) };
#if defined(MRT_GC_UNIT_TESTS)
    std::vector<StoreBarrierFlushEvent> events;
    FlushObserverScope observe(events);
#endif
    buf.Add(slot, prev, installed, rs);
    buf.Flush(rs, collector);
    SatbBuffer::Instance().GetRetiredObjects(retired);

    GC_EXPECT_EQ(retired.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(retired[0]), reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_TRUE(rs.Contains(slot));
#if defined(MRT_GC_UNIT_TESTS)
    GC_EXPECT_EQ(events.size(), 2u);
    GC_EXPECT_EQ(events[0], StoreBarrierFlushEvent::PREVIOUS_RETIRED);
    GC_EXPECT_EQ(events[1], StoreBarrierFlushEvent::SLOT_REMEMBERED);
#endif
}

GC_TEST(StoreBuf, NullPrevOnlyRemembersSlot)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    std::vector<BaseObject*> retired;
    SatbBuffer::Instance().GetRetiredObjects(retired);
    retired.clear();

    const MAddress slot = SlotAt(fx, 8);
    const StoreBarrierInstallState installed { static_cast<uint8_t>(GCPhase::GC_PHASE_TRACE), false,
                                               static_cast<uintptr_t>(::g_cjStoreGoodMask) };
    buf.Add(slot, zpointer::null, installed, rs);
    buf.Flush(rs);
    SatbBuffer::Instance().GetRetiredObjects(retired);

    GC_EXPECT_TRUE(retired.empty());
    GC_EXPECT_TRUE(rs.Contains(slot));
}

GC_TEST(StoreBuf, YoungHolderRetiresPrevWithoutRememberingSlot)
{
    GcHeapFixture fx;
    fx.region1->SetYoungRegionFlag(1);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    StoreBarrierBuffer buf;
    std::vector<BaseObject*> retired;
    SatbBuffer::Instance().GetRetiredObjects(retired);
    retired.clear();

    const MAddress slot = reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE;
    const uintptr_t colour = static_cast<uintptr_t>(::g_cjStoreGoodMask);
    const zpointer prev = RefField<>(fx.obj0, colour).GetFieldValue();
    buf.Add(slot, prev,
            StoreBarrierInstallState { static_cast<uint8_t>(GCPhase::GC_PHASE_TRACE), false, colour }, rs);
    buf.Flush(rs, collector);
    SatbBuffer::Instance().GetRetiredObjects(retired);

    GC_EXPECT_EQ(retired.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(retired[0]), reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_TRUE(!rs.Contains(slot));
}

GC_TEST(StoreBuf, PhaseFlipRetainsOnlyCurrentEpochPrev)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    StoreBarrierBuffer buf;
    std::vector<BaseObject*> retired;
    SatbBuffer::Instance().GetRetiredObjects(retired);
    retired.clear();

    const uintptr_t current = static_cast<uintptr_t>(::g_cjStoreGoodMask);
    const zpointer previous = RefField<>(fx.obj0, current).GetFieldValue();
    const zpointer currentPrev = RefField<>(fx.obj1, current).GetFieldValue();
    buf.Add(SlotAt(fx, 8), previous,
            StoreBarrierInstallState { static_cast<uint8_t>(GCPhase::GC_PHASE_TRACE), false,
                                       current ^ MARKED_OLD_MASK }, rs);
    buf.Add(SlotAt(fx, 9), currentPrev,
            StoreBarrierInstallState { static_cast<uint8_t>(GCPhase::GC_PHASE_TRACE), false, current }, rs);

    buf.Flush(rs, collector);
    SatbBuffer::Instance().GetRetiredObjects(retired);

    GC_EXPECT_EQ(retired.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(retired[0]), reinterpret_cast<MAddress>(fx.obj1));
}

GC_TEST(StoreBuf, PendingEntryFromOldEpochIsRejectedAfterOldMarkFlip)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    StoreBarrierBuffer buf;
    std::vector<BaseObject*> retired;
    SatbBuffer::Instance().GetRetiredObjects(retired);
    retired.clear();

    const uintptr_t before = static_cast<uintptr_t>(::g_cjStoreGoodMask);
    const zpointer prev = RefField<>(fx.obj0, before).GetFieldValue();
    buf.Add(SlotAt(fx, 12), prev,
            StoreBarrierInstallState { static_cast<uint8_t>(GCPhase::GC_PHASE_TRACE), false, before }, rs);
    // Publish the next old-mark epoch before this thread drains.  The pending
    // entry belongs to the install-time epoch and must not enter the new SATB.
    ::g_cjStoreGoodMask = before ^ MARKED_OLD_MASK;
    buf.Flush(rs, collector);
    ::g_cjStoreGoodMask = before;
    SatbBuffer::Instance().GetRetiredObjects(retired);

    GC_EXPECT_TRUE(retired.empty());
    GC_EXPECT_TRUE(rs.Contains(SlotAt(fx, 12)));
}

GC_TEST(StoreBuf, PendingOldMarkEntrySurvivesYoungMarkFlip)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    StoreBarrierBuffer buf;
    std::vector<BaseObject*> retired;
    SatbBuffer::Instance().GetRetiredObjects(retired);
    retired.clear();

    const uintptr_t before = static_cast<uintptr_t>(::g_cjStoreGoodMask);
    const zpointer prev = RefField<>(fx.obj0, before).GetFieldValue();
    buf.Add(SlotAt(fx, 13), prev,
            StoreBarrierInstallState { static_cast<uint8_t>(GCPhase::GC_PHASE_TRACE), false, before }, rs);
    // A young-mark publication does not change the old-mark epoch that owns
    // this entry, so its SATB half must still be retired after the flip.
    ::g_cjStoreGoodMask = before ^ MARKED_YOUNG_MASK;
    buf.Flush(rs, collector);
    ::g_cjStoreGoodMask = before;
    SatbBuffer::Instance().GetRetiredObjects(retired);

    GC_EXPECT_EQ(retired.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(retired[0]), reinterpret_cast<MAddress>(fx.obj0));
}

GC_TEST(StoreBuf, PhaseFlipLeavesOnePreviousAndOneCurrentSlot)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress previousSlot = SlotAt(fx, 8);
    const MAddress currentSlot = SlotAt(fx, 9);

    buf.Add(previousSlot, zpointer::null, rs);
    buf.Flush(rs);
    rs.FlipForMinor();
    buf.Add(currentSlot, zpointer::null, rs);
    buf.Flush(rs);

    std::unordered_set<MAddress> previous;
    GC_EXPECT_EQ(rs.ScanPreviousForMinor(previous), 1u);
    GC_EXPECT_EQ(previous.size(), 1u);
    GC_EXPECT_TRUE(previous.count(previousSlot) == 1);
    const std::unordered_set<MAddress> current = rs.Snapshot();
    GC_EXPECT_EQ(current.size(), 1u);
    GC_EXPECT_TRUE(current.count(currentSlot) == 1);
}

GC_TEST(StoreBuf, FullAutoFlushKeepsEveryEntry)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const size_t n = StoreBarrierBuffer::Capacity() + 1;
    for (size_t i = 0; i < n; ++i) {
        buf.Add(SlotAt(fx, i + 8), zpointer::null, rs);
    }
    GC_EXPECT_EQ(buf.Pending(), 1u);
    GC_EXPECT_EQ(rs.Size(), StoreBarrierBuffer::Capacity());
    buf.Flush(rs);
    GC_EXPECT_TRUE(buf.IsEmpty());
    std::unordered_set<MAddress> drained;
    rs.DrainForMinor(drained);
    GC_EXPECT_EQ(drained.size(), n);
    for (size_t i = 0; i < n; ++i) {
        GC_EXPECT_TRUE(drained.count(SlotAt(fx, i + 8)) == 1);
    }
}

GC_TEST(StoreBuf, UnflushedPendingInvisibleToDrain)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 8);
    buf.Add(slot, zpointer::null, rs);
    GC_EXPECT_EQ(buf.Pending(), 1u);
    std::unordered_set<MAddress> lost;
    rs.DrainForMinor(lost);
    GC_EXPECT_EQ(lost.size(), 0u);
    GC_EXPECT_EQ(buf.Pending(), 1u);
}

GC_TEST(StoreBuf, FlushBeforeRelocateSnapshotPublishesPending)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 8);
    buf.Add(slot, zpointer::null, rs);
    GC_EXPECT_TRUE(rs.Snapshot().count(slot) == 0);
    buf.Flush(rs);
    GC_EXPECT_TRUE(rs.Snapshot().count(slot) == 1);
}

GC_TEST(StoreBuf, MarkEndSnapshotLeavesCurrentForNextMinor)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 11);
    buf.Add(slot, zpointer::null, rs);
    buf.Flush(rs);
    std::unordered_set<MAddress> markEnd = rs.Snapshot();
    GC_EXPECT_TRUE(markEnd.count(slot) == 1);
    GC_EXPECT_TRUE(rs.Contains(slot));
    GC_EXPECT_EQ(rs.Size(), 1u);
}

GC_TEST(StoreBuf, FlushBeforeMinorDoesNotLoseEdges)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const size_t n = 7;
    for (size_t i = 0; i < n; ++i) {
        buf.Add(SlotAt(fx, i + 8), zpointer::null, rs);
    }
    buf.Flush(rs);
    std::unordered_set<MAddress> drained;
    rs.DrainForMinor(drained);
    GC_EXPECT_EQ(drained.size(), n);
    for (size_t i = 0; i < n; ++i) {
        GC_EXPECT_TRUE(drained.count(SlotAt(fx, i + 8)) == 1);
    }
}

GC_TEST(StoreBuf, ThreadExitFlushRedeems)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 9);
    buf.Add(slot, zpointer::null, rs);
    buf.Flush(rs);
    std::unordered_set<MAddress> drained;
    rs.DrainForMinor(drained);
    GC_EXPECT_TRUE(drained.count(slot) == 1);
}

GC_TEST(StoreBuf, ReRememberDoesNotFightBuffer)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 10);
    buf.Add(slot, zpointer::null, rs);
    rs.Record(slot);
    rs.Record(slot);
    GC_EXPECT_EQ(rs.Size(), 1u);
    buf.Flush(rs);
    GC_EXPECT_EQ(rs.Size(), 1u);
    std::unordered_set<MAddress> drained;
    rs.DrainForMinor(drained);
    GC_EXPECT_TRUE(drained.count(slot) == 1);
    GC_EXPECT_EQ(drained.size(), 1u);
}
