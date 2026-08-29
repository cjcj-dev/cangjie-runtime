// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// loadfc: six-exit constructive injection. A load-bad / zero-header RefField naming a
// ClearUnits-reused address (header word zeroed, no forwarding answer) must never leave any
// mutator load exit: every arm pins the controlled [LOADFC] abort of one specific product exit,
// and each exit has a healthy positive arm returning normally through the same product wiring.

#include <cstring>
#include <csignal>
#include <functional>
#include <sys/wait.h>
#include <unistd.h>

#include "gc_heap_fixture.hpp"

#include "Common/ColourPredicates.h"
#include "Heap/Barrier/Barrier.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/FinalizerProcessor.h"
#include "Heap/Heap.h"
#include "Heap/WCollector/IdleBarrier.h"
#include "ObjectModel/RefField.inline.h"
#include "gc_unittest.hpp"

// Test-only read of the heap-wide remembered-set init state so repeated fixtures in one process
// do not double-initialize it (the M0Exit fixtures may already have done so).
#define private public
#include "Heap/Barrier/RememberedSet.h"
#undef private

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" MapleRuntime::ObjectPtr CJ_MCC_ReadRefField(
    MapleRuntime::ObjectPtr obj, MapleRuntime::RefField<false>* field);
extern "C" MapleRuntime::ObjectPtr CJ_MCC_ReadWeakRef(
    MapleRuntime::ObjectPtr obj, MapleRuntime::RefField<false>* field);
extern "C" MapleRuntime::ObjectPtr CJ_MCC_ReadStaticRef(MapleRuntime::RootSlot* field);
extern "C" MapleRuntime::ObjectPtr CJ_MCC_AtomicReadReference(
    MapleRuntime::ObjectPtr obj, MapleRuntime::RefField<true>* field, MapleRuntime::MemoryOrder order);
extern "C" MapleRuntime::ObjectPtr CJ_MCC_AtomicSwapReference(
    MapleRuntime::ObjectPtr ref, MapleRuntime::ObjectPtr obj, MapleRuntime::RefField<true>* field,
    MapleRuntime::MemoryOrder order);
extern "C" void CJ_MCC_ArrayCopyRef(MapleRuntime::ObjectPtr dstObj, MapleRuntime::MAddress dstField,
                                     size_t dstSize, MapleRuntime::ObjectPtr srcObj,
                                     MapleRuntime::MAddress srcField, size_t srcSize);

namespace {

class NoAnswerCollector final : public Collector {
public:
    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    FindToVersionResult FindToVersion(BaseObject*) const override { return FindToVersionResult::NotForwarded(); }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    bool IsFromObject(BaseObject*) const override { return false; }
    bool IsGhostFromObject(BaseObject*) const override { return false; }
    bool IsUnmovableFromObject(BaseObject*) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* object) const override
    {
        const uintptr_t remap = ColourPredicates::current_remapped(static_cast<uintptr_t>(::g_cjLoadBadMask));
        return RefField<>(GcUnit::ColouredPointer(object, remap));
    }
    ZGenerationId remap_generation(RefField<>&) const override { return ZGenerationId::old; }
    BaseObject* relocate_or_remap_object(BaseObject* object, ZGenerationId) const override { return object; }
};

class InstalledBarrierScope {
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

struct LoadFcFixture {
    LoadFcFixture() : barrier(collector, rememberedSet), installed(barrier)
    {
        rememberedSet.Initialize(heap.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
        auto& heapRemset = Heap::GetHeap().GetRememberedSet();
        if (!heapRemset.initialized) {
            heapRemset.Initialize(heap.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
        }
        heap.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(heap.obj0) + 128);
        heap.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(heap.obj1) + 128);
    }

    // Zero the target header: the ClearUnits-reuse shape (typeInfo=0x0).
    void ClearTarget() { std::memset(heap.obj0, 0, sizeof(uint64_t)); }

    // Ordinary slot pointing at obj0 with the current good colour.
    RefField<>* MakePlainField()
    {
        field = &HeapSlotAt<>(reinterpret_cast<MAddress>(heap.obj1) + TYPEINFO_PTR_SIZE);
        field->StoreColoured(GcUnit::StoreGoodPointer(heap.obj0));
        return field;
    }

    GcHeapFixture heap;
    NoAnswerCollector collector;
    RememberedSet rememberedSet;
    IdleBarrier barrier;
    InstalledBarrierScope installed;
    RefField<false>* field = nullptr;
};

void ExpectControlledAbort(const std::function<void()>& body)
{
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        (void)signal(SIGABRT, SIG_DFL);
        body();
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
}

} // namespace

// ---- ordinary ----
GC_OTHER_VM_TEST(LoadFc, OrdinaryReadFailsClosedOnZeroHeader)
{
    LoadFcFixture fx;
    fx.ClearTarget();
    RefField<>* field = fx.MakePlainField();

    ExpectControlledAbort([&]() { (void)CJ_MCC_ReadRefField(fx.heap.obj1, field); });
}

GC_TEST(LoadFc, OrdinaryReadHealthyTargetReturnsNormally)
{
    LoadFcFixture fx;
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());
    RefField<>* field = fx.MakePlainField();

    ObjectPtr got = CJ_MCC_ReadRefField(fx.heap.obj1, field);

    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

// ---- weak ----
GC_OTHER_VM_TEST(LoadFc, WeakReadFailsClosedOnZeroHeader)
{
    LoadFcFixture fx;
    fx.ClearTarget();
    RefField<>* field = fx.MakePlainField();

    ExpectControlledAbort([&]() { (void)CJ_MCC_ReadWeakRef(fx.heap.obj1, field); });
}

GC_TEST(LoadFc, WeakReadHealthyTargetReturnsNormally)
{
    LoadFcFixture fx;
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());
    RefField<>* field = fx.MakePlainField();

    ObjectPtr got = CJ_MCC_ReadWeakRef(fx.heap.obj1, field);

    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

// ---- static root ----
GC_OTHER_VM_TEST(LoadFc, StaticReadFailsClosedOnZeroHeader)
{
    LoadFcFixture fx;
    fx.ClearTarget();
    RootSlot root;
    StorePlain(root, from_object(fx.heap.obj0));

    ExpectControlledAbort([&]() { (void)CJ_MCC_ReadStaticRef(&root); });
}

GC_TEST(LoadFc, StaticReadHealthyTargetReturnsNormally)
{
    LoadFcFixture fx;
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());
    RootSlot root;
    StorePlain(root, from_object(fx.heap.obj0));

    ObjectPtr got = CJ_MCC_ReadStaticRef(&root);

    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

// ---- atomic read ----
GC_OTHER_VM_TEST(LoadFc, AtomicReadFailsClosedOnZeroHeader)
{
    LoadFcFixture fx;
    fx.ClearTarget();
    RefField<true> field(to_zpointer(reinterpret_cast<MAddress>(fx.heap.obj0)));

    ExpectControlledAbort([&]() { (void)CJ_MCC_AtomicReadReference(fx.heap.obj1, &field, std::memory_order_seq_cst); });
}

GC_TEST(LoadFc, AtomicReadHealthyTargetReturnsNormally)
{
    LoadFcFixture fx;
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());
    RefField<true> field(to_zpointer(reinterpret_cast<MAddress>(fx.heap.obj0)));

    ObjectPtr got = CJ_MCC_AtomicReadReference(fx.heap.obj1, &field, std::memory_order_seq_cst);

    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

// ---- swap old value ----
GC_OTHER_VM_TEST(LoadFc, SwapOldValueFailsClosedOnZeroHeader)
{
    LoadFcFixture fx;
    fx.ClearTarget();
    RefField<true> field(to_zpointer(reinterpret_cast<MAddress>(fx.heap.obj0)));
    BaseObject* newRef = fx.heap.obj1;

    ExpectControlledAbort([&]() {
        (void)CJ_MCC_AtomicSwapReference(newRef, fx.heap.obj1, &field, std::memory_order_seq_cst);
    });
}

GC_TEST(LoadFc, SwapOldValueHealthyTargetReturnsNormally)
{
    LoadFcFixture fx;
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());
    RefField<true> field(to_zpointer(reinterpret_cast<MAddress>(fx.heap.obj0)));
    BaseObject* newRef = fx.heap.obj1;

    ObjectPtr got = CJ_MCC_AtomicSwapReference(newRef, fx.heap.obj1, &field, std::memory_order_seq_cst);

    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

// ---- bulk (ref-array copy reads each src slot through the load barrier) ----
GC_OTHER_VM_TEST(LoadFc, BulkCopyFailsClosedOnZeroHeaderSource)
{
    LoadFcFixture fx;
    fx.ClearTarget();
    RefField<>* field = fx.MakePlainField();
    RootSlot copied;

    ExpectControlledAbort([&]() {
        CJ_MCC_ArrayCopyRef(nullptr, reinterpret_cast<MAddress>(&copied), sizeof(copied), fx.heap.obj1,
                            reinterpret_cast<MAddress>(field), sizeof(*field));
    });
}

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

// ---- finalizer hand-out (FinalizerProcessor runtime consumer) ----
GC_OTHER_VM_TEST(LoadFc, FinalizerHandOutFailsClosedOnZeroHeader)
{
    LoadFcFixture fx;
    FinalizerProcessor processor;
    processor.RegisterFinalizer(fx.heap.obj0);
    const size_t offset = fx.heap.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.heap.obj0));
    GC_EXPECT_FALSE(fx.heap.region0->ResurrectObject(fx.heap.obj0, offset));
    GC_EXPECT_TRUE(processor.GetReferenceProcessor().DiscoverReference(
                       fx.heap.obj0, ReferenceType::FINAL) == ReferenceStatus::DISCOVERED);
    fx.ClearTarget();

    ExpectControlledAbort([&]() {
        processor.ProcessReferences([](BaseObject*) { return false; });
    });
}
