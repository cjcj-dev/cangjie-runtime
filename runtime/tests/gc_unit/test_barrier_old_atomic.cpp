// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>

// Populate reflection metadata in this TU; the runtime keeps its normal access.
#include "Common/TypeDef.h"
#include "Common/Dataref.h"
#define private public
#include "ObjectModel/FieldInfo.h"
#undef private

// This TU enables the existing mark-publication test peer after heap layout is fixed.
// Parse value-owned heap resources with the product macro configuration before
// enabling the existing test peers; their member offsets must match the SO.
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zBarrier.inline.hpp"

#ifndef MRT_TESTABLE_INTERNALS
#define MRT_TESTABLE_INTERNALS 1
#endif

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
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Mutator/Mutator.h"
#include "mark_publication_fixture.hpp"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/RefField.inline.h"
#include "ObjectModel/MObject.h"


#include "gc_generation_test.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" MapleRuntime::ObjectPtr CJ_MCC_AtomicReadReference(
    MapleRuntime::ObjectPtr obj, MapleRuntime::RefField<true>* field, MapleRuntime::MemoryOrder order);

namespace {

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
            replacement->ClearRegion();
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
        ThreadLocal::SetMutator(&mutator);
    }
    ~MutatorScope() { ThreadLocal::SetMutator(saved); }

private:
    Mutator* saved;
};

class MarkWindowScope final {
public:
    MarkWindowScope()
        : started(Heap::GetHeap().IsGcStarted()), reason(Heap::GetHeap().GetZGeneration(ZGenerationId::old).Snapshot().reason)
    {
        phase = Heap::GetHeap().GetZGeneration(ZGenerationId::old).GcPhase();
        activityCycle = &Heap::GetHeap().GetZGeneration(ZGenerationId::old);
        ownerWasActive = activityCycle->Snapshot().active;
        if (!ownerWasActive) activityCycle->Begin(1);
        ZGenerationTest::SetReason(Heap::GetHeap().GetZGeneration(ZGenerationId::old), GC_REASON_USER);
        Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::Mark);
    }
    ~MarkWindowScope()
    {
        Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(phase);
        ZGenerationTest::SetReason(Heap::GetHeap().GetZGeneration(ZGenerationId::old), reason);
        if (!ownerWasActive) activityCycle->End();
    }

private:
    bool started;
    ZGeneration* activityCycle = nullptr;
    bool ownerWasActive = false;
    GCReason reason;
    ZGenerationPhase phase = ZGenerationPhase::Relocate;
};

zpointer LoadBadPointer(BaseObject* object)
{
    const uintptr_t staleRemaps = static_cast<uintptr_t>(::g_cjLoadBadMask) & ZPointerRemappedMask;
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
    StoreFixture()
    {
        regionOld = heap.region0;
        regionNew = heap.region1;
        regionOld->reset(PageAge::old);
        regionNew->reset(PageAge::eden);
        // Page reset stamps allocation sequence; start the snapshot afterwards.
        marking = std::make_unique<MarkPublicationFixture>();
        holder = heap.obj0;
        oldValue = heap.PlaceObject(heap.heapStart + 256);
        newValue = heap.obj1;
        regionOld->SetRegionAllocPtr(reinterpret_cast<MAddress>(oldValue) + oldValue->GetSize());
        field = &HeapSlotAt<>(reinterpret_cast<MAddress>(holder) + TYPEINFO_PTR_SIZE);
        field->StoreColoured(to_zpointer(raw(GcUnit::StoreGoodPointer(oldValue)) ^ ZPointerMarkedOldMask));
        (void)DrainReceipts(oldValue, newValue);
    }

    GcHeapFixture heap;
    std::unique_ptr<MarkPublicationFixture> marking;
    ZPage* regionOld = nullptr;
    ZPage* regionNew = nullptr;
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

    ZBarrier::WriteReference(fixture.holder, *fixture.field, fixture.newValue);
    ThreadLocal::GetGCData().storeBarrierBuffer->Flush();
    const ReceiptCounts receipts = DrainReceipts(fixture.oldValue, fixture.newValue);
    const bool slotRemembered = SlotPageRemembered(reinterpret_cast<MAddress>(fixture.field));
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

    ZBarrier::WriteReference(fixture.holder, *fixture.field, fixture.newValue);
    const size_t pending = ThreadLocal::GetGCData().storeBarrierBuffer->Pending();
    ThreadLocal::GetGCData().storeBarrierBuffer->Flush();
    mutator.FlushStoreBarrierBuffer(false);
    const ReceiptCounts receipts = DrainReceipts(fixture.oldValue, fixture.newValue);
    const bool slotRemembered = SlotPageRemembered(reinterpret_cast<MAddress>(fixture.field));
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
    RefField<true>& field = HeapSlotAt<true>(reinterpret_cast<MAddress>(heap.obj1) + TYPEINFO_PTR_SIZE);
    const zpointer before = LoadBadPointer(heap.obj0);
    field.StoreColoured(before);

    BaseObject* const returned = CJ_MCC_AtomicReadReference(heap.obj1, &field, std::memory_order_seq_cst);
    RefField<> terminal(field.GetFieldValue());
    std::fprintf(stderr, "DETAIL arm=atomic_colour before=%#zx after=%#zx returned=%p target=%p load_good=%u\n",
                 static_cast<size_t>(raw(before)), static_cast<size_t>(raw(terminal.GetFieldValue())), returned,
                 static_cast<void*>(to_object(terminal.GetTargetObject())),
                 static_cast<unsigned>(ZPointer::is_load_good((terminal).GetFieldValue())));
    std::fflush(stderr);

    GC_EXPECT_TRUE(returned == heap.obj0);
    GC_EXPECT_TRUE(to_object(terminal.GetTargetObject()) == heap.obj0);
    GC_EXPECT_TRUE(ZPointer::is_load_good((terminal).GetFieldValue()));
}

GC_TEST(BarrierOldAtomic, AtomicFromToHealsRealSlot)
{
    GcHeapFixture heap;
    BaseObject* to = heap.PlaceObject(heap.heapStart + 256);
    heap.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(to) + to->GetSize());
    RefField<true>& field = HeapSlotAt<true>(reinterpret_cast<MAddress>(heap.obj1) + TYPEINFO_PTR_SIZE);
    const zpointer before = LoadBadPointer(heap.obj0);
    field.StoreColoured(before);

    BaseObject* const returned = CJ_MCC_AtomicReadReference(heap.obj1, &field, std::memory_order_seq_cst);
    RefField<> terminal(field.GetFieldValue());
    std::fprintf(stderr, "DETAIL arm=atomic_from_to before=%#zx after=%#zx returned=%p target=%p load_good=%u\n",
                 static_cast<size_t>(raw(before)), static_cast<size_t>(raw(terminal.GetFieldValue())), returned,
                 static_cast<void*>(to_object(terminal.GetTargetObject())),
                 static_cast<unsigned>(ZPointer::is_load_good((terminal).GetFieldValue())));
    std::fflush(stderr);

    GC_EXPECT_TRUE(returned == heap.obj0);
    GC_EXPECT_TRUE(to_object(terminal.GetTargetObject()) == heap.obj0);
    GC_EXPECT_TRUE(ZPointer::is_load_good((terminal).GetFieldValue()));
}

GC_TEST(BarrierOldAtomic, AtomicCasLostPreservesConcurrentWinner)
{
    GcHeapFixture heap;
    BaseObject* const winner = heap.PlaceObject(heap.heapStart + 256);
    heap.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(winner) + winner->GetSize());
    RefField<true>& field = HeapSlotAt<true>(reinterpret_cast<MAddress>(heap.obj1) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(LoadBadPointer(heap.obj0));
    BaseObject* const returned = CJ_MCC_AtomicReadReference(heap.obj1, &field, std::memory_order_seq_cst);
    field.StoreColoured(StoreGoodPointer(winner), std::memory_order_release);
    RefField<> terminal(field.GetFieldValue());
    GC_EXPECT_TRUE(returned == heap.obj0);
    GC_EXPECT_TRUE(to_object(terminal.GetTargetObject()) == winner);
    GC_EXPECT_TRUE(ZPointer::is_load_good((terminal).GetFieldValue()));
    std::fprintf(stderr, "DETAIL arm=atomic_cas_lost winner=%p\n", winner);
    std::fflush(stderr);
}

// Derived from zBarrierSet.inline.hpp:473/578: a native value payload retains
// its source color until each reference has passed the load barrier.
GC_TEST(BarrierOldAtomic, NativeBulkLoadBadSourceResolvesBeforeHeapPublication)
{
    GcHeapFixture heap;
    NativeSlot source(LoadBadPointer(heap.obj0));
    HeapSlot<>& destination = HeapSlotAt<>(reinterpret_cast<MAddress>(heap.obj1) + TYPEINFO_PTR_SIZE);
    destination.StoreColoured(zpointer::null);

    ZBarrier::ReadStaticStruct(reinterpret_cast<MAddress>(&destination), reinterpret_cast<MAddress>(&source),
                            sizeof(source), heap.typeInfo->GetGCTib());

    GC_EXPECT_EQ(destination.GetFieldValue(), StoreGoodPointer(heap.obj0));
    RootSlot local;
    ZBarrier::ReadStaticStruct(reinterpret_cast<MAddress>(&local), reinterpret_cast<MAddress>(&source),
                            sizeof(source), heap.typeInfo->GetGCTib());
    GC_EXPECT_EQ(raw(local.LoadPlain()), reinterpret_cast<uintptr_t>(heap.obj0));
}

// Derived from zBarrierSet.inline.hpp:258/473 and zBarrier.cpp:272:
// reflection bulk writes perform native old-value work before publishing each
// reference. Exercise the actual SetValue entry for every aggregate type.
GC_TEST(BarrierOldAtomic, ReflectionStaticAggregateStoreRetiresNativeOldValue)
{
    for (TypeKind kind : {TypeKind::TYPE_KIND_STRUCT, TypeKind::TYPE_KIND_TUPLE,
                          TypeKind::TYPE_KIND_ENUM, TypeKind::TYPE_KIND_VARRAY}) {
        GcHeapFixture heap;
        heap.region0->reset(PageAge::old);
        heap.region1->reset(PageAge::eden);
        MarkPublicationFixture marking;
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
        GC_EXPECT_FALSE(SlotPageRemembered(reinterpret_cast<MAddress>(&destination)));
    }
}
