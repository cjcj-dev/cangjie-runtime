// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Healthy load-barrier entry coverage. The removed FinalizeLoadForMutator
// zero-header termination policy has no ZGC counterpart (barrier:319-343).


#include "gc_heap_fixture.hpp"
#include "CompilerCalls.h"
#include "Interpreter/Options.h"
#include "Interpreter/RTInterface.h"
#include "Heap/z/zThreadLocalData.hpp"

#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zBarrier.hpp"
#include "ObjectModel/RefField.inline.h"
#include "gc_unittest.hpp"

// Test-only read of the heap-wide remembered-set init state so repeated fixtures in one process
// do not double-initialize it (another fixture may already have done so).
#define private public
#include "Heap/z/zRememberedSet.hpp"
#undef private

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" MapleRuntime::ObjectPtr CJ_MCC_ReadRefField(
    MapleRuntime::ObjectPtr obj, MapleRuntime::RefField<false>* field);
extern "C" MapleRuntime::ObjectPtr CJ_MCC_ReadWeakRef(
    MapleRuntime::ObjectPtr obj, MapleRuntime::RefField<false>* field);
extern "C" MapleRuntime::ObjectPtr CJ_MCC_ReadStaticRef(MapleRuntime::NativeSlot* field);
extern "C" MapleRuntime::ObjectPtr CJ_MCC_AtomicReadReference(
    MapleRuntime::ObjectPtr obj, MapleRuntime::RefField<true>* field, MapleRuntime::MemoryOrder order);
extern "C" MapleRuntime::ObjectPtr CJ_MCC_AtomicSwapReference(
    MapleRuntime::ObjectPtr ref, MapleRuntime::ObjectPtr obj, MapleRuntime::RefField<true>* field,
    MapleRuntime::MemoryOrder order);
extern "C" void CJ_MCC_ArrayCopyRef(MapleRuntime::ObjectPtr dstObj, MapleRuntime::MAddress dstField,
                                     size_t dstSize, MapleRuntime::ObjectPtr srcObj,
                                     MapleRuntime::MAddress srcField, size_t srcSize);

namespace {

struct LoadFcFixture {
    LoadFcFixture()
    {
        rememberedSet.Initialize(heap.heapStart, GcHeapFixture::kUnits * ZGranuleSize);
        auto& heapRemset = HeapTestRemset();
        if (!heapRemset.initialized) {
            heapRemset.Initialize(heap.heapStart, GcHeapFixture::kUnits * ZGranuleSize);
        }
        heap.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(heap.obj0) + 128);
        heap.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(heap.obj1) + 128);
    }

    // Ordinary slot pointing at obj0 with the current good colour.
    RefField<>* MakePlainField()
    {
        field = &HeapSlotAt<>(reinterpret_cast<MAddress>(heap.obj1) + TYPEINFO_PTR_SIZE);
        field->StoreColoured(GcUnit::StoreGoodPointer(heap.obj0));
        return field;
    }

    GcHeapFixture heap;
    RememberedSet rememberedSet;
    RefField<false>* field = nullptr;
};


} // namespace

// ---- ordinary ----

GC_TEST(LoadFc, OrdinaryReadHealthyTargetReturnsNormally)
{
    LoadFcFixture fx;
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());
    RefField<>* field = fx.MakePlainField();

    ObjectPtr got = CJ_MCC_LoadBarrierOnOopFieldPreloaded(
        reinterpret_cast<ObjectPtr>(raw(field->GetFieldValue())), reinterpret_cast<volatile zpointer*>(field));

    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

// ---- weak ----

GC_TEST(LoadFc, WeakReadHealthyTargetReturnsNormally)
{
    LoadFcFixture fx;
    fx.heap.typeInfo->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());
    RefField<>* field = fx.MakePlainField();

    ObjectPtr got = CJ_MCC_LoadBarrierOnWeakOopFieldPreloaded(
        reinterpret_cast<ObjectPtr>(raw(field->GetFieldValue())), reinterpret_cast<volatile zpointer*>(field));

    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

// ---- static root ----

GC_TEST(LoadFc, StaticReadHealthyTargetReturnsNormally)
{
    LoadFcFixture fx;
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());
    NativeSlot root(StoreGoodPointer(fx.heap.obj0));

    ObjectPtr got = CJ_MCC_LoadBarrierOnOopFieldPreloaded(
        reinterpret_cast<ObjectPtr>(raw(root.GetFieldValue())), reinterpret_cast<volatile zpointer*>(&root));

    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

// ---- atomic read ----

GC_TEST(LoadFc, AtomicReadHealthyTargetReturnsNormally)
{
    LoadFcFixture fx;
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());
    RefField<true> field(StoreGoodPointer(fx.heap.obj0));

    ObjectPtr got = CJ_MCC_LoadBarrierOnOopFieldPreloaded(
        reinterpret_cast<ObjectPtr>(raw(field.GetFieldValue(std::memory_order_seq_cst))),
        reinterpret_cast<volatile zpointer*>(&field));

    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

// ---- swap old value ----
GC_TEST(LoadFc, SwapOldValueHealthyTargetReturnsNormally)
{
    LoadFcFixture fx;
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());
    RefField<true> field(StoreGoodPointer(fx.heap.obj0));
    BaseObject* newRef = fx.heap.obj1;

    ObjectPtr got = CJ_MCC_AtomicSwapReference(newRef, fx.heap.obj1, &field, std::memory_order_seq_cst);

    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

// ---- bulk (ref-array copy reads each src slot through the load barrier) ----

GC_TEST(LoadFc, BulkCopyHealthySourceReturnsNormally)
{
    LoadFcFixture fx;
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());
    RefField<>* field = fx.MakePlainField();
    RootSlot copied;

    CJ_MCC_ArrayCopyRef(nullptr, reinterpret_cast<MAddress>(&copied), sizeof(copied), fx.heap.obj1,
                        reinterpret_cast<MAddress>(field), sizeof(*field));

    GC_EXPECT_EQ(static_cast<uintptr_t>(raw(copied.LoadPlain())), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

// zBarrierSetRuntime.cpp:29-39 and zBarrier.inline.hpp:319-343: the
// preloaded value selects the result; p only participates in self-healing.
GC_TEST(LoadPreloaded, StrongCompetingStoreReturnsObservedValue)
{
    LoadFcFixture fx;
    const zpointer observed = StoreGoodPointer(fx.heap.obj0);
    ZGlobalsPointers::flip_young_relocate_start();
    const zpointer replacement = StoreGoodPointer(fx.heap.obj1);
    auto* field = fx.MakePlainField();
    field->StoreColoured(replacement);
    GC_EXPECT_TRUE(ZPointer::is_load_bad(observed));
    const ObjectPtr result = CJ_MCC_LoadBarrierOnOopFieldPreloaded(
        reinterpret_cast<ObjectPtr>(raw(observed)), reinterpret_cast<volatile zpointer*>(field));
    std::printf("PRELOADED_STRONG_RESULT actual=%p expected=%p\n", result, fx.heap.obj0);
    GC_EXPECT_TRUE(result == fx.heap.obj0);
    GC_EXPECT_EQ(raw(field->GetFieldValue()), raw(replacement));
}

GC_TEST(LoadPreloaded, WeakCompetingStoreReturnsObservedValue)
{
    LoadFcFixture fx;
    fx.heap.typeInfo->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
    const zpointer observed = StoreGoodPointer(fx.heap.obj0);
    ZGlobalsPointers::flip_young_relocate_start();
    const zpointer replacement = StoreGoodPointer(fx.heap.obj1);
    auto* field = fx.MakePlainField();
    field->StoreColoured(replacement);
    GC_EXPECT_TRUE(ZPointer::is_mark_bad(observed));
    const ObjectPtr result = CJ_MCC_LoadBarrierOnWeakOopFieldPreloaded(
        reinterpret_cast<ObjectPtr>(raw(observed)), reinterpret_cast<volatile zpointer*>(field));
    std::printf("PRELOADED_WEAK_RESULT actual=%p expected=%p\n", result, fx.heap.obj0);
    GC_EXPECT_TRUE(result == fx.heap.obj0);
    GC_EXPECT_EQ(raw(field->GetFieldValue()), raw(replacement));
}

GC_TEST(LoadPreloaded, StrongHealsObservedSlot)
{
    LoadFcFixture fx;
    auto* field = fx.MakePlainField();
    const zpointer observed = field->GetFieldValue();
    ZGlobalsPointers::flip_young_relocate_start();
    const ObjectPtr result = CJ_MCC_LoadBarrierOnOopFieldPreloaded(
        reinterpret_cast<ObjectPtr>(raw(observed)), reinterpret_cast<volatile zpointer*>(field));
    GC_EXPECT_TRUE(result == fx.heap.obj0);
    const zpointer healed = field->GetFieldValue();
    std::printf("PRELOADED_STRONG_HEALED before=%lx after=%lx\n", raw(observed), raw(healed));
    GC_EXPECT_TRUE(ZPointer::is_load_good(healed));
    GC_EXPECT_TRUE(to_object(field->GetTargetObject()) == fx.heap.obj0);
}

GC_TEST(LoadPreloaded, WeakHealsObservedSlot)
{
    LoadFcFixture fx;
    fx.heap.typeInfo->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
    auto* field = fx.MakePlainField();
    const zpointer observed = field->GetFieldValue();
    ZGlobalsPointers::flip_young_relocate_start();
    const ObjectPtr result = CJ_MCC_LoadBarrierOnWeakOopFieldPreloaded(
        reinterpret_cast<ObjectPtr>(raw(observed)), reinterpret_cast<volatile zpointer*>(field));
    GC_EXPECT_TRUE(result == fx.heap.obj0);
    const zpointer healed = field->GetFieldValue();
    std::printf("PRELOADED_WEAK_HEALED before=%lx after=%lx\n", raw(observed), raw(healed));
    GC_EXPECT_TRUE(ZPointer::is_mark_good(healed));
    GC_EXPECT_TRUE(to_object(field->GetTargetObject()) == fx.heap.obj0);
}

GC_TEST(LoadPreloaded, ExportedMarkBadOffsetMatchesThreadData)
{
    GC_EXPECT_EQ(g_cjMarkBadMaskOffset, ThreadGCData::mark_bad_mask_offset());
}

#ifdef INTERPRETER_ENABLED
namespace MapleRuntime {
DYN_CJNativeInterface CreateCJNativeInterface(void* symbolHandle);
}

GC_TEST(LoadPreloaded, InterpreterHeapFieldUsesAccessBarrier)
{
    LoadFcFixture fx;
    auto* field = fx.MakePlainField();
    ZGlobalsPointers::flip_young_relocate_start();
    const DYN_CJNativeInterface interface = CreateCJNativeInterface(nullptr);
    const DYN_ObjRef result = interface.readInstanceField(fx.heap.obj1, field);
    std::printf("INTERPRETER_HEAP_RESULT actual=%p expected=%p\n", result, fx.heap.obj0);
    GC_EXPECT_TRUE(result == fx.heap.obj0);
    GC_EXPECT_TRUE(ZPointer::is_load_good(field->GetFieldValue()));
}

GC_TEST(LoadPreloaded, InterpreterStackFieldUsesPlainAccessor)
{
    LoadFcFixture fx;
    RootSlot slot;
    StorePlain(slot, from_object(fx.heap.obj0));
    const DYN_CJNativeInterface interface = CreateCJNativeInterface(nullptr);
    const DYN_ObjRef result = interface.readInstanceField(nullptr, &slot);
    GC_EXPECT_TRUE(result == fx.heap.obj0);
    GC_EXPECT_EQ(raw(slot.LoadPlain()), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}
#endif
