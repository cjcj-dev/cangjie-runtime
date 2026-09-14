// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

// This TU alone needs CollectorProxy friendship to publish a real heap phase.
// Product libraries keep their configured macro set.
#ifndef MRT_TESTABLE_INTERNALS
#define MRT_TESTABLE_INTERNALS 1
#endif

// Populate reflection metadata in this TU; the runtime keeps its normal access.
#include "Common/TypeDef.h"
#include "Common/Dataref.h"
#define private public
#include "ObjectModel/FieldInfo.h"
#undef private

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#define private public
#include "Heap/z/zStoreBarrierBuffer.hpp"
#undef private

#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Mutator/Mutator.h"
#include "mark_publication_fixture.hpp"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/RefField.inline.h"
#include "ObjectModel/MObject.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" MapleRuntime::ObjectPtr CJ_MCC_AtomicReadReference(
    MapleRuntime::ObjectPtr obj, MapleRuntime::RefField<true>* field, MapleRuntime::MemoryOrder order);

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

class BarrierCollector final : public Collector {
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
    FindToVersionResult FindToVersion(BaseObject* object, Generation) const override
    {
        return object == from && to != nullptr ? FindToVersionResult::Found(to) :
                                                FindToVersionResult::NotForwarded();
    }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>& field) const override { return IsLoadBad(field); }
    bool IsCurrentPointer(RefField<>& field) const override { return is_load_good(field); }
    bool IsFromObject(BaseObject* object) const override { return object == from && to != nullptr; }
    bool IsGhostFromObject(BaseObject*) const override { return false; }
    bool IsUnmovableFromObject(BaseObject*) const override { return false; }
    ZGenerationId remap_generation(RefField<>&) const override { return ZGenerationId::old; }
    BaseObject* relocate_or_remap_object(BaseObject* object, ZGenerationId) const override
    {
        std::unique_lock<std::mutex> lock(hookMutex);
        if (pauseBeforeHeal) {
            slowLoadObserved = true;
            hookCv.notify_all();
            hookCv.wait(lock, [this]() { return winnerStored; });
        }
        return object == from && to != nullptr ? to : object;
    }
    RefField<> GetAndTryTagRefField(BaseObject* object) const override
    {
        const uintptr_t remap = ColourPredicates::current_remapped(static_cast<uintptr_t>(::g_cjLoadBadMask));
        return RefField<>(GcUnit::ColouredPointer(object, remap));
    }

    BaseObject* from = nullptr;
    BaseObject* to = nullptr;
    mutable std::mutex hookMutex;
    mutable std::condition_variable hookCv;
    mutable bool pauseBeforeHeal = false;
    mutable bool slowLoadObserved = false;
    mutable bool winnerStored = false;
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

class AllocBufferScope final {
public:
    explicit AllocBufferScope(AllocBuffer* replacement)
        : replacement(replacement), saved(ThreadLocal::GetAllocBuffer())
    {
        ThreadLocal::SetAllocBuffer(replacement);
    }
    ~AllocBufferScope()
    {
        ThreadLocal::SetAllocBuffer(saved);
        if (replacement != nullptr) {
            replacement->SetRegion(nullptr);
        }
    }

private:
    AllocBuffer* replacement;
    AllocBuffer* saved;
};

// Supply the barrier TLS context; do not register this fixture with global safepoints.
class MutatorScope final {
public:
    explicit MutatorScope(Mutator& mutator) : saved(ThreadLocal::GetMutator())
    {
        mutator.SetMutatorPhase(GCPhase::GC_PHASE_TRACE);
        ThreadLocal::GetThreadLocalData()->mutator = &mutator;
    }
    ~MutatorScope() { ThreadLocal::GetThreadLocalData()->mutator = saved; }

private:
    Mutator* saved;
};

class MarkWindowScope final {
public:
    MarkWindowScope()
        : resources(Heap::GetHeap().GetCollectorResources()), started(resources.IsGcStarted()),
          reason(resources.GetGCStats().reason)
    {
        RelocationReceiptTestAccess::EnsureCollectorProxyBound(resources);
        phase = Heap::GetHeap().GetGCPhase(GCCycleGeneration::OLD);
        activityCycle = &Heap::GetHeap().GetCollector().GetGenerationCycle(GCCycleGeneration::OLD);
        ownerWasActive = activityCycle->Snapshot().active;
        if (!ownerWasActive) activityCycle->Begin(1);
        resources.GetGCStats().reason = GC_REASON_USER;
        Heap::GetHeap().SetGCPhase(GCCycleGeneration::OLD, GCPhase::GC_PHASE_TRACE);
    }
    ~MarkWindowScope()
    {
        Heap::GetHeap().SetGCPhase(GCCycleGeneration::OLD, phase);
        resources.GetGCStats().reason = reason;
        if (!ownerWasActive) activityCycle->End();
    }

private:
    CollectorResources& resources;
    bool started;
    GenerationCycle* activityCycle = nullptr;
    bool ownerWasActive = false;
    GCReason reason;
    GCPhase phase = GCPhase::GC_PHASE_IDLE;
};

zpointer LoadBadPointer(BaseObject* object)
{
    const uintptr_t staleRemaps = static_cast<uintptr_t>(::g_cjLoadBadMask) & REMAP_COLOUR_MASK;
    const uintptr_t staleRemap = staleRemaps & (~staleRemaps + 1);
    GC_EXPECT_TRUE(staleRemap != 0);
    return GcUnit::ColouredPointer(object, staleRemap);
}

struct ReceiptCounts {
    size_t oldValue = 0;
    size_t newValue = 0;
};

ReceiptCounts DrainReceipts(BaseObject* oldValue, BaseObject* newValue)
{
    std::vector<BaseObject*> retired;
    DrainPublishedMarkObjects(retired);
    ReceiptCounts counts;
    for (BaseObject* object : retired) {
        counts.oldValue += object == oldValue ? 1u : 0u;
        counts.newValue += object == newValue ? 1u : 0u;
    }
    return counts;
}

struct StoreFixture {
    StoreFixture() : barrier(collector, remembered), installed(barrier)
    {
        regionOld = heap.region0;
        regionNew = heap.region1;
        regionOld->SetYoungRegionFlag(0);
        regionNew->SetYoungRegionFlag(1);
        holder = heap.obj0;
        oldValue = heap.PlaceObject(heap.heapStart + 256);
        newValue = heap.obj1;
        regionOld->SetRegionAllocPtr(reinterpret_cast<MAddress>(oldValue) + oldValue->GetSize());
        field = &HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);
        field->StoreColoured(to_zpointer(raw(GcUnit::StoreGoodPointer(oldValue)) ^ MARKED_OLD_MASK));
        remembered.Initialize(heap.heapStart, 2 * RegionInfo::UNIT_SIZE);
        (void)DrainReceipts(oldValue, newValue);
    }

    GcHeapFixture heap;
    MarkPublicationFixture marking;
    BarrierCollector collector;
    RememberedSet remembered;
    Barrier barrier;
    InstalledBarrierScope installed;
    RegionInfo* regionOld = nullptr;
    RegionInfo* regionNew = nullptr;
    BaseObject* holder = nullptr;
    BaseObject* oldValue = nullptr;
    BaseObject* newValue = nullptr;
    RefField<false>* field = nullptr;
};

} // namespace

GC_TEST(BarrierOldAtomic, NoAllocBufferOverwriteRetiresOldValue)
{
    StoreFixture fixture;
    MarkWindowScope markWindow;
    Mutator mutator;
    MutatorScope mutatorScope(mutator);
    AllocBufferScope noBuffer(nullptr);

    fixture.barrier.WriteReference(fixture.holder, *fixture.field, fixture.newValue);
    ThreadLocal::GetGCData().storeBarrierBuffer.Flush(fixture.remembered, fixture.collector);
    const ReceiptCounts receipts = DrainReceipts(fixture.oldValue, fixture.newValue);
    const bool slotRemembered = fixture.remembered.Contains(reinterpret_cast<MAddress>(fixture.field));
    std::fprintf(stderr,
                 "DETAIL arm=no_alloc old_receipt=%zu new_receipt=%zu remset=%u final_target=%p\n",
                 receipts.oldValue, receipts.newValue, static_cast<unsigned>(slotRemembered),
                 static_cast<void*>(to_object(fixture.field->GetTargetObject())));
    std::fflush(stderr);

    GC_EXPECT_EQ(receipts.oldValue, 1u);
    GC_EXPECT_EQ(receipts.newValue, 0u);
    GC_EXPECT_TRUE(slotRemembered);
    GC_EXPECT_TRUE(to_object(fixture.field->GetTargetObject()) == fixture.newValue);
}

GC_TEST(BarrierOldAtomic, AllocBufferOverwriteRetiresOldValueControl)
{
    StoreFixture fixture;
    MarkWindowScope markWindow;
    Mutator mutator;
    MutatorScope mutatorScope(mutator);
    AllocBuffer alloc;
    AllocBufferScope withBuffer(&alloc);

    fixture.barrier.WriteReference(fixture.holder, *fixture.field, fixture.newValue);
    const size_t pending = ThreadLocal::GetGCData().storeBarrierBuffer.Pending();
    ThreadLocal::GetGCData().storeBarrierBuffer.Flush(fixture.remembered, fixture.collector);
    mutator.FlushStoreBarrierBuffer(false);
    const ReceiptCounts receipts = DrainReceipts(fixture.oldValue, fixture.newValue);
    const bool slotRemembered = fixture.remembered.Contains(reinterpret_cast<MAddress>(fixture.field));
    std::fprintf(stderr,
                 "DETAIL arm=with_alloc pending=%zu old_receipt=%zu new_receipt=%zu remset=%u final_target=%p\n",
                 pending, receipts.oldValue, receipts.newValue, static_cast<unsigned>(slotRemembered),
                 static_cast<void*>(to_object(fixture.field->GetTargetObject())));
    std::fflush(stderr);

    GC_EXPECT_EQ(pending, 1u);
    GC_EXPECT_EQ(receipts.oldValue, 1u);
    GC_EXPECT_EQ(receipts.newValue, 0u);
    GC_EXPECT_TRUE(slotRemembered);
}

GC_TEST(BarrierOldAtomic, AtomicColourOnlyHealsRealSlot)
{
    GcHeapFixture heap;
    BarrierCollector collector;
    RememberedSet remembered;
    remembered.Initialize(heap.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier barrier(collector, remembered);
    InstalledBarrierScope installed(barrier);
    RefField<true>& field = HeapSlotAt<true>(reinterpret_cast<MAddress>(heap.obj1) + TYPEINFO_PTR_SIZE);
    const zpointer before = LoadBadPointer(heap.obj0);
    field.StoreColoured(before);

    BaseObject* const returned = CJ_MCC_AtomicReadReference(heap.obj1, &field, std::memory_order_seq_cst);
    RefField<> terminal(field.GetFieldValue());
    std::fprintf(stderr, "DETAIL arm=atomic_colour before=%#zx after=%#zx returned=%p target=%p load_good=%u\n",
                 static_cast<size_t>(raw(before)), static_cast<size_t>(raw(terminal.GetFieldValue())), returned,
                 static_cast<void*>(to_object(terminal.GetTargetObject())),
                 static_cast<unsigned>(collector.is_load_good(terminal)));
    std::fflush(stderr);

    GC_EXPECT_TRUE(returned == heap.obj0);
    GC_EXPECT_TRUE(to_object(terminal.GetTargetObject()) == heap.obj0);
    GC_EXPECT_TRUE(collector.is_load_good(terminal));
}

GC_TEST(BarrierOldAtomic, AtomicFromToHealsRealSlot)
{
    GcHeapFixture heap;
    BarrierCollector collector;
    collector.from = heap.obj0;
    collector.to = heap.PlaceObject(heap.heapStart + 256);
    heap.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(collector.to) + collector.to->GetSize());
    RememberedSet remembered;
    remembered.Initialize(heap.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier barrier(collector, remembered);
    InstalledBarrierScope installed(barrier);
    RefField<true>& field = HeapSlotAt<true>(reinterpret_cast<MAddress>(heap.obj1) + TYPEINFO_PTR_SIZE);
    const zpointer before = LoadBadPointer(collector.from);
    field.StoreColoured(before);

    BaseObject* const returned = CJ_MCC_AtomicReadReference(heap.obj1, &field, std::memory_order_seq_cst);
    RefField<> terminal(field.GetFieldValue());
    std::fprintf(stderr, "DETAIL arm=atomic_from_to before=%#zx after=%#zx returned=%p target=%p load_good=%u\n",
                 static_cast<size_t>(raw(before)), static_cast<size_t>(raw(terminal.GetFieldValue())), returned,
                 static_cast<void*>(to_object(terminal.GetTargetObject())),
                 static_cast<unsigned>(collector.is_load_good(terminal)));
    std::fflush(stderr);

    GC_EXPECT_TRUE(returned == collector.to);
    GC_EXPECT_TRUE(to_object(terminal.GetTargetObject()) == collector.to);
    GC_EXPECT_TRUE(collector.is_load_good(terminal));
}

GC_TEST(BarrierOldAtomic, AtomicCasLostPreservesConcurrentWinner)
{
    GcHeapFixture heap;
    BarrierCollector collector;
    BaseObject* const winner = heap.PlaceObject(heap.heapStart + 256);
    heap.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(winner) + winner->GetSize());
    RememberedSet remembered;
    remembered.Initialize(heap.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier barrier(collector, remembered);
    InstalledBarrierScope installed(barrier);
    RefField<true>& field = HeapSlotAt<true>(reinterpret_cast<MAddress>(heap.obj1) + TYPEINFO_PTR_SIZE);
    collector.pauseBeforeHeal = true;
    constexpr size_t kForcedCasFailures = 8;
    for (size_t round = 0; round < kForcedCasFailures; ++round) {
        field.StoreColoured(LoadBadPointer(heap.obj0));
        {
            std::lock_guard<std::mutex> lock(collector.hookMutex);
            collector.slowLoadObserved = false;
            collector.winnerStored = false;
        }

        std::thread writer([&]() {
            std::unique_lock<std::mutex> lock(collector.hookMutex);
            collector.hookCv.wait(lock, [&]() { return collector.slowLoadObserved; });
            // ZBarrier::self_heal (zBarrier.inline.hpp:98) preserves a winner
            // satisfying the fast path. A real mutator store publishes store-good.
            field.StoreColoured(StoreGoodPointer(winner), std::memory_order_release);
            collector.winnerStored = true;
            lock.unlock();
            collector.hookCv.notify_all();
        });
        JoinGuard join(writer);

        BaseObject* const returned = CJ_MCC_AtomicReadReference(heap.obj1, &field, std::memory_order_seq_cst);
        writer.join();
        RefField<> terminal(field.GetFieldValue());
        GC_EXPECT_TRUE(returned == heap.obj0);
        GC_EXPECT_TRUE(to_object(terminal.GetTargetObject()) == winner);
        GC_EXPECT_TRUE(collector.is_load_good(terminal));
    }
    std::fprintf(stderr,
                 "DETAIL arm=atomic_cas_lost forced_failures=%zu winner_store_good=1 winner=%p\n",
                 kForcedCasFailures, winner);
    std::fflush(stderr);
}

// Derived from zBarrierSet.inline.hpp:473/578: a native value payload retains
// its source color until each reference has passed the load barrier.
GC_TEST(BarrierOldAtomic, NativeBulkLoadBadSourceResolvesBeforeHeapPublication)
{
    GcHeapFixture heap;
    BarrierCollector collector;
    RememberedSet remembered;
    remembered.Initialize(heap.heapStart, 2 * RegionInfo::UNIT_SIZE);
    Barrier barrier(collector, remembered);
    collector.from = heap.obj0;
    collector.to = heap.obj1;
    NativeSlot source(LoadBadPointer(heap.obj0));
    HeapSlot<>& destination = HeapSlotAt<>(reinterpret_cast<MAddress>(heap.obj1) + TYPEINFO_PTR_SIZE);
    destination.StoreColoured(zpointer::null);

    barrier.ReadStaticStruct(reinterpret_cast<MAddress>(&destination), reinterpret_cast<MAddress>(&source),
                            sizeof(source), heap.typeInfo->GetGCTib());

    GC_EXPECT_EQ(destination.GetFieldValue(), StoreGoodPointer(heap.obj1));
    // The same native source copied to a mutator-local value must be plain.
    RootSlot local;
    barrier.ReadStaticStruct(reinterpret_cast<MAddress>(&local), reinterpret_cast<MAddress>(&source),
                            sizeof(source), heap.typeInfo->GetGCTib());
    GC_EXPECT_EQ(raw(local.LoadPlain()), reinterpret_cast<uintptr_t>(heap.obj1));
}

// Derived from zBarrierSet.inline.hpp:258/473 and zBarrier.cpp:272:
// reflection bulk writes perform native old-value work before publishing each
// reference. Exercise the actual SetValue entry for every aggregate type.
GC_TEST(BarrierOldAtomic, ReflectionStaticAggregateStoreRetiresNativeOldValue)
{
    for (TypeKind kind : {TypeKind::TYPE_KIND_STRUCT, TypeKind::TYPE_KIND_TUPLE,
                          TypeKind::TYPE_KIND_ENUM, TypeKind::TYPE_KIND_VARRAY}) {
        GcHeapFixture heap;
        MarkPublicationFixture marking;
        heap.region0->SetYoungRegionFlag(0);
        heap.region1->SetYoungRegionFlag(1);
        BarrierCollector collector;
        RememberedSet remembered;
        remembered.Initialize(heap.heapStart, 2 * RegionInfo::UNIT_SIZE);
        Barrier barrier(collector, remembered);
        InstalledBarrierScope installed(barrier);
        alignas(TypeInfo) unsigned char componentStorage[sizeof(TypeInfo)] {};
        auto* component = reinterpret_cast<TypeInfo*>(componentStorage);
        component->SetType(TypeKind::TYPE_KIND_CLASS);
        heap.typeInfo->SetType(kind);
        if (kind == TypeKind::TYPE_KIND_VARRAY) {
            heap.typeInfo->SetComponentTypeInfo(component);
        }
        HeapSlot<>& source = HeapSlotAt<>(reinterpret_cast<MAddress>(heap.obj1) + TYPEINFO_PTR_SIZE);
        source.StoreColoured(StoreGoodPointer(heap.obj1));
        NativeSlot destination(LoadBadPointer(heap.obj0));
        StaticFieldInfo field {};
        field.fieldTypeInfo = heap.typeInfo;
        field.addr = reinterpret_cast<MAddress>(&destination);

        field.SetValue(static_cast<ObjRef>(heap.obj1));

        GC_EXPECT_EQ(destination.GetFieldValue(), StoreGoodPointer(heap.obj1));
        bool oldValueRetained = false;
        marking.DrainOld([&](BaseObject* object, bool) {
            oldValueRetained |= object == heap.obj0;
        });
        GC_EXPECT_TRUE(oldValueRetained);
        GC_EXPECT_FALSE(remembered.Contains(reinterpret_cast<MAddress>(&destination)));
    }
}
