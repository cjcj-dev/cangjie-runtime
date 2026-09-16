// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <unordered_set>
#include <vector>

#if defined(__linux__)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Match the existing test_young_conc test-peer shape: this TU alone needs the
// CollectorProxy friendship to publish a real product TRACE phase.  Product
// libraries retain their configured macro set.
#ifndef MRT_TESTABLE_INTERNALS
#define MRT_TESTABLE_INTERNALS 1
#endif

#include <thread>

#include "gc_heap_fixture.hpp"
#include "Concurrency/ConcurrencyModel.h"
#include "gc_unittest.hpp"
#include "b09_runtime_fixture.hpp"

#define private public
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#undef private

#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Mutator/Mutator.h"
#include "mark_publication_fixture.hpp"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/RefField.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" void CJ_MCC_PostWriteRefField(ObjectPtr ref, ObjectPtr obj, RefField<false>* field,
                                          uintptr_t observedPrev);

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

// A store-bad / load-good previous value exercises the ZGC store slow path.
zpointer StoreBadPointer(BaseObject* object)
{
    return to_zpointer(raw(StoreGoodPointer(object)) ^ ZPointerMarkedOldMask);
}

class StoreBufferCollector final : public Collector {
public:
    void MarkOldObjectIfActive(BaseObject* object, bool gcThread = false) const override
    { MarkPublicationFixture::Current().collector.MarkOldObjectIfActive(object, gcThread); }
    void MarkYoungObjectIfActive(BaseObject* object) const override
    { MarkPublicationFixture::Current().collector.MarkYoungObjectIfActive(object); }
    GCCycleSnapshot GetCycleSnapshot(GCCycleGeneration generation) const override
    { return MarkPublicationFixture::Current().collector.GetCycleSnapshot(generation); }
    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    FindToVersionResult FindToVersion(BaseObject*, Generation) const override
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

// Establish the barrier's TLS input without registering a synthetic thread in
// the global safepoint manager. Thread registration is outside this fixture.
class InstalledMutatorScope final {
public:
    explicit InstalledMutatorScope(Mutator& mutator) : saved(ThreadLocal::GetMutator())
    { ThreadLocal::SetMutator(&mutator); }
    ~InstalledMutatorScope() { ThreadLocal::SetMutator(saved); }
private:
    Mutator* saved;
};

class InstalledBarrierScope final {
public:
    explicit InstalledBarrierScope(Barrier& barrier) : previous(Heap::barrierPtr)
    {
        Heap::barrierPtr = &barrier;
    }

    ~InstalledBarrierScope() { Heap::barrierPtr = previous; }

private:
    Barrier* previous;
};

} // namespace

GC_TEST(StoreBuf, EntryCarriesPairedPrevAndInstallColour)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 8);
    const zpointer prev = RefField<>(fx.obj0, ::g_cjStoreGoodMask).GetFieldValue();

    buf.Add(slot, prev, rs);

    const StoreBarrierEntry& entry = buf.buffer[buf.current];
    GC_EXPECT_EQ(entry.p, slot);
    GC_EXPECT_EQ(raw(entry.prev), raw(prev));
    GC_EXPECT_EQ(buf.LastProcessedColorForTest(), static_cast<uintptr_t>(::g_cjStoreGoodMask));
}

GC_TEST(StoreBuf, ProductWriteCarriesOldValueOnlyInPrevArm)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);

    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    Barrier barrier(collector, rs);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);

    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const zpointer prev = StoreBadPointer(fx.obj0);
    field.StoreColoured(prev);

    Heap& heap = Heap::GetHeap();
    CollectorResources& resources = heap.GetCollectorResources();
    RelocationReceiptTestAccess::EnsureCollectorProxyBound(resources);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    const GCPhase phaseBefore = heap.GetGCPhase(GCCycleGeneration::OLD);
    auto& activityCycle = Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::OLD);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    resources.GetGCStats().reason = GC_REASON_USER;
    heap.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_TRACE);

    Mutator mutator;
    mutator.SetMutatorPhase(GCPhase::GC_PHASE_TRACE);
    InstalledMutatorScope mutatorScope(mutator);
    barrier.WriteReference(fx.obj0, field, fx.obj1);
    heap.SetGCPhase(GCCycleGeneration::OLD, phaseBefore);
    resources.GetGCStats().reason = reasonBefore;
    if (!ownerWasActive) activityCycle.End();

    StoreBarrierBuffer& buf = *ThreadLocal::GetGCData().storeBarrierBuffer;
    const size_t pending = buf.Pending();
    std::vector<BaseObject*> retired;
    // No new-value marking is part of the ZGC SATB store barrier.
    // Draining before the flush must therefore find no old-value work.
    DrainPublishedMarkObjects(retired);
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
    DrainPublishedMarkObjects(retired);
    // The product TraceBarrier path contributes exactly one SATB retirement;
    // its former direct enqueue was removed, leaving the paired flush as the
    // sole producer for this runtime-domain write.
    GC_EXPECT_EQ(retired.size(), 1u);
}

GC_TEST(StoreBuf, ProductPhaseFlushHandsPairedPrevToMark)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);

    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    Barrier barrier(collector, rs);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);

    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreBadPointer(fx.obj0));

    Heap& heap = Heap::GetHeap();
    CollectorResources& resources = heap.GetCollectorResources();
    RelocationReceiptTestAccess::EnsureCollectorProxyBound(resources);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    const GCPhase phaseBefore = heap.GetGCPhase(GCCycleGeneration::OLD);
    auto& activityCycle = Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::OLD);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    resources.GetGCStats().reason = GC_REASON_USER;
    heap.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_TRACE);

    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    retired.clear();
    Mutator mutator;
    mutator.SetMutatorPhase(GCPhase::GC_PHASE_TRACE);
    ThreadLocal::SetAllocBuffer(&alloc);
#if defined(MRT_TESTABLE_INTERNALS)
    mutator.SetStoreBarrierRememberedSetForTest(&rs);
#endif
    InstalledMutatorScope mutatorScope(mutator);
    barrier.WriteReference(fx.obj0, field, fx.obj1);
    GC_EXPECT_EQ(ThreadLocal::GetGCData().storeBarrierBuffer->Pending(), 1u);
    mutator.TransitionToGCPhaseExclusive(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);
    GC_EXPECT_TRUE(ThreadLocal::GetGCData().storeBarrierBuffer->IsEmpty());
    heap.SetGCPhase(GCCycleGeneration::OLD, phaseBefore);
    resources.GetGCStats().reason = reasonBefore;
    if (!ownerWasActive) activityCycle.End();

    DrainPublishedMarkObjects(retired);
    size_t oldCount = 0;
    size_t newCount = 0;
    for (BaseObject* object : retired) {
        oldCount += object == fx.obj0 ? 1u : 0u;
        newCount += object == fx.obj1 ? 1u : 0u;
    }
    GC_EXPECT_EQ(oldCount, 1u);
    GC_EXPECT_EQ(newCount, 0u);
}

// ZGC store_barrier_on_heap_oop_field (zBarrier.inline.hpp:695-706) keys on the
// field address, not the holder. A null obj with a heap slot still buffers (p,prev).
GC_TEST(StoreBuf, ProductNullHolderBypassesPendingRelocationEntry)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    Barrier barrier(collector, rs);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);
    Mutator mutator;
    InstalledMutatorScope mutatorScope(mutator);
    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const zpointer prev = StoreBadPointer(fx.obj0);
    field.StoreColoured(prev);

    barrier.WriteReference(nullptr, field, fx.obj1);

    StoreBarrierBuffer& buf = *ThreadLocal::GetGCData().storeBarrierBuffer;
    GC_EXPECT_EQ(buf.Pending(), 1u);
    if (buf.Pending() == 1u) {
        GC_EXPECT_EQ(buf.buffer[buf.current].p, reinterpret_cast<MAddress>(&field));
        GC_EXPECT_EQ(raw(buf.buffer[buf.current].prev), raw(prev));
    }
    std::fprintf(stderr, "TARGET_HOLDER_MARK_AND_REMEMBER_EXECUTED\n");
}

// ZGC: holder identity is not a store-buffer key; heap field p is buffered.
GC_TEST(StoreBuf, ProductNonHeapHolderBypassesPendingRelocationEntry)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    Barrier barrier(collector, rs);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);
    Mutator mutator;
    InstalledMutatorScope mutatorScope(mutator);
    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const zpointer prev = StoreBadPointer(fx.obj0);
    field.StoreColoured(prev);
    alignas(8) unsigned char nativeStorage[128] {};
    BaseObject* nonHeapHolder = fx.PlaceObject(reinterpret_cast<MAddress>(nativeStorage));
    GC_EXPECT_TRUE(nonHeapHolder != nullptr);
    GC_EXPECT_FALSE(Heap::IsHeapAddress(nonHeapHolder));

    barrier.WriteReference(nonHeapHolder, field, fx.obj1);

    StoreBarrierBuffer& buf = *ThreadLocal::GetGCData().storeBarrierBuffer;
    GC_EXPECT_EQ(buf.Pending(), 1u);
    if (buf.Pending() == 1u) {
        GC_EXPECT_EQ(buf.buffer[buf.current].p, reinterpret_cast<MAddress>(&field));
        GC_EXPECT_EQ(raw(buf.buffer[buf.current].prev), raw(prev));
    }
    std::fprintf(stderr, "TARGET_HOLDER_MARK_AND_REMEMBER_EXECUTED\n");
}

// Deterministic compiler-hit object graph: oldReferent is reachable only from
// holder.field before the overwrite; newReferent is reachable from that field
// afterwards. A store-bad old word sends the compiler ABI through the slow path; the current
// old-mark epoch retires oldReferent when the paired store buffer is flushed.
GC_TEST(StoreBuf, CompilerStoreBadOverwriteHandsObservedOldToMark)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);

    BaseObject* const holder = fx.obj0;
    BaseObject* const oldReferent = fx.PlaceObject(fx.heapStart + 256);
    BaseObject* const newReferent = fx.obj1;
    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);

    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    Barrier barrier(collector, rs);
    InstalledBarrierScope installedBarrier(barrier);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);

    const zpointer oldWord = StoreBadPointer(oldReferent);
    field.StoreColoured(oldWord);
    const uintptr_t observedPrev = raw(field.GetFieldValue());
    const bool compilerHit = (observedPrev & static_cast<uintptr_t>(::g_cjStoreBadMask)) == 0;
    const zpointer newWord = StoreGoodPointer(newReferent);

    Heap& heap = Heap::GetHeap();
    CollectorResources& resources = heap.GetCollectorResources();
    RelocationReceiptTestAccess::EnsureCollectorProxyBound(resources);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    const GCPhase phaseBefore = heap.GetGCPhase(GCCycleGeneration::OLD);
    auto& activityCycle = Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::OLD);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    resources.GetGCStats().reason = GC_REASON_USER;
    heap.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_TRACE);

    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    retired.clear();
    Mutator mutator;
    mutator.SetMutatorPhase(GCPhase::GC_PHASE_TRACE);
    ThreadLocal::SetAllocBuffer(&alloc);
#if defined(MRT_TESTABLE_INTERNALS)
    mutator.SetStoreBarrierRememberedSetForTest(&rs);
#endif
    InstalledMutatorScope mutatorScope(mutator);

    // This is the compiler slow-arm ordering: capture, overwrite, then ABI exit.
    field.StoreColoured(newWord);
    CJ_MCC_PostWriteRefField(newReferent, holder, &field, observedPrev);
    const size_t pending = ThreadLocal::GetGCData().storeBarrierBuffer->Pending();
    mutator.TransitionToGCPhaseExclusive(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER);

    heap.SetGCPhase(GCCycleGeneration::OLD, phaseBefore);
    resources.GetGCStats().reason = reasonBefore;
    if (!ownerWasActive) activityCycle.End();

    DrainPublishedMarkObjects(retired);
    size_t oldReceipts = 0;
    size_t newReceipts = 0;
    for (BaseObject* object : retired) {
        oldReceipts += object == oldReferent ? 1u : 0u;
        newReceipts += object == newReferent ? 1u : 0u;
    }
    std::fprintf(stderr,
                 "DETAIL arm=compiler_store_bad_overwrite compiler_hit=%u observed_prev=0x%zx installed=0x%zx "
                 "pending=%zu old_receipts=%zu new_receipts=%zu field_target=%p\n",
                 static_cast<unsigned>(compilerHit), static_cast<size_t>(observedPrev),
                 static_cast<size_t>(raw(field.GetFieldValue())), pending, oldReceipts, newReceipts,
                 static_cast<void*>(to_object(field.GetTargetObject())));
    std::fflush(stderr);

    GC_EXPECT_FALSE(compilerHit);
    GC_EXPECT_EQ(pending, 1u);
    GC_EXPECT_TRUE(ThreadLocal::GetGCData().storeBarrierBuffer->IsEmpty());
    GC_EXPECT_EQ(oldReceipts, 1u);
    GC_EXPECT_EQ(newReceipts, 0u);
    GC_EXPECT_EQ(raw(field.GetTargetObject()), reinterpret_cast<MAddress>(newReferent));
}

GC_TEST(StoreBuf, GcAssistedPhaseFlushDefersStoreBuffer)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);

    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    Barrier barrier(collector, rs);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);

    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreBadPointer(fx.obj0));

    Heap& heap = Heap::GetHeap();
    CollectorResources& resources = heap.GetCollectorResources();
    RelocationReceiptTestAccess::EnsureCollectorProxyBound(resources);
    const bool startedBefore = resources.IsGcStarted();
    const GCReason reasonBefore = resources.GetGCStats().reason;
    const GCPhase phaseBefore = heap.GetGCPhase(GCCycleGeneration::OLD);
    auto& activityCycle = Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::OLD);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    resources.GetGCStats().reason = GC_REASON_USER;
    heap.SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_TRACE);

    Mutator mutator;
    mutator.SetMutatorPhase(GCPhase::GC_PHASE_TRACE);
    ThreadLocal::SetAllocBuffer(&alloc);
#if defined(MRT_TESTABLE_INTERNALS)
    mutator.SetStoreBarrierRememberedSetForTest(&rs);
#endif
    InstalledMutatorScope mutatorScope(mutator);
    barrier.WriteReference(fx.obj0, field, fx.obj1);
    GC_EXPECT_EQ(ThreadLocal::GetGCData().storeBarrierBuffer->Pending(), 1u);

    // A GC worker assisting a saferegion transition must not consume the
    // paired store entry: ZGC on_new_phase runs in the Java-thread flush.
    mutator.TransitionToGCPhaseExclusive(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER, false);
    GC_EXPECT_EQ(ThreadLocal::GetGCData().storeBarrierBuffer->Pending(), 1u);
    // The mutator-side transition (or the next explicit safepoint) consumes it.
    mutator.TransitionToGCPhaseExclusive(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER, true);
    GC_EXPECT_TRUE(ThreadLocal::GetGCData().storeBarrierBuffer->IsEmpty());

    heap.SetGCPhase(GCCycleGeneration::OLD, phaseBefore);
    resources.GetGCStats().reason = reasonBefore;
    if (!ownerWasActive) activityCycle.End();
}

GC_TEST(StoreBuf, NonNullPrevPublishesMarkBeforeRememberingSlot)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    StoreBarrierBuffer buf;
    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    retired.clear();

    const MAddress slot = SlotAt(fx, 8);
    const zpointer prev = RefField<>(fx.obj0, ::g_cjStoreGoodMask).GetFieldValue();
#if defined(MRT_GC_UNIT_TESTS)
    std::vector<StoreBarrierFlushEvent> events;
    FlushObserverScope observe(events);
#endif
    buf.Add(slot, prev, rs);
    buf.Flush(rs, collector);
    DrainPublishedMarkObjects(retired);

    GC_EXPECT_EQ(retired.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(retired[0]), reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_TRUE(Heap::GetHeap().GetRememberedSet().Contains(slot));
#if defined(MRT_GC_UNIT_TESTS)
    GC_EXPECT_EQ(events.size(), 2u);
    GC_EXPECT_EQ(events[0], StoreBarrierFlushEvent::PREVIOUS_RETIRED);
    GC_EXPECT_EQ(events[1], StoreBarrierFlushEvent::SLOT_REMEMBERED);
#endif
}

GC_TEST(StoreBuf, NullPrevOnlyRemembersSlot)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    retired.clear();

    const MAddress slot = SlotAt(fx, 8);
    buf.Add(slot, zpointer::null, rs);
    buf.Flush(rs);
    DrainPublishedMarkObjects(retired);

    GC_EXPECT_TRUE(retired.empty());
    GC_EXPECT_TRUE(Heap::GetHeap().GetRememberedSet().Contains(slot));
}

GC_TEST(StoreBuf, NullAndPreMarkPreviousAreNormalSkips)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 8);
    HeapSlotAt<>(slot).StoreColoured(zpointer::null);
    const uintptr_t saved = ::g_cjStoreGoodMask;
    const zpointer previous = RefField<>(fx.obj0, saved).GetFieldValue();
    buf.Add(slot, previous, rs);
    buf.Add(SlotAt(fx, 9), zpointer::null, rs);
    ::g_cjStoreGoodMask ^= ZPointerMarkedOldMask;
    buf.Flush(rs);
    ::g_cjStoreGoodMask = saved;
    std::vector<BaseObject*> marked;
    markFixture.DrainObjects(marked);
    GC_EXPECT_EQ(marked.size(), 1u);
    GC_EXPECT_TRUE(Heap::GetHeap().GetRememberedSet().Contains(slot));
    GC_EXPECT_TRUE(buf.IsEmpty());
}

GC_TEST(StoreBuf, ResolvedInvalidPreviousIsClassifiedAndCleared)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    StoreBarrierBuffer buf;
    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    retired.clear();

    uintptr_t outsideHeap = 0;
    const zpointer previous = RefField<>(reinterpret_cast<BaseObject*>(&outsideHeap),
                                         static_cast<uintptr_t>(::g_cjStoreGoodMask)).GetFieldValue();
    const uintptr_t colour = static_cast<uintptr_t>(::g_cjStoreGoodMask);
    const MAddress slot = SlotAt(fx, 8);
#if defined(MRT_GC_UNIT_TESTS)
    std::vector<StoreBarrierFlushEvent> events;
    FlushObserverScope observe(events);
#endif
    buf.Add(slot, previous, rs);
    buf.Flush(rs, collector);
    DrainPublishedMarkObjects(retired);

    std::fprintf(stderr,
                 "DETAIL arm=resolved_invalid prev=%#zx installed_phase=%u installed_store_good=%#zx "
                 "retired_receipts=%zu current=%zu slot_remembered=%u\n",
                 static_cast<size_t>(raw(previous)), static_cast<unsigned>(GCPhase::GC_PHASE_TRACE),
                 static_cast<size_t>(colour), retired.size(), buf.Current(),
                 static_cast<unsigned>(Heap::GetHeap().GetRememberedSet().Contains(slot)));
    std::fflush(stderr);
    GC_EXPECT_TRUE(retired.empty());
    GC_EXPECT_EQ(buf.Current(), StoreBarrierBuffer::Capacity());
    GC_EXPECT_TRUE(Heap::GetHeap().GetRememberedSet().Contains(slot));
#if defined(MRT_GC_UNIT_TESTS)
    GC_EXPECT_EQ(events.size(), 2u);
    GC_EXPECT_EQ(events[0], StoreBarrierFlushEvent::PREVIOUS_INVALID);
    GC_EXPECT_EQ(events[1], StoreBarrierFlushEvent::SLOT_REMEMBERED);
#endif
}

#if defined(MRT_GC_UNIT_TESTS) && defined(__linux__)
GC_TEST(StoreBuf, YoungSlotExcludedFromOldPhaseSnapshot)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 8);
    HeapSlotAt<>(slot).StoreColoured(zpointer::null);
    const uintptr_t saved = ::g_cjStoreGoodMask;
    const zpointer previous = RefField<>(fx.obj0, saved).GetFieldValue();
    fx.region0->SetYoungRegionFlag(1);
    buf.Add(slot, previous, rs);
    ::g_cjStoreGoodMask ^= ZPointerMarkedYoungMask;
    buf.Flush(rs);
    ::g_cjStoreGoodMask = saved;
    std::vector<BaseObject*> marked;
    markFixture.DrainObjects(marked);
    GC_EXPECT_TRUE(marked.empty());
    GC_EXPECT_FALSE(Heap::GetHeap().GetRememberedSet().Contains(slot));
    GC_EXPECT_TRUE(buf.IsEmpty());
}
#endif

GC_TEST(StoreBuf, YoungHolderRetiresPrevWithoutRememberingSlot)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region1->SetYoungRegionFlag(1);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    StoreBarrierBuffer buf;
    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    retired.clear();

    const MAddress slot = reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE;
    const uintptr_t colour = static_cast<uintptr_t>(::g_cjStoreGoodMask);
    const zpointer prev = RefField<>(fx.obj0, colour).GetFieldValue();
    buf.Add(slot, prev, rs);
    buf.Flush(rs, collector);
    DrainPublishedMarkObjects(retired);

    GC_EXPECT_EQ(retired.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(retired[0]), reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_TRUE(!Heap::GetHeap().GetRememberedSet().Contains(slot));
}

GC_TEST(StoreBuf, AddConsumesPreviousPhaseBeforeCurrentEntry)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 8);
    HeapSlotAt<>(slot).StoreColoured(zpointer::null);
    const uintptr_t saved = ::g_cjStoreGoodMask;
    const zpointer previous = RefField<>(fx.obj0, saved).GetFieldValue();
    buf.Add(slot, previous, rs);
    ::g_cjStoreGoodMask ^= ZPointerMarkedOldMask;
    const MAddress currentSlot = SlotAt(fx, 9);
    buf.Add(currentSlot, RefField<>(fx.obj1, ::g_cjStoreGoodMask).GetFieldValue(), rs);
    GC_EXPECT_EQ(buf.Pending(), 2u);
    buf.Flush(rs);
    ::g_cjStoreGoodMask = saved;
    std::vector<BaseObject*> marked;
    markFixture.DrainObjects(marked);
    GC_EXPECT_EQ(marked.size(), 2u);
    GC_EXPECT_TRUE(Heap::GetHeap().GetRememberedSet().Contains(slot) && Heap::GetHeap().GetRememberedSet().Contains(currentSlot));
}

GC_TEST(StoreBuf, PendingEntryFromOldEpochIsRejectedAfterOldMarkFlip)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    StoreBarrierBuffer buf;
    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    retired.clear();

    const uintptr_t before = static_cast<uintptr_t>(::g_cjStoreGoodMask);
    const zpointer prev = RefField<>(fx.obj0, before).GetFieldValue();
    buf.Add(SlotAt(fx, 12), prev, rs);
    // Publish the next old-mark epoch before this thread drains.  The pending
    // entry belongs to the install-time epoch and must not enter the new SATB.
    ::g_cjStoreGoodMask = before ^ ZPointerMarkedOldMask;
    buf.Flush(rs, collector);
    ::g_cjStoreGoodMask = before;
    DrainPublishedMarkObjects(retired);

    GC_EXPECT_EQ(retired.size(), 1u);
    GC_EXPECT_TRUE(Heap::GetHeap().GetRememberedSet().Contains(SlotAt(fx, 12)));
}

GC_TEST(StoreBuf, PendingOldMarkEntrySurvivesYoungMarkFlip)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    StoreBarrierBuffer buf;
    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    retired.clear();

    const uintptr_t before = static_cast<uintptr_t>(::g_cjStoreGoodMask);
    const zpointer prev = RefField<>(fx.obj0, before).GetFieldValue();
    buf.Add(SlotAt(fx, 13), prev, rs);
    // A young-mark publication does not change the old-mark epoch that owns
    // this entry, so its SATB half must still be retired after the flip.
    ::g_cjStoreGoodMask = before ^ ZPointerMarkedYoungMask;
    buf.Flush(rs, collector);
    ::g_cjStoreGoodMask = before;
    DrainPublishedMarkObjects(retired);

    GC_EXPECT_EQ(retired.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(retired[0]), reinterpret_cast<MAddress>(fx.obj0));
}

GC_TEST(StoreBuf, PhaseFlipLeavesOnePreviousAndOneCurrentSlot)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress previousSlot = SlotAt(fx, 8);
    const MAddress currentSlot = SlotAt(fx, 9);

    RememberedSet& heapRs = Heap::GetHeap().GetRememberedSet();
    buf.Add(previousSlot, zpointer::null, rs);
    buf.Flush(rs);
    heapRs.FlipForMinor();
    buf.Add(currentSlot, zpointer::null, rs);
    buf.Flush(rs);

    std::unordered_set<MAddress> previous;
    GC_EXPECT_EQ(heapRs.ScanPreviousForMinor(previous), 1u);
    GC_EXPECT_EQ(previous.size(), 1u);
    GC_EXPECT_TRUE(previous.count(previousSlot) == 1);
    GC_EXPECT_TRUE(heapRs.Contains(currentSlot));
}

GC_TEST(StoreBuf, FullAutoFlushKeepsEveryEntry)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const size_t n = StoreBarrierBuffer::Capacity() + 1;
    for (size_t i = 0; i < n; ++i) {
        buf.Add(SlotAt(fx, i + 8), zpointer::null, rs);
    }
    GC_EXPECT_EQ(buf.Pending(), 1u);
    RememberedSet& heapRs = Heap::GetHeap().GetRememberedSet();
    GC_EXPECT_EQ(heapRs.Size(), StoreBarrierBuffer::Capacity());
    buf.Flush(rs);
    GC_EXPECT_TRUE(buf.IsEmpty());
    for (size_t i = 0; i < n; ++i) {
        GC_EXPECT_TRUE(heapRs.Contains(SlotAt(fx, i + 8)));
    }
}

GC_TEST(StoreBuf, UnflushedPendingInvisibleToDrain)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
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
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 8);
    buf.Add(slot, zpointer::null, rs);
    RememberedSet& heapRs = Heap::GetHeap().GetRememberedSet();
    GC_EXPECT_TRUE(!heapRs.Contains(slot));
    buf.Flush(rs);
    GC_EXPECT_TRUE(heapRs.Contains(slot));
}

GC_TEST(StoreBuf, MarkEndSnapshotLeavesCurrentForNextMinor)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 11);
    buf.Add(slot, zpointer::null, rs);
    buf.Flush(rs);
    GC_EXPECT_TRUE(Heap::GetHeap().GetRememberedSet().Contains(slot));
}

GC_TEST(StoreBuf, FlushBeforeMinorDoesNotLoseEdges)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const size_t n = 7;
    for (size_t i = 0; i < n; ++i) {
        buf.Add(SlotAt(fx, i + 8), zpointer::null, rs);
    }
    buf.Flush(rs);
    RememberedSet& heapRs = Heap::GetHeap().GetRememberedSet();
    for (size_t i = 0; i < n; ++i) {
        GC_EXPECT_TRUE(heapRs.Contains(SlotAt(fx, i + 8)));
    }
}

GC_TEST(StoreBuf, ThreadExitFlushRedeems)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 9);
    buf.Add(slot, zpointer::null, rs);
    buf.Flush(rs);
    GC_EXPECT_TRUE(Heap::GetHeap().GetRememberedSet().Contains(slot));
}

GC_TEST(StoreBuf, ReRememberDoesNotFightBuffer)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
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

// ZThreadLocalData + ZMark::flush: detach publishes both generation stacks,
// including non-full chunks, even when this OS thread owns no allocator.
GC_OTHER_VM_TEST(StoreBarrierBuffer, DetachPublishesBothGenerationsWithoutAllocator)
{
    MapleRuntime::GcUnit::B09RuntimeFixture runtime;
    GcHeapFixture heap;
    MarkPublicationFixture marking;
    std::thread owner([&] {
        ThreadLocal::SetAllocBuffer(nullptr);
        RegisterCurrentMarkFlushThread();
        marking.collector.PublishThreadRoot(heap.obj0, true, true);
        marking.collector.PublishThreadRoot(heap.obj1, false, false);
        MutatorManager::Instance().UnregisterMarkFlushThread(ThreadLocal::GetThreadLocalData());
    });
    owner.join();
    size_t young = 0;
    size_t old = 0;
    marking.DrainDomain(*marking.collector.YoungMarkDomain(), [&](BaseObject* object, bool follow) {
        GC_EXPECT_TRUE(object == heap.obj0);
        GC_EXPECT_TRUE(follow);
        ++young;
    });
    marking.DrainOld([&](BaseObject* object, bool follow) {
        GC_EXPECT_TRUE(object == heap.obj1);
        GC_EXPECT_TRUE(!follow);
        ++old;
    });
    GC_EXPECT_EQ(young, 1u);
    GC_EXPECT_EQ(old, 1u);
}

// ZBarrier::no_keep_alive_store_barrier_on_heap_oop_field (zBarrier.inline.hpp:719):
// raw null takes remember(p), whereas an ordinary strong store may bypass it.
GC_TEST(StoreBuf, WeakRawNullStoreRetainsRememberedSlot)
{
    for (bool preloaded : {false, true}) {
        {
            const bool weak = true;
            GcHeapFixture fx;
            fx.region0->SetYoungRegionFlag(0);
            fx.region1->SetYoungRegionFlag(1);
            fx.typeInfo->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
            RememberedSet rs;
            rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
            StoreBufferCollector collector;
            Barrier barrier(collector, rs);
            AllocBuffer alloc;
            AllocBufferScope allocScope(alloc);
            Mutator mutator;
            InstalledMutatorScope mutatorScope(mutator);
            HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
            field.StoreColoured(zpointer::null);
            if (preloaded) {
                field.StoreColoured(StoreGoodPointer(fx.obj1));
                barrier.PostWriteReference(fx.obj0, field, fx.obj1, zpointer::null);
            } else {
                barrier.WriteReference(fx.obj0, field, fx.obj1);
            }
            GC_EXPECT_EQ(Heap::GetHeap().GetRememberedSet().Contains(reinterpret_cast<MAddress>(&field)), weak);
            GC_EXPECT_TRUE(to_object(field.GetTargetObject()) == fx.obj1);
        }
    }
}

// Derived from ZBarrierSet::oop_atomic_{cmpxchg,xchg}_not_in_heap and
// ZBarrier::self_heal: native atomics publish store-good, including null.
GC_TEST(StoreBuf, NativeAtomicUsesColoredHealingAndCompareValue)
{
    GcHeapFixture fx;
    StoreBufferCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier barrier(collector, rs);
    HeapSlot<true> native(zpointer::null);
    barrier.AtomicWriteReference(nullptr, native, nullptr, std::memory_order_seq_cst);
    GC_EXPECT_EQ(native.GetFieldValue(), StoreGoodPointer(nullptr));
    GC_EXPECT_TRUE(barrier.CompareAndSwapReference(nullptr, native, nullptr, fx.obj0,
        std::memory_order_seq_cst, std::memory_order_seq_cst));
    GC_EXPECT_EQ(native.GetFieldValue(), StoreGoodPointer(fx.obj0));
    GC_EXPECT_FALSE(barrier.CompareAndSwapReference(nullptr, native, nullptr, fx.obj1,
        std::memory_order_seq_cst, std::memory_order_seq_cst));
    GC_EXPECT_TRUE(barrier.AtomicSwapReference(nullptr, native, fx.obj1, std::memory_order_seq_cst) == fx.obj0);
    GC_EXPECT_TRUE(barrier.AtomicReadReference(nullptr, native, std::memory_order_seq_cst) == fx.obj1);
    GC_EXPECT_EQ(native.GetFieldValue(), StoreGoodPointer(fx.obj1));
}

// ZBarrierSet::value_copy_in_heap / oop_copy_one_barriers at the explicit
// uncolored-local <-> native-zpointer <-> heap-zpointer boundaries.
GC_TEST(StoreBuf, BulkPreservesSourceStorageProtocol)
{
    GcHeapFixture fx;
    StoreBufferCollector collector;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier barrier(collector, rs);
    RootSlot local;
    StorePlain(local, from_object(fx.obj0));
    NativeSlot native(zpointer::null);
    const GCTib layout = fx.obj0->GetGCTib();
    barrier.WriteStaticStruct(reinterpret_cast<MAddress>(&native), sizeof(native),
        reinterpret_cast<MAddress>(&local), sizeof(local), layout);
    GC_EXPECT_EQ(native.GetFieldValue(), StoreGoodPointer(fx.obj0));
    RootSlot result;
    barrier.ReadStaticStruct(reinterpret_cast<MAddress>(&result), reinterpret_cast<MAddress>(&native),
        sizeof(native), layout);
    GC_EXPECT_EQ(raw(result.LoadPlain()), reinterpret_cast<uintptr_t>(fx.obj0));
    HeapSlot<>& heap = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    heap.StoreColoured(zpointer::null);
    barrier.ReadStaticStruct(reinterpret_cast<MAddress>(&heap), reinterpret_cast<MAddress>(&native),
        sizeof(native), layout);
    GC_EXPECT_EQ(heap.GetFieldValue(), StoreGoodPointer(fx.obj0));
    GC_EXPECT_EQ(raw(local.LoadPlain()), reinterpret_cast<uintptr_t>(fx.obj0));
}

extern "C" void MRT_VisitorCaller(void*, void*);

GC_TEST(StoreBuf, ThreadRootVisitorIncludesExecuteClosure)
{
    GcHeapFixture fx;
    LWTData data {};
    data.execute = fx.obj0;
    size_t executeVisits = 0;
    RootVisitor visitor = [&](RootSlot& root) {
        if (&root == &RootSlotAt(&data.execute)) {
            ++executeVisits;
            GC_EXPECT_EQ(raw(root.LoadPlain()), reinterpret_cast<uintptr_t>(fx.obj0));
            StorePlain(root, from_object(fx.obj1));
        }
    };
    MRT_VisitorCaller(&data, &visitor);
    GC_EXPECT_EQ(executeVisits, 1u);
    GC_EXPECT_TRUE(data.execute == static_cast<void*>(fx.obj1));
}

// ZBarrier::store_barrier_on_heap_oop_field: a store-good old word is a fast exit.
// Paired with CompilerStoreBadOverwriteHandsObservedOldToMark as its control.
GC_TEST(StoreBuf, CompilerStoreGoodOverwriteSkipsMarkAndBuffer)
{
    GcHeapFixture fx;
    MarkPublicationFixture marking;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
    StoreBufferCollector collector;
    Barrier barrier(collector, rs);
    InstalledBarrierScope installed(barrier);
    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const zpointer previous = StoreGoodPointer(fx.obj0);
    field.StoreColoured(StoreGoodPointer(fx.obj1));
    CJ_MCC_PostWriteRefField(fx.obj1, fx.obj0, &field, raw(previous));
    std::vector<BaseObject*> marked;
    marking.DrainObjects(marked);
    GC_EXPECT_TRUE(marked.empty());
    GC_EXPECT_TRUE(ThreadLocal::GetGCData().storeBarrierBuffer->IsEmpty());
    GC_EXPECT_FALSE(Heap::GetHeap().GetRememberedSet().Contains(reinterpret_cast<MAddress>(&field)));
    GC_EXPECT_EQ(field.GetFieldValue(), StoreGoodPointer(fx.obj1));
}
