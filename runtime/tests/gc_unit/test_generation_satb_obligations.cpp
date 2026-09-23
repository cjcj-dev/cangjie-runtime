// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.
// Per-generation publication and independent product mark consumption.
//
// Companion of run_generation_cycle_context.sh. Its fixtures reach product
// internals through MRT_TESTABLE_INTERNALS friend access (MarkPublicationFixture
// in CopyCollector.h/CollectorProxy.h/zMark.hpp/zDriver.hpp), so it only exists in
// the testable configuration; the runner builds it only against a testable
// product SO and reports SATB_RC=NOT_RUN otherwise. The process entry is
// gc_unit_main.cpp, the same one as cj_gc_unit, so --gtest_filter= /
// --gtest_list_tests and the GC_OTHER_VM_TEST child re-exec (gc_unittest.hpp
// RunInOtherVm) are handled identically here, one process per test.
#if !defined(MRT_TESTABLE_INTERNALS)
#error "test_generation_satb_obligations.cpp requires MRT_TESTABLE_INTERNALS; build it against a testable product SO"
#endif

#include "Common/Runtime.h"
#include "gc_heap_fixture.hpp"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zResurrection.hpp"
#include "Concurrency/Concurrency.h"
#include "gc_unittest.hpp"
#include "mark_publication_fixture.hpp"
#include <cstdio>
#include "Heap/z/zBarrier.inline.hpp"
using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" int CJ_ScheduleManagerInit();
extern "C" ObjectPtr CJ_MCC_ReadWeakRef(ObjectPtr, RefField<false>*);

namespace {
class GenerationMarkRuntime final : public Runtime {
public:
    explicit GenerationMarkRuntime(MutatorManager& manager)
    {
        mutatorManager = &manager;
        concurrencyModel = &concurrency;
        runtime = this;
        manager.Init();
        const ConcurrencyParam params = {1024, 64, 1};
        concurrency.Init(params);
    }
    ~GenerationMarkRuntime() override { runtime = nullptr; }
    RuntimeParam GetRuntimeParam() const override { return RuntimeParam {}; }
    void SetGCThreshold(uint64_t) override {}
private:
    Concurrency concurrency;
};
}

GC_OTHER_VM_TEST(GenerationMark, YoungMarkWorkDoesNotConsumeOldStripes)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MutatorManager manager;
    GenerationMarkRuntime runtime(manager);
    GcHeapFixture fx;
    MarkPublicationFixture mark;
    fx.region0->reset(PageAge::old);
    fx.region1->reset(PageAge::eden);
    // reset() creates allocating pages; the mark input must predate its cycle.
    // ZGC zPage.inline.hpp:180-186, as in the P1Mark policy fixture.
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    Heap::GetHeap().MarkObjectIfActive(fx.obj0);
    Heap::GetHeap().MarkObjectIfActive(fx.obj1);
    GC_EXPECT_EQ(mark.OldPending(), 1u);
    GC_EXPECT_EQ(mark.YoungPending(), 1u);
    // ZGC zGeneration.cpp:891-895: use the product combined follow task.
    Heap::GetHeap().young().mark_follow();
    const size_t live = fx.region1->live_bytes();
    std::fprintf(stderr, "GENERATION_YOUNG_FOLLOW_ASSERT live=%zu expected=%zu\n",
                 live, static_cast<size_t>(fx.obj1->GetSize()));
    GC_EXPECT_EQ(live, fx.obj1->GetSize());
    GC_EXPECT_EQ(mark.YoungPending(), 0u);
    // Completing/cleaning young work must leave old's object and carrier intact.
    GC_EXPECT_TRUE(RegionSpace::IsMarkedObject<Generation::Young>(fx.obj1));
    GC_EXPECT_EQ(mark.OldPending(), 1u);
    std::vector<BaseObject*> oldObjects;
    mark.DrainOld([&](BaseObject* object, bool) { oldObjects.push_back(object); });
    GC_EXPECT_EQ(oldObjects.size(), 1u);
    GC_EXPECT_TRUE(oldObjects.front() == fx.obj0);
}

// Admission-side state invariant from ZGeneration::mark_object_if_active.
// The real successful-pause publication is covered by the product call chain;
// this focused test does not claim to execute FinishOldMark.
GC_TEST(GenerationMark, MarkCompleteStopsOldPublication)
{
    GcHeapFixture fx;
    MarkPublicationFixture mark;
    Heap::GetHeap().MarkObjectIfActive(fx.obj0);
    GC_EXPECT_EQ(mark.OldPending(), 1u);
    mark.CompleteOldMarkForAdmissionTest();
    Heap::GetHeap().MarkObjectIfActive(fx.obj1);
    GC_EXPECT_EQ(mark.OldPending(), 1u);
    std::vector<BaseObject*> oldObjects;
    mark.DrainOld([&](BaseObject* object, bool) { oldObjects.push_back(object); });
    GC_EXPECT_EQ(oldObjects.size(), 1u);
    GC_EXPECT_TRUE(oldObjects.front() == fx.obj0);
}

// Migrated weak-get cases from gc.TestReferenceRefersToDuringConcMark and
// ZBarrier::blocking_keep_alive_on_weak_slow_path. These are admission tests;
// the fixture does not execute the old mark-end pause or the rendezvous.
GC_TEST(GenerationMark, BlockedWeakReadSeparatesOldStrongAndFinalizable)
{
    GcHeapFixture fx;
    MarkPublicationFixture mark;
    struct RestoreBlock {
        ~RestoreBlock() { ZResurrection::unblock(); }
    } restore;
    RestoreMarkFlips flips;
    const zpointer stored = CaptureStoreGoodThenFlipMark(fx.obj0, flips, false, true);
    GC_EXPECT_TRUE(ZPointer::is_mark_bad(stored));
    std::fprintf(stderr, "SATB_QUAL name=BlockedWeakReadSeparatesOldStrongAndFinalizable mark_bad=%d\n",
                 ZPointer::is_mark_bad(stored) ? 1 : 0);
    fx.typeInfo->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(stored);
    mark.CompleteOldMarkForAdmissionTest();
    ZResurrection::block();
    GC_EXPECT_TRUE(CJ_MCC_ReadWeakRef(fx.obj1, &field) == nullptr);
    (void)GcHeapFixture::MarkFinalizable(fx.region0, fx.obj0);
    GC_EXPECT_TRUE(CJ_MCC_ReadWeakRef(fx.obj1, &field) == nullptr);
    (void)GcHeapFixture::MarkStrong(fx.region0, fx.obj0);
    GC_EXPECT_TRUE(CJ_MCC_ReadWeakRef(fx.obj1, &field) == fx.obj0);
}

GC_TEST(GenerationMark, BlockedWeakReadKeepsYoungAlive)
{
    GcHeapFixture fx;
    MarkPublicationFixture mark;
    struct RestoreBlock {
        ~RestoreBlock() { ZResurrection::unblock(); }
    } restore;
    fx.region0->reset(PageAge::eden);
    ZResurrection::block();
    RestoreMarkFlips flips;
    const zpointer stored = CaptureStoreGoodThenFlipMark(fx.obj0, flips, true, false);
    GC_EXPECT_TRUE(ZPointer::is_mark_bad(stored));
    std::fprintf(stderr, "SATB_QUAL name=BlockedWeakReadKeepsYoungAlive mark_bad=%d\n",
                 ZPointer::is_mark_bad(stored) ? 1 : 0);
    fx.typeInfo->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(stored);
    GC_EXPECT_TRUE(CJ_MCC_ReadWeakRef(fx.obj1, &field) == fx.obj0);
    std::vector<BaseObject*> published;
    mark.Drain([&](BaseObject* object, bool) { published.push_back(object); });
    GC_EXPECT_EQ(published.size(), 1u);
    GC_EXPECT_TRUE(published.front() == fx.obj0);
}

GC_TEST(GenerationMark, UnblockedWeakReadPublishesOldKeepAlive)
{
    GcHeapFixture fx;
    MarkPublicationFixture mark;
    RestoreMarkFlips flips;
    const zpointer stored = CaptureStoreGoodThenFlipMark(fx.obj0, flips, false, true);
    GC_EXPECT_TRUE(ZPointer::is_mark_bad(stored));
    std::fprintf(stderr, "SATB_QUAL name=UnblockedWeakReadPublishesOldKeepAlive mark_bad=%d\n",
                 ZPointer::is_mark_bad(stored) ? 1 : 0);
    fx.typeInfo->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(stored);
    GC_EXPECT_TRUE(CJ_MCC_ReadWeakRef(fx.obj1, &field) == fx.obj0);
    std::vector<BaseObject*> published;
    mark.DrainOld([&](BaseObject* object, bool) { published.push_back(object); });
    GC_EXPECT_EQ(published.size(), 1u);
    GC_EXPECT_TRUE(published.front() == fx.obj0);
}

// ZGC zBarrier.inline.hpp:504-523 and zBarrier.cpp:106-144.
GC_TEST(WeakLoadFamily, BlockedNoKeepAliveRejectsFinalizableOld)
{
    GcHeapFixture fx;
    MarkPublicationFixture mark;
    RestoreMarkFlips flips;
    const zpointer stored = CaptureStoreGoodThenFlipMark(fx.obj0, flips, false, true);
    fx.typeInfo->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(stored);
    (void)GcHeapFixture::MarkFinalizable(fx.region0, fx.obj0);
    mark.CompleteOldMarkForAdmissionTest();
    struct Unblock { ~Unblock() { ZResurrection::unblock(); } } unblock;
    ZResurrection::block();
    const zaddress result = ZBarrier::no_keep_alive_load_barrier_on_weak_oop_field_preloaded(
        reinterpret_cast<volatile zpointer*>(&field), stored);
    const bool strong = fx.region0->is_object_strongly_live(from_object(fx.obj0));
    std::fprintf(stderr, "WEAK_LOAD_RESULT blocked=1 result=%zx strong=%d\n", raw(result), strong);
    GC_EXPECT_TRUE(is_null(result));
    GC_EXPECT_FALSE(strong);
    // Positive arm: same object and pointer, outside the blocked window.
    ZResurrection::unblock();
    const zaddress unblocked = ZBarrier::no_keep_alive_load_barrier_on_weak_oop_field_preloaded(
        reinterpret_cast<volatile zpointer*>(&field), stored);
    std::fprintf(stderr, "WEAK_LOAD_RESULT blocked=0 result=%zx expected=%zx\n",
                 raw(unblocked), reinterpret_cast<uintptr_t>(fx.obj0));
    GC_EXPECT_TRUE(to_object(unblocked) == fx.obj0);
}

GC_TEST(WeakLoadFamily, UnblockedNoKeepAliveDoesNotPublishMark)
{
    GcHeapFixture fx;
    MarkPublicationFixture mark;
    RestoreMarkFlips flips;
    const zpointer stored = CaptureStoreGoodThenFlipMark(fx.obj0, flips, false, true);
    fx.typeInfo->SetType(TypeKind::TYPE_KIND_WEAKREF_CLASS);
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(stored);
    const zaddress result = ZBarrier::no_keep_alive_load_barrier_on_weak_oop_field_preloaded(
        reinterpret_cast<volatile zpointer*>(&field), stored);
    const size_t pending = mark.OldPending();
    std::fprintf(stderr, "WEAK_LOAD_NO_MARK result=%zx pending=%zu\n", raw(result), pending);
    GC_EXPECT_TRUE(to_object(result) == fx.obj0);
    GC_EXPECT_EQ(pending, 0u);
    // Same product pointer through the keep-alive entry must publish marking.
    const zaddress kept = ZBarrier::load_barrier_on_weak_oop_field_preloaded(
        reinterpret_cast<volatile zpointer*>(&field), stored);
    GC_EXPECT_TRUE(to_object(kept) == fx.obj0);
    GC_EXPECT_EQ(mark.OldPending(), 1u);
    mark.DrainOld([](BaseObject*, bool) {});
}

#if defined(MRT_DEBUG) && MRT_DEBUG == 1
GC_OTHER_VM_TEST(WeakLoadFamily, RejectsNonReferentSlot)
{
    constexpr const char* name = "WeakLoadFamily.RejectsNonReferentSlot";
    if (std::getenv("GC_UNIT_WEAK_INVALID_SLOT") == nullptr) {
        GC_EXPECT_EQ(setenv("GC_UNIT_WEAK_INVALID_SLOT", "1", 1), 0);
        try {
            RunInOtherVm(name, "obj->IsWeakRef()");
        } catch (...) {
            unsetenv("GC_UNIT_WEAK_INVALID_SLOT");
            throw;
        }
        unsetenv("GC_UNIT_WEAK_INVALID_SLOT");
        std::fprintf(stderr, "WEAK_SLOT_TARGET_ASSERT_EXECUTED\n");
        return;
    }
    GcHeapFixture fx;
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj1) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreGoodPointer(fx.obj0));
    (void)CJ_MCC_ReadWeakRef(fx.obj1, &field);
}
#endif
