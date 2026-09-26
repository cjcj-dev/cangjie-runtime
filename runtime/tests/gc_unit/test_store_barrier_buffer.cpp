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


#include "gc_generation_test.hpp"
#include "ObjectModel/FieldInfo.h"

#include "Heap/z/zAccess.hpp"

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
    explicit AllocBufferScope(AllocBuffer& alloc) : alloc(alloc), saved(ThreadLocal::GetThreadLocalData()->buffer)
    {
        ThreadLocal::GetThreadLocalData()->buffer = &alloc;
    }

    ~AllocBufferScope()
    {
        ThreadLocal::GetThreadLocalData()->buffer = saved;
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

    const StoreBarrierEntry& entry = buf.buffer[buf.Current()];
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(entry.p), slot);
    GC_EXPECT_EQ(raw(entry.prev), raw(prev));
    GC_EXPECT_EQ(buf.Pending(), 1u);
}

GC_TEST(StoreBuf, ProductWriteCarriesOldValueOnlyInPrevArm)
{
    GcHeapFixture fx;
    fx.region0()->reset(PageAge::old);
    fx.region1()->reset(PageAge::eden);
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
    const ZGenerationPhase phaseBefore = Heap::GetHeap().GetZGeneration(ZGenerationId::old).GcPhase();
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Mark);

    Mutator mutator;
    InstalledMutatorScope mutatorScope(mutator);
    HeapAccess<>::oop_store(&(field), fx.obj1);

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
    const StoreBarrierEntry& entry = buf.buffer[buf.Current()];
    const bool pairMatches = raw(entry.prev) == raw(prev) && raw(entry.prev) != reinterpret_cast<MAddress>(entry.p);
    GC_EXPECT_EQ(reinterpret_cast<MAddress>(entry.p), reinterpret_cast<MAddress>(&field));
    GC_EXPECT_EQ(raw(entry.prev), raw(prev));
    GC_EXPECT_NE(raw(entry.prev), reinterpret_cast<MAddress>(entry.p));
    if (!pairMatches) {
        buf.buffer[buf.Current()] = {};
        buf.current = StoreBarrierBuffer::BufferSizeBytes;
        return;
    }
    buf.Flush();
    DrainPublishedMarkObjects(retired);
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(phaseBefore);
    GC_EXPECT_EQ(retired.size(), 1u);
}

// The real mutator store and flush publish prev into the young mark domain.
// The incoming value is distinct, so marking it cannot satisfy this assertion.
GC_TEST(StoreBuf, ProductWriteFlushPublishesPreviousYoungValue)
{
    GcHeapFixture fx;
    fx.region0()->reset(PageAge::old);
    fx.region1()->reset(PageAge::eden);
    MarkPublicationFixture markFixture;
    BaseObject* incoming = fx.PlaceObject(fx.heapStart + ZGranuleSize + 128);
    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreBadPointer(fx.obj1));
    Mutator mutator;
    InstalledMutatorScope mutatorScope(mutator);
    HeapAccess<>::oop_store(&(field), incoming);
    const size_t buffered = mutator.GetGCData().storeBarrierBuffer->Pending();
    const size_t youngBefore = markFixture.YoungPending();
    mutator.FlushStoreBarrierBuffer();
    const size_t youngAfter = markFixture.YoungPending();
    const size_t oldAfter = markFixture.OldPending();
    const bool remembered = SlotPageRemembered(reinterpret_cast<MAddress>(&field));
    std::vector<BaseObject*> work;
    markFixture.DrainObjects(work);
    size_t previousCount = 0;
    size_t incomingCount = 0;
    for (BaseObject* object : work) {
        previousCount += object == fx.obj1;
        incomingCount += object == incoming;
    }
    std::fprintf(stderr,
                 "DETAIL young_prev buffered=%zu young_before=%zu young_after=%zu old_after=%zu "
                 "previous=%zu incoming=%zu remset=%u\n",
                 buffered, youngBefore, youngAfter, oldAfter, previousCount, incomingCount,
                 static_cast<unsigned>(remembered));
    // Assert the consumed result before any producer-presence check can mask it.
    GC_EXPECT_EQ(youngAfter, 1u);
    GC_EXPECT_EQ(previousCount, 1u);
    GC_EXPECT_EQ(incomingCount, 0u);
    GC_EXPECT_TRUE(remembered);
    GC_EXPECT_EQ(buffered, 1u);
    GC_EXPECT_EQ(youngBefore, 0u);
    GC_EXPECT_EQ(oldAfter, 0u);
    GC_EXPECT_TRUE(to_object(field.GetTargetObject()) == incoming);
}

// ZGC zMark.inline.hpp:51-55: objects on pages allocated in this cycle are
// implicitly live. Contrast with ProductWriteCarriesOldValueOnlyInPrevArm.
GC_TEST(StoreBuf, AllocatingPreviousValueIsImplicitlyLive)
{
    for (PageAge age : {PageAge::old, PageAge::eden}) {
        GcHeapFixture fx;
        fx.region0()->reset(PageAge::old);
        MarkPublicationFixture markFixture;
        // Deliberately allocate the previous value's page after mark start.
        fx.region1()->reset(age);
        HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
        field.StoreColoured(StoreBadPointer(fx.obj1));
        Mutator mutator;
        InstalledMutatorScope mutatorScope(mutator);
        HeapAccess<>::oop_store(&(field), nullptr);
        const size_t pending = mutator.GetGCData().storeBarrierBuffer->Pending();
        mutator.FlushStoreBarrierBuffer();
        std::vector<BaseObject*> retired;
        DrainPublishedMarkObjects(retired);
        const bool remembered = SlotPageRemembered(reinterpret_cast<MAddress>(&field));
        std::fprintf(stderr, "DETAIL allocating age=%u allocating=%u pending=%zu retired=%zu remset=%u\n",
                     static_cast<unsigned>(age), static_cast<unsigned>(fx.region1()->IsAllocating()),
                     pending, retired.size(), static_cast<unsigned>(remembered));
        GC_EXPECT_EQ(pending, 1u);
        GC_EXPECT_TRUE(fx.region1()->IsAllocating());
        GC_EXPECT_TRUE(fx.region1()->is_object_strongly_live(from_object(fx.obj1)));
        GC_EXPECT_TRUE(retired.empty());
        GC_EXPECT_TRUE(remembered);
        GC_EXPECT_TRUE(is_null(field.GetTargetObject()));
    }
}

GC_TEST(StoreBuf, ProductPhaseFlushHandsPairedPrevToMark)
{
    GcHeapFixture fx;
    fx.region0()->reset(PageAge::old);
    fx.region1()->reset(PageAge::eden);
    // SATB previous values must predate mark start (ZGC zMark.inline.hpp:51-55).
    MarkPublicationFixture markFixture;

    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);

    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreBadPointer(fx.obj0));

    Heap& heap = Heap::GetHeap();
    const ZGenerationPhase phaseBefore = Heap::GetHeap().GetZGeneration(ZGenerationId::old).GcPhase();
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Mark);

    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    retired.clear();
    Mutator mutator;
    ThreadLocal::GetThreadLocalData()->buffer = &alloc;
#if defined(MRT_TESTABLE_INTERNALS)

#endif
    InstalledMutatorScope mutatorScope(mutator);
    HeapAccess<>::oop_store(&(field), fx.obj1);
    GC_EXPECT_EQ(ThreadLocal::GetGCData().storeBarrierBuffer->Pending(), 1u);
    mutator.FlushStoreBarrierBuffer();
    GC_EXPECT_TRUE(ThreadLocal::GetGCData().storeBarrierBuffer->IsEmpty());
    DrainPublishedMarkObjects(retired);
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(phaseBefore);
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
    fx.region0()->reset(PageAge::old);
    fx.region1()->reset(PageAge::eden);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);
    Mutator mutator;
    InstalledMutatorScope mutatorScope(mutator);
    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const zpointer prev = StoreBadPointer(fx.obj0);
    field.StoreColoured(prev);

    HeapAccess<>::oop_store(&(field), fx.obj1);

    StoreBarrierBuffer& buf = *ThreadLocal::GetGCData().storeBarrierBuffer;
    GC_EXPECT_EQ(buf.Pending(), 1u);
    if (buf.Pending() == 1u) {
        GC_EXPECT_EQ(reinterpret_cast<MAddress>(buf.buffer[buf.Current()].p), reinterpret_cast<MAddress>(&field));
        GC_EXPECT_EQ(raw(buf.buffer[buf.Current()].prev), raw(prev));
    }
    std::fprintf(stderr, "TARGET_HOLDER_MARK_AND_REMEMBER_EXECUTED\n");
}

// ZGC: holder identity is not a store-buffer key; heap field p is buffered.
GC_TEST(StoreBuf, ProductNonHeapHolderBypassesPendingRelocationEntry)
{
    GcHeapFixture fx;
    MarkPublicationFixture markFixture;
    fx.region0()->reset(PageAge::old);
    fx.region1()->reset(PageAge::eden);
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

    HeapAccess<>::oop_store(&(field), fx.obj1);

    StoreBarrierBuffer& buf = *ThreadLocal::GetGCData().storeBarrierBuffer;
    GC_EXPECT_EQ(buf.Pending(), 1u);
    if (buf.Pending() == 1u) {
        GC_EXPECT_EQ(reinterpret_cast<MAddress>(buf.buffer[buf.Current()].p), reinterpret_cast<MAddress>(&field));
        GC_EXPECT_EQ(raw(buf.buffer[buf.Current()].prev), raw(prev));
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
    fx.region0()->reset(PageAge::old);
    fx.region1()->reset(PageAge::eden);
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
    const ZGenerationPhase phaseBefore = Heap::GetHeap().GetZGeneration(ZGenerationId::old).GcPhase();
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Mark);

    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    retired.clear();
    Mutator mutator;
    ThreadLocal::GetThreadLocalData()->buffer = &alloc;
#if defined(MRT_TESTABLE_INTERNALS)

#endif
    InstalledMutatorScope mutatorScope(mutator);

    // ZGC store_at_resolved: store barrier sees the previous colored word, then the store.
    ZBarrier::store_barrier_on_heap_oop_field(reinterpret_cast<volatile zpointer*>(&field), false); // oldvalue-anchor
    field.StoreColoured(newWord);
    const size_t pending = ThreadLocal::GetGCData().storeBarrierBuffer->Pending();
    mutator.FlushStoreBarrierBuffer();
    DrainPublishedMarkObjects(retired);
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(phaseBefore);
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
    fx.region0()->reset(PageAge::old);
    fx.region1()->reset(PageAge::eden);

    RememberedSet rs;
    rs.Initialize(fx.heapStart, 2 * ZGranuleSize);
    AllocBuffer alloc;
    AllocBufferScope allocScope(alloc);

    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreBadPointer(fx.obj0));

    Heap& heap = Heap::GetHeap();
    const ZGenerationPhase phaseBefore = Heap::GetHeap().GetZGeneration(ZGenerationId::old).GcPhase();
    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Mark);

    Mutator mutator;
    ThreadLocal::GetThreadLocalData()->buffer = &alloc;
#if defined(MRT_TESTABLE_INTERNALS)

#endif
    InstalledMutatorScope mutatorScope(mutator);
    HeapAccess<>::oop_store(&(field), fx.obj1);
    GC_EXPECT_EQ(ThreadLocal::GetGCData().storeBarrierBuffer->Pending(), 1u);

    // A GC worker assisting a saferegion transition must not consume the
    // paired store entry: ZGC on_new_phase runs in the Java-thread flush.
    mutator.FlushStoreBarrierBuffer(false);
    GC_EXPECT_EQ(ThreadLocal::GetGCData().storeBarrierBuffer->Pending(), 1u);
    // The mutator-side transition (or the next explicit safepoint) consumes it.
    mutator.FlushStoreBarrierBuffer(true);
    GC_EXPECT_TRUE(ThreadLocal::GetGCData().storeBarrierBuffer->IsEmpty());

    Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(phaseBefore);
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
    fx.region0()->reset(PageAge::eden);
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
    fx.region1()->reset(PageAge::eden);
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
    heap.region0()->reset(PageAge::eden);
    heap.region1()->reset(PageAge::old);
    MarkPublicationFixture marking;
    std::thread owner([&] {
        ThreadLocal::GetThreadLocalData()->buffer = nullptr;
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
    NativeAccess<MO_SEQ_CST>::oop_store(&(native), nullptr);
    GC_EXPECT_EQ(native.GetFieldValue(), StoreGoodPointer(nullptr));
    GC_EXPECT_TRUE((NativeAccess<MO_SEQ_CST>::oop_atomic_cmpxchg(&(native), nullptr, fx.obj0) == nullptr));
    GC_EXPECT_EQ(native.GetFieldValue(), StoreGoodPointer(fx.obj0));
    GC_EXPECT_FALSE((NativeAccess<MO_SEQ_CST>::oop_atomic_cmpxchg(&(native), nullptr, fx.obj1) == nullptr));
    GC_EXPECT_TRUE(NativeAccess<MO_SEQ_CST>::oop_atomic_xchg(&(native), fx.obj1) == fx.obj0);
    GC_EXPECT_TRUE(NativeAccess<MO_SEQ_CST>::oop_load(&(native)) == fx.obj1);
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
    NativeAccess<>::value_copy(
        ValuePayload(reinterpret_cast<MAddress>(&local), sizeof(local), layout, ValuePayload::Kind::Uncolored),
        ValuePayload(reinterpret_cast<MAddress>(&native), sizeof(native), ValuePayload::Kind::Native));
    GC_EXPECT_EQ(native.GetFieldValue(), StoreGoodPointer(fx.obj0));
    RootSlot result;
    NativeAccess<>::value_copy(
        ValuePayload(reinterpret_cast<MAddress>(&native), sizeof(native), layout, ValuePayload::Kind::Native),
        ValuePayload(reinterpret_cast<MAddress>(&result), sizeof(native), ValuePayload::Kind::Uncolored));
    GC_EXPECT_EQ(raw(result.LoadPlain()), reinterpret_cast<uintptr_t>(fx.obj0));
    HeapSlot<>& heap = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    heap.StoreColoured(zpointer::null);
    NativeAccess<>::value_copy(
        ValuePayload(reinterpret_cast<MAddress>(&native), sizeof(native), layout, ValuePayload::Kind::Native),
        ValuePayload(reinterpret_cast<MAddress>(&heap), sizeof(native), ValuePayload::Kind::Heap));
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
    uintptr_t color = ZPointerStoreGoodMask;
    auto* previous = MRT_BindUncoloredVisitColor(&color);
    MRT_VisitorCaller(&data, &visitor);
    MRT_BindUncoloredVisitColor(previous);
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

extern "C" void CJ_MCC_WriteRefField(ObjectPtr, ObjectPtr, RefField<false>*);
extern "C" void CJ_MCC_WriteRefField_Strong(ObjectPtr, ObjectPtr, RefField<false>*);
extern "C" void CJ_MCC_WriteRefField_Weak(ObjectPtr, ObjectPtr, RefField<false>*);

extern "C" void MCC_SetInstanceFieldValue(InstanceFieldInfo*, TypeInfo*, ObjRef, ObjRef);

namespace {
using StoreEntry = void (*)(ObjectPtr, ObjectPtr, RefField<false>*);
void CheckStoreAccessor(StoreEntry entry, bool weak, bool weakHolder, bool reflection = false)
{
    GcHeapFixture fx;
    fx.region0()->reset(PageAge::old);
    fx.region1()->reset(PageAge::eden);
    MarkPublicationFixture marking;
    if (weakHolder) {
        fx.typeInfo->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
    }
    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreBadPointer(fx.obj0));
    Mutator mutator;
    InstalledMutatorScope mutatorScope(mutator);
    // Flush the product buffer through its normal phase-change consumer.
    // A weak entry must remember without publishing previous-value mark work.
    if (reflection) {
        TypeInfo* fieldTypes[] = {fx.typeInfo};
        U32 offsets[] = {0};
        fx.typeInfo->SetFieldNum(1);
        fx.typeInfo->SetFieldAddr(fieldTypes);
        fx.typeInfo->SetOffsets(offsets);
        InstanceFieldInfo fieldInfo{};
        MCC_SetInstanceFieldValue(&fieldInfo, fx.typeInfo,
            reinterpret_cast<MObject*>(fx.obj0), reinterpret_cast<MObject*>(fx.obj1));
    } else {
        entry(fx.obj1, fx.obj0, &field);
    }
    mutator.FlushStoreBarrierBuffer(true);
    const bool remembered = Heap::page(reinterpret_cast<MAddress>(&field))->is_remembered(
        reinterpret_cast<volatile zpointer*>(&field));
    std::vector<BaseObject*> marked;
    marking.DrainObjects(marked);
    const bool keptAlive = std::find(marked.begin(), marked.end(), fx.obj0) != marked.end();
    const bool installed = to_object(field.GetTargetObject()) == fx.obj1;
    std::fprintf(stderr, "TARGET_STORE_ACCESSOR weak=%d remembered=%d kept_alive=%d installed=%d\n",
                 weak, remembered, keptAlive, installed);
    // One target invariant: preliminary existence assertions cannot hide it.
    GC_EXPECT_TRUE(remembered && keptAlive == !weak && installed);
}
}

GC_TEST(StoreAccess843, StaticWeakRemembersWithoutKeepingAlive)
{
    CheckStoreAccessor(CJ_MCC_WriteRefField_Weak, true, false);
}
GC_TEST(StoreAccess843, StaticStrongKeepsPreviousAlive)
{
    CheckStoreAccessor(CJ_MCC_WriteRefField_Strong, false, true);
}
GC_TEST(StoreAccess843, UnknownWeakResolvesBeforeBarrier)
{
    CheckStoreAccessor(CJ_MCC_WriteRefField, true, true);
}
GC_TEST(StoreAccess843, UnknownStrongResolvesBeforeBarrier)
{
    CheckStoreAccessor(CJ_MCC_WriteRefField, false, false);
}

GC_TEST(StoreAccess843, ReflectionWeakRemembersWithoutKeepingAlive)
{
    CheckStoreAccessor(nullptr, true, true, true);
}
GC_TEST(StoreAccess843, ReflectionStrongKeepsPreviousAlive)
{
    CheckStoreAccessor(nullptr, false, false, true);
}

#include "Cangjie.h"
#include "ObjectModel/MObject.h"
namespace MapleRuntime {
extern "C" ObjRef MCC_NewObject(const TypeInfo*, MSize);
extern "C" void CJ_MCC_StoreBarrierOnHeapField(volatile zpointer*);
extern "C" void CJ_MCC_StoreBarrierOnHeapFieldNoKeepAlive(volatile zpointer*);
extern "C" const uintptr_t g_cjStoreBarrierBufferOffset;
extern "C" const uintptr_t g_cjStoreBarrierBufferCurrentOffset;
extern "C" const uintptr_t g_cjStoreBarrierBufferBufferOffset;
extern "C" const uintptr_t g_cjStoreBarrierEntrySize;
extern "C" const uintptr_t g_cjStoreBarrierEntryPOffset;
extern "C" const uintptr_t g_cjStoreBarrierEntryPrevOffset;
}
namespace {
struct ByteBufferResult {
    size_t appended = 0;
    size_t full = SIZE_MAX;
    size_t afterFlush = SIZE_MAX;
    bool pairs = true;
    bool unchanged = true;
    bool offsets = false;
};
void* FillByteBuffer(void* context)
{
    auto& result = *static_cast<ByteBufferResult*>(context);
    Mutator::GetMutator()->SetManagedContext(false);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(sizeof(uintptr_t));
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    auto* object = static_cast<BaseObject*>(MCC_NewObject(type, TYPEINFO_PTR_SIZE + sizeof(uintptr_t)));
    auto* slot = reinterpret_cast<volatile zpointer*>(reinterpret_cast<uintptr_t>(object) + TYPEINFO_PTR_SIZE);
    auto& data = ThreadLocal::GetGCData();
    auto& buffer = *data.storeBarrierBuffer;
    result.offsets = ThreadGCDataABI::GCDataPointer == offsetof(ThreadLocalData, gcData) &&
        ThreadGCDataABI::LoadBadMask == offsetof(ThreadGCData, loadBadMask) &&
        ThreadGCDataABI::StoreBadMask == offsetof(ThreadGCData, storeBadMask) &&
        ThreadGCDataABI::StoreGoodMask == offsetof(ThreadGCData, storeGoodMask) &&
        g_cjStoreBarrierBufferOffset == offsetof(ThreadGCData, storeBarrierBuffer) &&
        g_cjStoreBarrierBufferCurrentOffset == offsetof(StoreBarrierBuffer, current) &&
        g_cjStoreBarrierBufferBufferOffset == offsetof(StoreBarrierBuffer, buffer) &&
        g_cjStoreBarrierEntrySize == sizeof(StoreBarrierEntry) &&
        g_cjStoreBarrierEntryPOffset == offsetof(StoreBarrierEntry, p) &&
        g_cjStoreBarrierEntryPrevOffset == offsetof(StoreBarrierEntry, prev);
    const zpointer previous = StoreBadPointer(object);
    for (size_t i = 0; i <= StoreBarrierBuffer::Capacity(); ++i) {
        *slot = previous;
        CJ_MCC_StoreBarrierOnHeapField(slot);
        const size_t expected = StoreBarrierBuffer::BufferSizeBytes -
            ((i % StoreBarrierBuffer::Capacity()) + 1) * sizeof(StoreBarrierEntry);
        result.unchanged &= raw(*slot) == raw(previous);
        // Stop at a malformed cursor so the target invariant reports the first
        // incorrect result before a subsequent append can address outside it.
        if (buffer.current != expected) { result.pairs = false; return nullptr; }
        const auto& entry = buffer.buffer[buffer.Current()];
        result.pairs &= reinterpret_cast<MAddress>(entry.p) == reinterpret_cast<MAddress>(slot) && raw(entry.prev) == raw(previous);
        ++result.appended;
        if (i + 1 == StoreBarrierBuffer::Capacity()) result.full = buffer.current;
    }
    result.afterFlush = buffer.current;
    CJ_MCC_StoreBarrierOnHeapFieldNoKeepAlive(slot);
    result.unchanged &= raw(*slot) == raw(previous);
    // A store-good control must not append or change the slot.
    *slot = StoreGoodPointer(object);
    CJ_MCC_StoreBarrierOnHeapField(slot);
    result.unchanged &= raw(*slot) == raw(StoreGoodPointer(object)) && buffer.current == result.afterFlush;
    return nullptr;
}
}
GC_RUNTIME_OTHER_VM_TEST(StoreBuffer856, AllocatedBarrierOnlyFillsByteCursor)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ByteBufferResult result;
    auto task = RunCJTask(FillByteBuffer, &result);
    void* value = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &value), E_OK);
    ReleaseHandle(task);
    std::fprintf(stderr, "BYTE_BUFFER_TARGET appended=%zu full=%zu after_flush=%zu pairs=%d unchanged=%d offsets=%d\n",
        result.appended, result.full, result.afterFlush, result.pairs, result.unchanged, result.offsets);
    GC_EXPECT_TRUE(result.appended == StoreBarrierBuffer::Capacity() + 1 && result.full == 0 &&
        result.afterFlush == StoreBarrierBuffer::BufferSizeBytes - sizeof(StoreBarrierEntry) &&
        result.pairs && result.unchanged && result.offsets);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

namespace {
void CheckBarrierOnly856(bool weak)
{
    GcHeapFixture fx;
    fx.region0()->reset(PageAge::old);
    fx.region1()->reset(PageAge::eden);
    MarkPublicationFixture marking;
    HeapSlot<>& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const zpointer previous = StoreBadPointer(fx.obj0);
    field.StoreColoured(previous);
    Mutator mutator;
    InstalledMutatorScope scope(mutator);
    auto* slot = reinterpret_cast<volatile zpointer*>(&field);
    if (weak) CJ_MCC_StoreBarrierOnHeapFieldNoKeepAlive(slot);
    else CJ_MCC_StoreBarrierOnHeapField(slot);
    mutator.FlushStoreBarrierBuffer(true);
    const bool remembered = Heap::page(reinterpret_cast<MAddress>(slot))->is_remembered(slot);
    std::vector<BaseObject*> marked;
    marking.DrainObjects(marked);
    const bool kept = std::find(marked.begin(), marked.end(), fx.obj0) != marked.end();
    const bool unchanged = raw(field.GetFieldValue()) == raw(previous);
    std::fprintf(stderr, "BARRIER_ONLY_TARGET weak=%d remembered=%d kept=%d unchanged=%d\n",
        weak, remembered, kept, unchanged);
    GC_EXPECT_TRUE(remembered && kept == !weak && unchanged);
}
}
GC_TEST(StoreBuffer856, StrongOnlyPublishesPrevious) { CheckBarrierOnly856(false); }
GC_TEST(StoreBuffer856, WeakOnlyRemembersPreviousSlot) { CheckBarrierOnly856(true); }

// ZGC zBarrierSet.cpp:253-273. Observe state produced by real lifecycle
// entries in the runtime SO; no test copy of Mutator::Init or detach hooks.
namespace {
bool InitialThreadMasks(const ThreadGCData& data, const ThreadGCData::Masks& masks)
{
    return data.loadGoodMask == masks.loadGood && data.loadBadMask == masks.loadBad &&
        data.markBadMask == masks.markBad && data.storeGoodMask == masks.storeGood &&
        data.storeBadMask == masks.storeBad;
}
}

GC_OTHER_VM_TEST(ThreadLifecycle, NativeAttachPublishesInitialState)
{
    GcHeapFixture heap;
    const auto masks = ThreadGCData::PublishedMasks();
    bool initialized = false;
    std::thread owner([&] {
        ThreadLocal::InitializeCleaner();
        const auto& data = ThreadLocal::GetGCData();
        initialized = InitialThreadMasks(data, masks) &&
            data.storeBarrierBuffer->lastProcessedColor == masks.storeGood &&
            data.storeBarrierBuffer->lastInstalledColor == masks.storeGood;
    });
    owner.join();
    std::fprintf(stderr, "THREAD_ATTACH_NATIVE_TARGET executed=1 initialized=%d\n", initialized);
    GC_EXPECT_TRUE(initialized);
}

GC_OTHER_VM_TEST(ThreadLifecycle, ManagedAttachAndRebindPreserveState)
{
    B09RuntimeFixture runtime;
    GcHeapFixture heap;
    const auto masks = ThreadGCData::PublishedMasks();
    bool initialized = false;
    bool preserved = false;
    std::thread thread([&] {
        auto& manager = MutatorManager::Instance();
        auto* owner = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        auto& data = owner->GetGCData();
        initialized = InitialThreadMasks(data, masks) &&
            data.storeBarrierBuffer->lastProcessedColor == masks.storeGood &&
            // ZGC stackWatermark.cpp:162-163 initializes current epoch + done.
            owner->GetStackWatermark().IsDone(static_cast<uint32_t>(masks.storeGood));
        StackWatermarkSet::on_safepoint(*owner);
        const auto watermark = owner->GetStackWatermark().PackedState();
        const auto color = data.storeGoodMask;
        manager.UnbindMutator(*owner);
        manager.BindMutator(*owner);
        preserved = owner->GetStackWatermark().PackedState() == watermark && data.storeGoodMask == color;
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    });
    thread.join();
    std::fprintf(stderr, "THREAD_ATTACH_MANAGED_TARGET executed=1 initialized=%d rebind_preserved=%d\n",
                 initialized, preserved);
    GC_EXPECT_TRUE(initialized);
    GC_EXPECT_TRUE(preserved);
}

namespace {
void CheckThreadDetachMarksObjects(bool managed)
{
    B09RuntimeFixture runtime;
    GcHeapFixture heap;
    heap.region0()->reset(PageAge::eden);
    heap.region1()->reset(PageAge::old);
    MarkPublicationFixture marking;
    size_t privateYoung = 0;
    size_t privateOld = 0;
    std::thread thread([&] {
        auto& manager = MutatorManager::Instance();
        Mutator* owner = nullptr;
        if (managed) {
            owner = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
            // Complete the last-transition watermark before producing the
            // private work, so a root scan cannot mask a broken detach.
            StackWatermarkSet::on_safepoint(*owner);
        } else {
            ThreadLocal::InitializeCleaner();
        }
        ZBarrier::Mark<false, false, false, false>(from_object(heap.obj0));
        ZBarrier::Mark<false, false, false, false>(from_object(heap.obj1));
        auto& data = ThreadLocal::GetGCData();
        privateYoung = data.markStacks[0].Population();
        privateOld = data.markStacks[1].Population();
        if (managed) {
            manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        }
        // Native detach is the actual TLS destructor after this return.
    });
    thread.join();
    auto& young = Heap::GetHeap().young().Mark();
    auto& old = Heap::GetHeap().old().Mark();
    young.MarkFollow();
    old.MarkFollow();
    const bool markedYoung = heap.region0()->is_object_marked(from_object(heap.obj0), false);
    const bool markedOld = heap.region1()->is_object_marked(from_object(heap.obj1), false);
    const bool ended = young.TryEnd() && old.TryEnd();
    std::fprintf(stderr,
        "THREAD_DETACH_TARGET executed=1 managed=%d private_young=%zu private_old=%zu marked_young=%d marked_old=%d ended=%d\n",
        managed, privateYoung, privateOld, markedYoung, markedOld, ended);
    // The target is evaluated before setup checks: a missing publication must
    // fail here, not at an earlier existence assertion.
    GC_EXPECT_TRUE(markedYoung && markedOld && ended);
    GC_EXPECT_EQ(privateYoung, 1u);
    GC_EXPECT_EQ(privateOld, 1u);
}
}

GC_OTHER_VM_TEST(ThreadLifecycle, NativeDetachMarksBothGenerations)
{
    CheckThreadDetachMarksObjects(false);
}

GC_OTHER_VM_TEST(ThreadLifecycle, ManagedDetachMarksBothGenerations)
{
    CheckThreadDetachMarksObjects(true);
}

namespace {
// The real native attach/detach entry owns the Mutator and its GC data. An
// unbound logical owner models the same identity transition used by CJThread
// scheduling; the handshake must retain it in the SMR target snapshot.
void CheckLogicalMarkHandshake(bool unbound, bool selfProcess)
{
    B09RuntimeFixture runtime;
    GcHeapFixture heap;
    heap.region0()->reset(PageAge::eden);
    heap.region1()->reset(PageAge::old);
    MarkPublicationFixture marking;
    std::atomic<bool> ready{false}, release{false};
    size_t privateYoung = 0, privateOld = 0;
    std::thread producer([&] {
        auto& manager = MutatorManager::Instance();
        auto* owner = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        StackWatermarkSet::on_safepoint(*owner);
        if (selfProcess) { owner->DoLeaveSaferegion(); }
        ZBarrier::Mark<false, false, false, false>(from_object(heap.obj0));
        ZBarrier::Mark<false, false, false, false>(from_object(heap.obj1));
        privateYoung = owner->GetGCData().markStacks[0].Population();
        privateOld = owner->GetGCData().markStacks[1].Population();
        if (unbound) { manager.UnbindMutator(*owner); }
        ready.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            if (selfProcess) { owner->GetHandshakeState().process_by_self(); }
            std::this_thread::yield();
        }
        if (unbound) { manager.BindMutator(*owner); }
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    });
    while (!ready.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    // These product worker tasks call proactive/termination handshakes and
    // consume the resulting stacks. Read mark bits before producer detach,
    // whose final publication must not mask a missing handshake target.
    auto& young = Heap::GetHeap().young().Mark();
    auto& old = Heap::GetHeap().old().Mark();
    young.MarkFollow();
    old.MarkFollow();
    const bool markedYoung = heap.region0()->is_object_marked(from_object(heap.obj0), false);
    const bool markedOld = heap.region1()->is_object_marked(from_object(heap.obj1), false);
    release.store(true, std::memory_order_release);
    producer.join();
    std::fprintf(stderr,
        "LOGICAL_HANDSHAKE_TARGET executed=1 unbound=%d self=%d private_young=%zu private_old=%zu marked_young=%d marked_old=%d\n",
        unbound, selfProcess, privateYoung, privateOld, markedYoung, markedOld);
    GC_EXPECT_TRUE(markedYoung && markedOld);
    GC_EXPECT_EQ(privateYoung, 1u);
    GC_EXPECT_EQ(privateOld, 1u);
}
}
GC_OTHER_VM_TEST(LogicalMarkHandshake1145, UnboundOwnerPublishesBothGenerations)
{
    CheckLogicalMarkHandshake(true, false);
}
GC_OTHER_VM_TEST(LogicalMarkHandshake1145, SafeOwnerPublishesBothGenerations)
{
    CheckLogicalMarkHandshake(false, false);
}
GC_OTHER_VM_TEST(LogicalMarkHandshake1145, RunningOwnerPublishesBothGenerations)
{
    CheckLogicalMarkHandshake(false, true);
}

namespace {
class RetainedSyncHandshake1145 final : public HandshakeClosure {
public:
    RetainedSyncHandshake1145() : HandshakeClosure("RetainedSyncHandshake1145") {}
    void do_thread(Mutator* thread) override
    {
        if (thread != target) { return; }
        // The product invokes this closure while holding the queue lock.
        // Inspect its existing accessor here; operation_pending would relock.
        HandshakeOperation* op = thread->GetHandshakeState().get_op();
        retained = op != nullptr && op->closure() == this && !op->is_completed();
        executed = true;
    }
    Mutator* target = nullptr;
    bool retained = false;
    bool executed = false;
};

void CheckSynchronousHandshakeQueue(bool self, bool broadcast)
{
    B09RuntimeFixture runtime;
    RetainedSyncHandshake1145 closure;
    std::atomic<bool> ready{false}, release{false};
    bool removed = false;
    auto execute = [&] {
        if (broadcast) { Handshake::execute(&closure); }
        else { Handshake::execute(&closure, closure.target); }
    };
    std::thread owner([&] {
        auto& manager = MutatorManager::Instance();
        closure.target = manager.CreateRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
        if (self) {
            execute();
            removed = !closure.target->GetHandshakeState().has_operation();
        } else {
            ready.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        }
        manager.DestroyRuntimeMutator(ThreadType::UNCOMMITTER_THREAD);
    });
    if (!self) {
        while (!ready.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        execute();
        removed = !closure.target->GetHandshakeState().has_operation();
        release.store(true, std::memory_order_release);
    }
    owner.join();
    std::fprintf(stderr,
        "SYNC_HANDSHAKE_QUEUE_TARGET executed=%d self=%d broadcast=%d retained=%d removed=%d\n",
        closure.executed, self, broadcast, closure.retained, removed);
    GC_EXPECT_TRUE(closure.executed && closure.retained && removed);
}
}

GC_OTHER_VM_TEST(LogicalMarkHandshake1145, SelfTargetRetainsOperationDuringClosure)
{
    CheckSynchronousHandshakeQueue(true, false);
}
GC_OTHER_VM_TEST(LogicalMarkHandshake1145, SelfBroadcastRetainsOperationDuringClosure)
{
    CheckSynchronousHandshakeQueue(true, true);
}
GC_OTHER_VM_TEST(LogicalMarkHandshake1145, SafeTargetRetainsOperationDuringClosure)
{
    CheckSynchronousHandshakeQueue(false, false);
}
GC_OTHER_VM_TEST(LogicalMarkHandshake1145, SafeBroadcastRetainsOperationDuringClosure)
{
    CheckSynchronousHandshakeQueue(false, true);
}

// #976: exported compiler ABI -> AccessBarrier store -> real TLS buffer.
extern "C" void CJ_MCC_AtomicWriteReference(BaseObject*, BaseObject*, HeapSlot<true>*, MemoryOrder);
extern "C" BaseObject* CJ_MCC_AtomicSwapReference(BaseObject*, BaseObject*, HeapSlot<true>*, MemoryOrder);

GC_TEST(AccessBarrier976, AtomicReleaseStoreBuffersWithoutHealing)
{
    GcHeapFixture fx;
    fx.region0()->reset(PageAge::old);
    fx.region1()->reset(PageAge::eden);
    MarkPublicationFixture marking;
    Mutator mutator;
    InstalledMutatorScope installed(mutator);
    auto& buffer = *ThreadLocal::GetGCData().storeBarrierBuffer;
    buffer.clear();
    buffer.Initialize(ZPointerStoreGoodMask);
    auto& field = HeapSlotAt<true>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    const zpointer previous = StoreBadPointer(fx.obj0);
    field.StoreColoured(previous);
    const size_t before = buffer.Pending();
    CJ_MCC_AtomicWriteReference(fx.obj1, fx.obj0, &field, std::memory_order_release);
    const size_t after = buffer.Pending();
    std::fprintf(stderr, "ACCESS976_STORE_ASSERT before=%zu after=%zu raw=%zx\n", before, after, raw(field.GetFieldValue()));
    GC_EXPECT_EQ(after, before + 1);
    GC_EXPECT_EQ(field.GetFieldValue(), StoreGoodPointer(fx.obj1));
    const StoreBarrierEntry& entry = buffer.buffer[buffer.Current()];
    GC_EXPECT_EQ(entry.prev, previous);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(entry.p), reinterpret_cast<uintptr_t>(&field));
    buffer.Flush();
}

GC_TEST(AccessBarrier976, AtomicExchangeHealsWithoutBufferingControl)
{
    GcHeapFixture fx;
    fx.region0()->reset(PageAge::old);
    fx.region1()->reset(PageAge::eden);
    MarkPublicationFixture marking;
    Mutator mutator;
    InstalledMutatorScope installed(mutator);
    auto& buffer = *ThreadLocal::GetGCData().storeBarrierBuffer;
    buffer.clear();
    buffer.Initialize(ZPointerStoreGoodMask);
    auto& field = HeapSlotAt<true>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreBadPointer(fx.obj0));
    const size_t before = buffer.Pending();
    BaseObject* result = CJ_MCC_AtomicSwapReference(fx.obj1, fx.obj0, &field, std::memory_order_seq_cst);
    const size_t after = buffer.Pending();
    std::fprintf(stderr, "ACCESS976_XCHG_ASSERT before=%zu after=%zu result=%p\n", before, after, result);
    GC_EXPECT_EQ(after, before);
    GC_EXPECT_TRUE(result == fx.obj0);
    GC_EXPECT_EQ(field.GetFieldValue(), StoreGoodPointer(fx.obj1));
}

extern "C" bool CJ_MCC_AtomicCompareAndSwapReference(BaseObject*, BaseObject*, BaseObject*, HeapSlot<true>*, MemoryOrder, MemoryOrder);
extern "C" BaseObject* CJ_MCC_AtomicReadReference(BaseObject*, HeapSlot<true>*, MemoryOrder);

GC_TEST(AccessBarrier976, NativeAtomicCompareSuccessFailureAndExchange)
{
    GcHeapFixture fx;
    HeapSlot<true> field(zpointer::null);
    CJ_MCC_AtomicWriteReference(fx.obj0, nullptr, &field, std::memory_order_release);
    GC_EXPECT_EQ(field.GetFieldValue(), StoreGoodPointer(fx.obj0));
    GC_EXPECT_TRUE(CJ_MCC_AtomicReadReference(nullptr, &field, std::memory_order_acquire) == fx.obj0);
    const bool failed = CJ_MCC_AtomicCompareAndSwapReference(fx.obj1, nullptr, nullptr, &field,
        std::memory_order_seq_cst, std::memory_order_seq_cst);
    const bool succeeded = CJ_MCC_AtomicCompareAndSwapReference(fx.obj0, fx.obj1, nullptr, &field,
        std::memory_order_seq_cst, std::memory_order_seq_cst);
    BaseObject* exchanged = CJ_MCC_AtomicSwapReference(nullptr, nullptr, &field, std::memory_order_seq_cst);
    std::fprintf(stderr, "ACCESS976_NATIVE_ASSERT failed=%d succeeded=%d exchanged=%p raw=%zx\n",
        failed, succeeded, exchanged, raw(field.GetFieldValue()));
    GC_EXPECT_FALSE(failed);
    GC_EXPECT_TRUE(succeeded);
    GC_EXPECT_TRUE(exchanged == fx.obj1);
    GC_EXPECT_EQ(field.GetFieldValue(), StoreGoodPointer(nullptr));
}

GC_TEST(AccessBarrier976, HeapAtomicCompareSuccessFailure)
{
    GcHeapFixture fx;
    auto& field = HeapSlotAt<true>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreBadPointer(fx.obj1));
    const bool failed = CJ_MCC_AtomicCompareAndSwapReference(fx.obj0, nullptr, fx.obj0, &field,
        std::memory_order_seq_cst, std::memory_order_seq_cst);
    const zpointer afterFailure = field.GetFieldValue();
    const bool succeeded = CJ_MCC_AtomicCompareAndSwapReference(fx.obj1, fx.obj0, fx.obj0, &field,
        std::memory_order_seq_cst, std::memory_order_seq_cst);
    std::fprintf(stderr, "ACCESS976_CAS_ASSERT failed=%d succeeded=%d raw=%zx\n", failed, succeeded, raw(field.GetFieldValue()));
    GC_EXPECT_FALSE(failed);
    GC_EXPECT_EQ(afterFailure, StoreGoodPointer(fx.obj1));
    GC_EXPECT_TRUE(succeeded);
    GC_EXPECT_EQ(field.GetFieldValue(), StoreGoodPointer(fx.obj0));
}

extern "C" void MCC_WriteRefField(BaseObject*, BaseObject*, HeapSlot<>*);
extern "C" void MCC_WriteRefField_Strong(BaseObject*, BaseObject*, HeapSlot<>*);

GC_TEST(AccessBarrier976, UnknownWeakStoreResolvesAtFieldOffset)
{
    GcHeapFixture fx;
    fx.typeInfo->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
    fx.region0()->reset(PageAge::old);
    fx.region1()->reset(PageAge::eden);
    MarkPublicationFixture marking;
    Mutator mutator;
    InstalledMutatorScope installed(mutator);
    auto& buffer = *ThreadLocal::GetGCData().storeBarrierBuffer;
    buffer.clear();
    buffer.Initialize(ZPointerStoreGoodMask);
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreBadPointer(fx.obj1));
    MCC_WriteRefField(nullptr, fx.obj0, &field);
    std::fprintf(stderr, "ACCESS976_UNKNOWN_ASSERT pending=%zu remembered=%d raw=%zx\n",
        static_cast<size_t>(buffer.Pending()), SlotPageRemembered(reinterpret_cast<MAddress>(&field)), raw(field.GetFieldValue()));
    GC_EXPECT_EQ(buffer.Pending(), 0u);
    GC_EXPECT_TRUE(SlotPageRemembered(reinterpret_cast<MAddress>(&field)));
    GC_EXPECT_EQ(field.GetFieldValue(), StoreGoodPointer(nullptr));
}

GC_TEST(AccessBarrier976, KnownStrongStoreIgnoresWeakHolderControl)
{
    GcHeapFixture fx;
    fx.typeInfo->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
    fx.region0()->reset(PageAge::old);
    fx.region1()->reset(PageAge::eden);
    MarkPublicationFixture marking;
    Mutator mutator;
    InstalledMutatorScope installed(mutator);
    auto& buffer = *ThreadLocal::GetGCData().storeBarrierBuffer;
    buffer.clear();
    buffer.Initialize(ZPointerStoreGoodMask);
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreBadPointer(fx.obj1));
    MCC_WriteRefField_Strong(nullptr, fx.obj0, &field);
    std::fprintf(stderr, "ACCESS976_STRONG_ASSERT pending=%zu raw=%zx\n",
        static_cast<size_t>(buffer.Pending()), raw(field.GetFieldValue()));
    GC_EXPECT_EQ(buffer.Pending(), 1u);
    GC_EXPECT_EQ(field.GetFieldValue(), StoreGoodPointer(nullptr));
    buffer.Flush();
}

#if defined(__linux__)
GC_TEST(StoreAccess1085, ValueRecordNonReferentStore)
{
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        GcHeapFixture fx;
        auto* record = reinterpret_cast<BaseObject*>(reinterpret_cast<MAddress>(fx.obj0) + 16);
        *reinterpret_cast<uint64_t*>(record) = UINT64_C(0x100000090);
        auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(record) + 16);
        const ptrdiff_t offset = reinterpret_cast<uintptr_t>(&field) - reinterpret_cast<uintptr_t>(record);
        const bool heapField = Heap::IsHeapAddress(&field);
        if (!heapField || offset == static_cast<ptrdiff_t>(TYPEINFO_PTR_SIZE)) {
            std::fprintf(stderr, "STORE1085_PATH_MISS heap=%d offset=%ld\n",
                heapField, static_cast<long>(offset));
            _exit(2);
        }
        field.StoreColoured(StoreGoodPointer(nullptr));
        CJ_MCC_WriteRefField(fx.obj1, record, &field);
        const bool installed = field.GetFieldValue() == StoreGoodPointer(fx.obj1);
        std::fprintf(stderr, "STORE1085_PRODUCT_RESULT installed=%d raw=%zx offset=%ld\n",
            installed, raw(field.GetFieldValue()), static_cast<long>(offset));
        _exit(installed ? 0 : 1);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    const bool stored = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::fprintf(stderr, "STORE1085_TARGET_ASSERT waited=%d status=%d stored=%d\n",
        static_cast<int>(waited), status, stored);
    GC_EXPECT_TRUE(stored);
}
#endif
