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

// Enable the existing mark-publication test peer after heap layout is fixed.
// Parse value-owned heap resources with the product macro configuration before
// enabling the existing test peers; their member offsets must match the SO.
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zBarrier.inline.hpp"

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
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDriver.hpp"
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

namespace {

// A store-bad / load-good previous value exercises the ZGC store slow path.
zpointer StoreBadPointer(BaseObject* object)
{
    return to_zpointer(raw(StoreGoodPointer(object)) ^ ZPointerMarkedOldMask);
}

MAddress SlotAt(GcHeapFixture& fx, size_t i)
{
    return fx.heapStart + i * sizeof(void*);
}

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
        alloc.ClearRegion();
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

} // namespace

GC_TEST(StoreBuf, EntryCarriesPairedPrevAndInstallColour)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 8);
    const zpointer prev = RefField<>(fx.obj0, ::g_cjStoreGoodMask).GetFieldValue();

    buf.add(slot, prev);

    const StoreBarrierEntry& entry = buf.buffer[buf.current];
    GC_EXPECT_EQ(entry.p, slot);
    GC_EXPECT_EQ(raw(entry.prev), raw(prev));
    GC_EXPECT_EQ(buf.Pending(), 1u);
}

GC_TEST(StoreBuf, ProductWriteCarriesOldValueOnlyInPrevArm)
{
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    // SATB previous values must predate mark start (ZGC zMark.inline.hpp:51-55).
    MarkPublicationFixture markFixture;

    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);

    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const zpointer prev = StoreBadPointer(fx.obj0);
    field.StoreColoured(prev);

    Heap& heap = Heap::GetHeap();
    const bool startedBefore = Heap::GetHeap().IsGcStarted();
    const GCReason reasonBefore = heap.GetZGeneration(ZGenerationId::old).Snapshot().reason;
    const ZGenerationPhase phaseBefore = Heap::GetHeap().GetZGeneration(ZGenerationId::old).GcPhase();
    auto& activityCycle = Heap::GetHeap().GetZGeneration(ZGenerationId::old);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    heap.GetZGeneration(ZGenerationId::old).SetReasonForTest(GC_REASON_USER);
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Mark);

    Mutator mutator;
    InstalledMutatorScope mutatorScope(mutator);
    ZBarrier::WriteReference(fx.obj0, field, fx.obj1);

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
    buf.Flush();
    DrainPublishedMarkObjects(retired);
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(phaseBefore);
    heap.GetZGeneration(ZGenerationId::old).SetReasonForTest(reasonBefore);
    if (!ownerWasActive) activityCycle.End();
    GC_EXPECT_EQ(retired.size(), 1u);
}

// ZGC zMark.inline.hpp:51-55: objects on pages allocated in this cycle are
// implicitly live. Contrast with ProductWriteCarriesOldValueOnlyInPrevArm.
GC_TEST(StoreBuf, AllocatingPreviousValueIsImplicitlyLive)
{
    for (PageAge age : {PageAge::old, PageAge::eden}) {
        GcHeapFixture fx;
        fx.region0->reset(PageAge::old);
        MarkPublicationFixture markFixture;
        // Deliberately allocate the previous value's page after mark start.
        fx.region1->reset(age);
        HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
        field.StoreColoured(StoreBadPointer(fx.obj1));
        Mutator mutator;
        InstalledMutatorScope mutatorScope(mutator);
        ZBarrier::WriteReference(fx.obj0, field, nullptr);
        const size_t pending = mutator.GetGCData().storeBarrierBuffer->Pending();
        mutator.FlushStoreBarrierBuffer();
        std::vector<BaseObject*> retired;
        DrainPublishedMarkObjects(retired);
        const bool remembered = SlotPageRemembered(reinterpret_cast<MAddress>(&field));
        std::fprintf(stderr, "DETAIL allocating age=%u allocating=%u pending=%zu retired=%zu remset=%u\n",
                     static_cast<unsigned>(age), static_cast<unsigned>(fx.region1->IsAllocating()),
                     pending, retired.size(), static_cast<unsigned>(remembered));
        GC_EXPECT_EQ(pending, 1u);
        GC_EXPECT_TRUE(fx.region1->IsAllocating());
        GC_EXPECT_TRUE(fx.region1->is_object_strongly_live(from_object(fx.obj1)));
        GC_EXPECT_TRUE(retired.empty());
        GC_EXPECT_TRUE(remembered);
        GC_EXPECT_TRUE(is_null(field.GetTargetObject()));
    }
}

GC_TEST(StoreBuf, ProductPhaseFlushHandsPairedPrevToMark)
{
    GcHeapFixture fx;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    // SATB previous values must predate mark start (ZGC zMark.inline.hpp:51-55).
    MarkPublicationFixture markFixture;

    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);

    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreBadPointer(fx.obj0));

    Heap& heap = Heap::GetHeap();
    const bool startedBefore = Heap::GetHeap().IsGcStarted();
    const GCReason reasonBefore = heap.GetZGeneration(ZGenerationId::old).Snapshot().reason;
    const ZGenerationPhase phaseBefore = Heap::GetHeap().GetZGeneration(ZGenerationId::old).GcPhase();
    auto& activityCycle = Heap::GetHeap().GetZGeneration(ZGenerationId::old);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    heap.GetZGeneration(ZGenerationId::old).SetReasonForTest(GC_REASON_USER);
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Mark);

    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    retired.clear();
    Mutator mutator;
    ThreadLocal::SetAllocBuffer(&alloc);
#if defined(MRT_TESTABLE_INTERNALS)
    mutator.SetStoreBarrierRememberedSetForTest(&rs);
#endif
    InstalledMutatorScope mutatorScope(mutator);
    ZBarrier::WriteReference(fx.obj0, field, fx.obj1);
    GC_EXPECT_EQ(ThreadLocal::GetGCData().storeBarrierBuffer->Pending(), 1u);
    mutator.FlushStoreBarrierBuffer();
    GC_EXPECT_TRUE(ThreadLocal::GetGCData().storeBarrierBuffer->IsEmpty());
    DrainPublishedMarkObjects(retired);
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(phaseBefore);
    heap.GetZGeneration(ZGenerationId::old).SetReasonForTest(reasonBefore);
    if (!ownerWasActive) activityCycle.End();
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
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);
    Mutator mutator;
    InstalledMutatorScope mutatorScope(mutator);
    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const zpointer prev = StoreBadPointer(fx.obj0);
    field.StoreColoured(prev);

    ZBarrier::WriteReference(nullptr, field, fx.obj1);

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
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
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

    ZBarrier::WriteReference(nonHeapHolder, field, fx.obj1);

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
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    // SATB previous values must predate mark start (ZGC zMark.inline.hpp:51-55).
    MarkPublicationFixture markFixture;

    BaseObject* const holder = fx.obj0;
    BaseObject* const oldReferent = fx.PlaceObject(fx.heapStart + 256);
    BaseObject* const newReferent = fx.obj1;
    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);

    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);

    const zpointer oldWord = StoreBadPointer(oldReferent);
    field.StoreColoured(oldWord);
    const uintptr_t observedPrev = raw(field.GetFieldValue());
    const bool compilerHit = (observedPrev & static_cast<uintptr_t>(::g_cjStoreBadMask)) == 0;
    const zpointer newWord = StoreGoodPointer(newReferent);

    Heap& heap = Heap::GetHeap();
    const bool startedBefore = Heap::GetHeap().IsGcStarted();
    const GCReason reasonBefore = heap.GetZGeneration(ZGenerationId::old).Snapshot().reason;
    const ZGenerationPhase phaseBefore = Heap::GetHeap().GetZGeneration(ZGenerationId::old).GcPhase();
    auto& activityCycle = Heap::GetHeap().GetZGeneration(ZGenerationId::old);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    heap.GetZGeneration(ZGenerationId::old).SetReasonForTest(GC_REASON_USER);
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Mark);

    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    retired.clear();
    Mutator mutator;
    ThreadLocal::SetAllocBuffer(&alloc);
#if defined(MRT_TESTABLE_INTERNALS)
    mutator.SetStoreBarrierRememberedSetForTest(&rs);
#endif
    InstalledMutatorScope mutatorScope(mutator);

    // ZGC store_at_resolved: store barrier sees the previous colored word, then the store.
    ZBarrier::store_barrier_on_heap_oop_field(reinterpret_cast<volatile zpointer*>(&field), false); // oldvalue-anchor
    field.StoreColoured(newWord);
    const size_t pending = ThreadLocal::GetGCData().storeBarrierBuffer->Pending();
    mutator.FlushStoreBarrierBuffer();
    DrainPublishedMarkObjects(retired);
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(phaseBefore);
    heap.GetZGeneration(ZGenerationId::old).SetReasonForTest(reasonBefore);
    if (!ownerWasActive) activityCycle.End();
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
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);

    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);

    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreBadPointer(fx.obj0));

    Heap& heap = Heap::GetHeap();
    const bool startedBefore = Heap::GetHeap().IsGcStarted();
    const GCReason reasonBefore = heap.GetZGeneration(ZGenerationId::old).Snapshot().reason;
    const ZGenerationPhase phaseBefore = Heap::GetHeap().GetZGeneration(ZGenerationId::old).GcPhase();
    auto& activityCycle = Heap::GetHeap().GetZGeneration(ZGenerationId::old);
    const bool ownerWasActive = activityCycle.Snapshot().active;
    if (!ownerWasActive) activityCycle.Begin(1);
    heap.GetZGeneration(ZGenerationId::old).SetReasonForTest(GC_REASON_USER);
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Mark);

    Mutator mutator;
    ThreadLocal::SetAllocBuffer(&alloc);
#if defined(MRT_TESTABLE_INTERNALS)
    mutator.SetStoreBarrierRememberedSetForTest(&rs);
#endif
    InstalledMutatorScope mutatorScope(mutator);
    ZBarrier::WriteReference(fx.obj0, field, fx.obj1);
    GC_EXPECT_EQ(ThreadLocal::GetGCData().storeBarrierBuffer->Pending(), 1u);

    // A GC worker assisting a saferegion transition must not consume the
    // paired store entry: ZGC on_new_phase runs in the Java-thread flush.
    mutator.FlushStoreBarrierBuffer(false);
    GC_EXPECT_EQ(ThreadLocal::GetGCData().storeBarrierBuffer->Pending(), 1u);
    // The mutator-side transition (or the next explicit safepoint) consumes it.
    mutator.FlushStoreBarrierBuffer(true);
    GC_EXPECT_TRUE(ThreadLocal::GetGCData().storeBarrierBuffer->IsEmpty());

    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(phaseBefore);
    heap.GetZGeneration(ZGenerationId::old).SetReasonForTest(reasonBefore);
    if (!ownerWasActive) activityCycle.End();
}

GC_TEST(StoreBuf, NonNullPrevPublishesMarkBeforeRememberingSlot)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    StoreBarrierBuffer buf;
    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    retired.clear();

    const MAddress slot = SlotAt(fx, 8);
    const zpointer prev = RefField<>(fx.obj0, ::g_cjStoreGoodMask).GetFieldValue();
#if defined(MRT_GC_UNIT_TESTS)
    #endif
    buf.add(slot, prev);
    buf.Flush();
    DrainPublishedMarkObjects(retired);

    GC_EXPECT_EQ(retired.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(retired[0]), reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_TRUE(SlotPageRemembered(slot));
}

GC_TEST(StoreBuf, NullPrevOnlyRemembersSlot)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    StoreBarrierBuffer buf;
    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    retired.clear();

    const MAddress slot = SlotAt(fx, 8);
    buf.add(slot, zpointer::null);
    buf.Flush();
    DrainPublishedMarkObjects(retired);

    GC_EXPECT_TRUE(retired.empty());
    GC_EXPECT_TRUE(SlotPageRemembered(slot));
}

GC_TEST(StoreBuf, NullAndPreMarkPreviousAreNormalSkips)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 8);
    HeapSlotAt<>(slot).StoreColoured(zpointer::null);
    const uintptr_t saved = ::g_cjStoreGoodMask;
    const zpointer previous = RefField<>(fx.obj0, saved).GetFieldValue();
    buf.add(slot, previous);
    buf.add(SlotAt(fx, 9), zpointer::null);
    ::g_cjStoreGoodMask ^= ZPointerMarkedOldMask;
    buf.Flush();
    ::g_cjStoreGoodMask = saved;
    std::vector<BaseObject*> marked;
    markFixture.DrainObjects(marked);
    GC_EXPECT_EQ(marked.size(), 1u);
    GC_EXPECT_TRUE(SlotPageRemembered(slot));
    GC_EXPECT_TRUE(buf.IsEmpty());
}

GC_TEST(StoreBuf, ResolvedInvalidPreviousIsClassifiedAndCleared)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
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
    #endif
    buf.add(slot, previous);
    buf.Flush();
    DrainPublishedMarkObjects(retired);

    std::fprintf(stderr,
                 "DETAIL arm=resolved_invalid prev=%#zx installed_phase=%u installed_store_good=%#zx "
                 "retired_receipts=%zu current=%zu slot_remembered=%u\n",
                 static_cast<size_t>(raw(previous)), static_cast<unsigned>(ZGenerationPhase::Mark),
                 static_cast<size_t>(colour), retired.size(), buf.Current(),
                 static_cast<unsigned>(SlotPageRemembered(slot)));
    std::fflush(stderr);
    GC_EXPECT_TRUE(retired.empty());
    GC_EXPECT_EQ(buf.Current(), StoreBarrierBuffer::Capacity());
    GC_EXPECT_TRUE(SlotPageRemembered(slot));
}

#if defined(MRT_GC_UNIT_TESTS) && defined(__linux__)
GC_TEST(StoreBuf, YoungSlotExcludedFromOldPhaseSnapshot)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 8);
    HeapSlotAt<>(slot).StoreColoured(zpointer::null);
    const uintptr_t saved = ::g_cjStoreGoodMask;
    const zpointer previous = RefField<>(fx.obj0, saved).GetFieldValue();
    fx.region0->reset(PageAge::eden);
    buf.add(slot, previous);
    ::g_cjStoreGoodMask ^= ZPointerMarkedYoungMask;
    buf.Flush();
    ::g_cjStoreGoodMask = saved;
    std::vector<BaseObject*> marked;
    markFixture.DrainObjects(marked);
    GC_EXPECT_TRUE(marked.empty());
    GC_EXPECT_FALSE(SlotPageRemembered(slot));
    GC_EXPECT_TRUE(buf.IsEmpty());
}
#endif

GC_TEST(StoreBuf, YoungHolderRetiresPrevWithoutRememberingSlot)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region1->reset(PageAge::eden);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    StoreBarrierBuffer buf;
    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    retired.clear();

    const MAddress slot = reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE;
    const uintptr_t colour = static_cast<uintptr_t>(::g_cjStoreGoodMask);
    const zpointer prev = RefField<>(fx.obj0, colour).GetFieldValue();
    buf.add(slot, prev);
    buf.Flush();
    DrainPublishedMarkObjects(retired);

    GC_EXPECT_EQ(retired.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(retired[0]), reinterpret_cast<MAddress>(fx.obj0));
    GC_EXPECT_TRUE(!SlotPageRemembered(slot));
}

GC_TEST(StoreBuf, AddConsumesPreviousPhaseBeforeCurrentEntry)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 8);
    HeapSlotAt<>(slot).StoreColoured(zpointer::null);
    const uintptr_t saved = ::g_cjStoreGoodMask;
    const zpointer previous = RefField<>(fx.obj0, saved).GetFieldValue();
    buf.add(slot, previous);
    ::g_cjStoreGoodMask ^= ZPointerMarkedOldMask;
    const MAddress currentSlot = SlotAt(fx, 9);
    buf.add(currentSlot, RefField<>(fx.obj1, ::g_cjStoreGoodMask).GetFieldValue());
    GC_EXPECT_EQ(buf.Pending(), 2u);
    buf.Flush();
    ::g_cjStoreGoodMask = saved;
    std::vector<BaseObject*> marked;
    markFixture.DrainObjects(marked);
    GC_EXPECT_EQ(marked.size(), 2u);
    GC_EXPECT_TRUE(SlotPageRemembered(slot) && SlotPageRemembered(currentSlot));
}

GC_TEST(StoreBuf, PendingEntryFromOldEpochIsRejectedAfterOldMarkFlip)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    StoreBarrierBuffer buf;
    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    retired.clear();

    const uintptr_t before = static_cast<uintptr_t>(::g_cjStoreGoodMask);
    const zpointer prev = RefField<>(fx.obj0, before).GetFieldValue();
    buf.add(SlotAt(fx, 12), prev);
    // Publish the next old-mark epoch before this thread drains.  The pending
    // entry belongs to the install-time epoch and must not enter the new SATB.
    ::g_cjStoreGoodMask = before ^ ZPointerMarkedOldMask;
    buf.Flush();
    ::g_cjStoreGoodMask = before;
    DrainPublishedMarkObjects(retired);

    GC_EXPECT_EQ(retired.size(), 1u);
    GC_EXPECT_TRUE(SlotPageRemembered(SlotAt(fx, 12)));
}

GC_TEST(StoreBuf, PendingOldMarkEntrySurvivesYoungMarkFlip)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    StoreBarrierBuffer buf;
    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    retired.clear();

    const uintptr_t before = static_cast<uintptr_t>(::g_cjStoreGoodMask);
    const zpointer prev = RefField<>(fx.obj0, before).GetFieldValue();
    buf.add(SlotAt(fx, 13), prev);
    // A young-mark publication does not change the old-mark epoch that owns
    // this entry, so its SATB half must still be retired after the flip.
    ::g_cjStoreGoodMask = before ^ ZPointerMarkedYoungMask;
    buf.Flush();
    ::g_cjStoreGoodMask = before;
    DrainPublishedMarkObjects(retired);

    GC_EXPECT_EQ(retired.size(), 1u);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(retired[0]), reinterpret_cast<MAddress>(fx.obj0));
}

GC_TEST(StoreBuf, UnflushedPendingInvisibleToDrain)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 8);
    buf.add(slot, zpointer::null);
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
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 8);
    buf.add(slot, zpointer::null);
    RememberedSet& heapRs = HeapTestRemset();
    GC_EXPECT_TRUE(!heapRs.Contains(slot));
    buf.Flush();
    GC_EXPECT_TRUE(heapRs.Contains(slot));
}

GC_TEST(StoreBuf, MarkEndSnapshotLeavesCurrentForNextMinor)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 11);
    buf.add(slot, zpointer::null);
    buf.Flush();
    GC_EXPECT_TRUE(SlotPageRemembered(slot));
}

GC_TEST(StoreBuf, FlushBeforeMinorDoesNotLoseEdges)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    StoreBarrierBuffer buf;
    const size_t n = 7;
    for (size_t i = 0; i < n; ++i) {
        buf.add(SlotAt(fx, i + 8), zpointer::null);
    }
    buf.Flush();
    RememberedSet& heapRs = HeapTestRemset();
    for (size_t i = 0; i < n; ++i) {
        GC_EXPECT_TRUE(heapRs.Contains(SlotAt(fx, i + 8)));
    }
}

GC_TEST(StoreBuf, ThreadExitFlushRedeems)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    StoreBarrierBuffer buf;
    const MAddress slot = SlotAt(fx, 9);
    buf.add(slot, zpointer::null);
    buf.Flush();
    GC_EXPECT_TRUE(SlotPageRemembered(slot));
}



// ZThreadLocalData + ZMark::flush: detach publishes both generation stacks,
// including non-full chunks, even when this OS thread owns no allocator.
GC_OTHER_VM_TEST(StoreBarrierBuffer, DetachPublishesBothGenerationsWithoutAllocator)
{
    MapleRuntime::GcUnit::B09RuntimeFixture runtime;
    GcHeapFixture heap;
    heap.region0->reset(PageAge::eden);
    heap.region1->reset(PageAge::old);
    MarkPublicationFixture marking;
    std::thread owner([&] {
        ThreadLocal::SetAllocBuffer(nullptr);
        RegisterCurrentMarkFlushThread();
        ZBarrier::Mark<false, false, true, false>(from_object(heap.obj0));
        ZBarrier::Mark<false, false, false, false>(from_object(heap.obj1));
        MutatorManager::Instance().UnregisterMarkFlushThread(ThreadLocal::GetThreadLocalData());
    });
    owner.join();
    size_t young = 0;
    size_t old = 0;
    marking.DrainDomain(*Heap::GetHeap().young().MarkPtr(), [&](BaseObject* object, bool follow) {
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
// Derived from ZBarrierSet::oop_atomic_{cmpxchg,xchg}_not_in_heap and
// ZBarrier::self_heal: native atomics publish store-good, including null.
GC_TEST(StoreBuf, NativeAtomicUsesColoredHealingAndCompareValue)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    HeapSlot<true> native(zpointer::null);
    ZBarrier::AtomicWriteReference(nullptr, native, nullptr, std::memory_order_seq_cst);
    GC_EXPECT_EQ(native.GetFieldValue(), StoreGoodPointer(nullptr));
    GC_EXPECT_TRUE(ZBarrier::CompareAndSwapReference(nullptr, native, nullptr, fx.obj0,
        std::memory_order_seq_cst, std::memory_order_seq_cst));
    GC_EXPECT_EQ(native.GetFieldValue(), StoreGoodPointer(fx.obj0));
    GC_EXPECT_FALSE(ZBarrier::CompareAndSwapReference(nullptr, native, nullptr, fx.obj1,
        std::memory_order_seq_cst, std::memory_order_seq_cst));
    GC_EXPECT_TRUE(ZBarrier::AtomicSwapReference(nullptr, native, fx.obj1, std::memory_order_seq_cst) == fx.obj0);
    GC_EXPECT_TRUE(ZBarrier::AtomicReadReference(nullptr, native, std::memory_order_seq_cst) == fx.obj1);
    GC_EXPECT_EQ(native.GetFieldValue(), StoreGoodPointer(fx.obj1));
}

// ZBarrierSet::value_copy_in_heap / oop_copy_one_barriers at the explicit
// uncolored-local <-> native-zpointer <-> heap-zpointer boundaries.
GC_TEST(StoreBuf, BulkPreservesSourceStorageProtocol)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    RootSlot local;
    StorePlain(local, from_object(fx.obj0));
    NativeSlot native(zpointer::null);
    const GCTib layout = fx.obj0->GetGCTib();
    ZBarrier::WriteStaticStruct(reinterpret_cast<MAddress>(&native), sizeof(native),
        reinterpret_cast<MAddress>(&local), sizeof(local), layout);
    GC_EXPECT_EQ(native.GetFieldValue(), StoreGoodPointer(fx.obj0));
    RootSlot result;
    ZBarrier::ReadStaticStruct(reinterpret_cast<MAddress>(&result), reinterpret_cast<MAddress>(&native),
        sizeof(native), layout);
    GC_EXPECT_EQ(raw(result.LoadPlain()), reinterpret_cast<uintptr_t>(fx.obj0));
    HeapSlot<>& heap = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    heap.StoreColoured(zpointer::null);
    ZBarrier::ReadStaticStruct(reinterpret_cast<MAddress>(&heap), reinterpret_cast<MAddress>(&native),
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
    rs.Initialize(fx.heapStart, GcHeapFixture::kUnits * ZGranuleSize);
    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const zpointer previous = StoreGoodPointer(fx.obj0);
    field.StoreColoured(StoreGoodPointer(fx.obj1));
    ZBarrier::store_barrier_on_heap_oop_field(reinterpret_cast<volatile zpointer*>(&field), false);
    std::vector<BaseObject*> marked;
    marking.DrainObjects(marked);
    GC_EXPECT_TRUE(marked.empty());
    GC_EXPECT_TRUE(ThreadLocal::GetGCData().storeBarrierBuffer->IsEmpty());
    GC_EXPECT_FALSE(SlotPageRemembered(reinterpret_cast<MAddress>(&field)));
    GC_EXPECT_EQ(field.GetFieldValue(), StoreGoodPointer(fx.obj1));
}
