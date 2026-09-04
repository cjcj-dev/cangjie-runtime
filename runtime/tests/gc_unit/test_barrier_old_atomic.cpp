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

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#define private public
#include "Heap/Barrier/StoreBarrierBuffer.h"
#undef private

#include "Common/ColourPredicates.h"
#include "Heap/Allocator/AllocBuffer.h"
#include "Heap/Barrier/Barrier.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Heap.h"
#include "Heap/WCollector/TraceBarrier.h"
#include "Mutator/Mutator.h"
#include "Mutator/SatbBuffer.h"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/RefField.inline.h"

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
    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    FindToVersionResult FindToVersion(BaseObject* object) const override
    {
        return object == from && to != nullptr ? FindToVersionResult::Found(to) :
                                                FindToVersionResult::NotForwarded();
    }
    BaseObject* ResolveStoreValue(BaseObject* object, const ForwardingProvenance& = {}) const override
    {
        return object == from && to != nullptr ? to : object;
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
        return ResolveStoreValue(object, ForwardingProvenance{
            ForwardingHolderKind::HeapRef, object, &object });
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
    explicit InstalledBarrierScope(Barrier& barrier) : previous(Heap::currentBarrierPtr), installed(&barrier)
    {
        Heap::currentBarrierPtr = &installed;
    }
    ~InstalledBarrierScope() { Heap::currentBarrierPtr = previous; }

private:
    Barrier** previous;
    Barrier* installed;
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

class MutatorScope final {
public:
    explicit MutatorScope(Mutator& mutator) : saved(ThreadLocal::GetMutator())
    {
        mutator.SetMutatorPhase(GCPhase::GC_PHASE_TRACE);
        ThreadLocal::SetMutator(&mutator);
    }
    ~MutatorScope() { ThreadLocal::SetMutator(saved); }

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
        phase = Heap::GetHeap().GetGCPhase();
        resources.SetGcStarted(true);
        resources.GetGCStats().reason = GC_REASON_USER;
        Heap::GetHeap().SetGCPhase(GCPhase::GC_PHASE_TRACE);
    }
    ~MarkWindowScope()
    {
        Heap::GetHeap().SetGCPhase(phase);
        resources.GetGCStats().reason = reason;
        resources.SetGcStarted(started);
    }

private:
    CollectorResources& resources;
    bool started;
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
    SatbBuffer::Instance().GetRetiredObjects(retired);
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
        field->StoreColoured(GcUnit::StoreGoodPointer(oldValue));
        remembered.Initialize(heap.heapStart, 2 * RegionInfo::UNIT_SIZE);
        (void)DrainReceipts(oldValue, newValue);
    }

    GcHeapFixture heap;
    BarrierCollector collector;
    RememberedSet remembered;
    TraceBarrier barrier;
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
    mutator.FlushSatbBuffer(false);
    const ReceiptCounts receipts = DrainReceipts(fixture.oldValue, fixture.newValue);
    const bool slotRemembered = fixture.remembered.Contains(reinterpret_cast<MAddress>(fixture.field));
    std::fprintf(stderr,
                 "DETAIL arm=no_alloc old_receipt=%zu new_receipt=%zu remset=%u final_target=%p\n",
                 receipts.oldValue, receipts.newValue, static_cast<unsigned>(slotRemembered),
                 static_cast<void*>(to_object(fixture.field->GetTargetObject())));
    std::fflush(stderr);

    GC_EXPECT_EQ(receipts.oldValue, 1u);
    GC_EXPECT_TRUE(receipts.newValue >= 1u);
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
    const size_t pending = alloc.GetStoreBarrierBuffer().Pending();
    alloc.GetStoreBarrierBuffer().Flush(fixture.remembered, fixture.collector);
    mutator.FlushSatbBuffer(false);
    const ReceiptCounts receipts = DrainReceipts(fixture.oldValue, fixture.newValue);
    const bool slotRemembered = fixture.remembered.Contains(reinterpret_cast<MAddress>(fixture.field));
    std::fprintf(stderr,
                 "DETAIL arm=with_alloc pending=%zu old_receipt=%zu new_receipt=%zu remset=%u final_target=%p\n",
                 pending, receipts.oldValue, receipts.newValue, static_cast<unsigned>(slotRemembered),
                 static_cast<void*>(to_object(fixture.field->GetTargetObject())));
    std::fflush(stderr);

    GC_EXPECT_EQ(pending, 1u);
    GC_EXPECT_EQ(receipts.oldValue, 1u);
    GC_EXPECT_TRUE(receipts.newValue >= 1u);
    GC_EXPECT_TRUE(slotRemembered);
}

GC_TEST(BarrierOldAtomic, AtomicColourOnlyHealsRealSlot)
{
    GcHeapFixture heap;
    BarrierCollector collector;
    RememberedSet remembered;
    remembered.Initialize(heap.heapStart, 2 * RegionInfo::UNIT_SIZE);
    TraceBarrier barrier(collector, remembered);
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
    TraceBarrier barrier(collector, remembered);
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
    TraceBarrier barrier(collector, remembered);
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
            // Keep the concurrent winner load-bad. The old unbounded self-heal retries
            // against this word and overwrites it on its second CAS; the one-shot path
            // must stop after losing its single CAS.
            field.StoreColoured(LoadBadPointer(winner), std::memory_order_release);
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
        GC_EXPECT_FALSE(collector.is_load_good(terminal));
    }
    std::fprintf(stderr,
                 "DETAIL arm=atomic_cas_lost forced_failures=%zu max_heal_cas_per_read=1 winner=%p\n",
                 kForcedCasFailures, winner);
    std::fflush(stderr);
}
