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
#include "Heap/z/zBarrier.hpp"
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
    Heap::GetHeap().MarkObjectIfActive(fx.obj0);
    Heap::GetHeap().MarkObjectIfActive(fx.obj1);
    GC_EXPECT_EQ(mark.OldPending(), 1u);
    GC_EXPECT_EQ(mark.YoungPending(), 1u);
    WorkStack work;
    std::vector<BaseObject*> reached;
    GC_EXPECT_TRUE(mark.FollowYoung(work, reached));
    GC_EXPECT_TRUE(work.empty());
    GC_EXPECT_EQ(mark.YoungPending(), 0u);
    GC_EXPECT_EQ(reached.size(), 1u);
    GC_EXPECT_TRUE(reached.front() == fx.obj1);
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
    RefField<> field(stored);
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
    RefField<> field(stored);
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
    RefField<> field(stored);
    GC_EXPECT_TRUE(CJ_MCC_ReadWeakRef(fx.obj1, &field) == fx.obj0);
    std::vector<BaseObject*> published;
    mark.DrainOld([&](BaseObject* object, bool) { published.push_back(object); });
    GC_EXPECT_EQ(published.size(), 1u);
    GC_EXPECT_TRUE(published.front() == fx.obj0);
}
