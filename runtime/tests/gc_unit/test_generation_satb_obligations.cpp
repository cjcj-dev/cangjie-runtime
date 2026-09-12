// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.
// Per-generation publication and independent product mark consumption.
#include "Common/Runtime.h"
#include "gc_heap_fixture.hpp"
#include "Concurrency/Concurrency.h"
#include "gc_unittest.hpp"
#include "mark_publication_fixture.hpp"
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
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    LiveInfo* live = fx.PlantLiveInfo(fx.region1);
    (void)fx.PlantMarkBitmap<Generation::Young>(live, fx.region1->GetRegionSize());
    mark.collector.MarkObjectIfActive(fx.obj0);
    mark.collector.MarkObjectIfActive(fx.obj1);
    GC_EXPECT_EQ(mark.OldPending(), 1u);
    GC_EXPECT_EQ(mark.YoungPending(), 1u);
    TracingCollector::WorkStack work;
    std::vector<BaseObject*> reached;
    GC_EXPECT_TRUE(mark.FollowYoung(work, reached));
    GC_EXPECT_TRUE(work.empty());
    GC_EXPECT_EQ(mark.YoungPending(), 0u);
    GC_EXPECT_EQ(reached.size(), 1u);
    GC_EXPECT_TRUE(reached.front() == fx.obj1);
    // Completing/cleaning young work must leave old's object and carrier intact.
    GC_EXPECT_TRUE(mark.collector.IsMarkedObject<Generation::Young>(fx.obj1));
    GC_EXPECT_EQ(mark.OldPending(), 1u);
    std::vector<BaseObject*> oldObjects;
    mark.DrainOld([&](BaseObject* object, bool) { oldObjects.push_back(object); });
    GC_EXPECT_EQ(oldObjects.size(), 1u);
    GC_EXPECT_TRUE(oldObjects.front() == fx.obj0);
    fx.region1->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}

// Admission-side state invariant from ZGeneration::mark_object_if_active.
// The real successful-pause publication is covered by the product call chain;
// this focused test does not claim to execute FinishOldMark.
GC_TEST(GenerationMark, MarkCompleteStopsOldPublication)
{
    GcHeapFixture fx;
    MarkPublicationFixture mark;
    mark.collector.MarkObjectIfActive(fx.obj0);
    GC_EXPECT_EQ(mark.OldPending(), 1u);
    mark.CompleteOldMarkForAdmissionTest();
    mark.collector.MarkObjectIfActive(fx.obj1);
    GC_EXPECT_EQ(mark.OldPending(), 1u);
    std::vector<BaseObject*> oldObjects;
    mark.DrainOld([&](BaseObject* object, bool) { oldObjects.push_back(object); });
    GC_EXPECT_EQ(oldObjects.size(), 1u);
    GC_EXPECT_TRUE(oldObjects.front() == fx.obj0);
}

GC_TEST(GenerationMark, AllocatedBlackPublishesFollowWithoutSatbNode)
{
    GcHeapFixture fx;
    MarkPublicationFixture mark;
    fx.region0->SetYoungRegionFlag(1);
    Mutator mutator;
    mutator.PublishYoungAllocBlack(fx.obj0);
    GC_EXPECT_EQ(mark.YoungPending(), 1u);
    BaseObject* observed = nullptr;
    bool follow = false;
    mark.Drain([&](BaseObject* object, bool value) { observed = object; follow = value; });
    GC_EXPECT_TRUE(observed == fx.obj0);
    GC_EXPECT_TRUE(follow);
}
// Migrated weak-get cases from gc.TestReferenceRefersToDuringConcMark and
// ZBarrier::blocking_keep_alive_on_weak_slow_path. These are admission tests;
// the fixture does not execute the old mark-end pause or the rendezvous.
GC_TEST(GenerationMark, BlockedWeakReadSeparatesOldStrongAndFinalizable)
{
    GcHeapFixture fx;
    MarkPublicationFixture mark;
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    struct RestoreBlock {
        CollectorResources& resources;
        ~RestoreBlock() { resources.UnblockResurrection(); }
    } restore { resources };
    RefField<> field(StoreGoodPointer(fx.obj0));
    mark.CompleteOldMarkForAdmissionTest();
    resources.BlockResurrection();
    GC_EXPECT_TRUE(CJ_MCC_ReadWeakRef(fx.obj1, &field) == nullptr);
    const size_t offset = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    fx.region0->ResurrectObject(fx.obj0, offset);
    GC_EXPECT_TRUE(CJ_MCC_ReadWeakRef(fx.obj1, &field) == nullptr);
    fx.region0->MarkObject(fx.region0->GetMarkView<Generation::Old>(), fx.obj0, fx.obj0->GetSize());
    GC_EXPECT_TRUE(CJ_MCC_ReadWeakRef(fx.obj1, &field) == fx.obj0);
}

GC_TEST(GenerationMark, BlockedWeakReadKeepsYoungAlive)
{
    GcHeapFixture fx;
    MarkPublicationFixture mark;
    auto& resources = Heap::GetHeap().GetCollectorResources();
    struct RestoreBlock {
        CollectorResources& resources;
        ~RestoreBlock() { resources.UnblockResurrection(); }
    } restore { resources };
    fx.region0->SetYoungRegionFlag(1);
    resources.BlockResurrection();
    RefField<> field(StoreGoodPointer(fx.obj0));
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
    RefField<> field(StoreGoodPointer(fx.obj0));
    GC_EXPECT_TRUE(CJ_MCC_ReadWeakRef(fx.obj1, &field) == fx.obj0);
    std::vector<BaseObject*> published;
    mark.DrainOld([&](BaseObject* object, bool) { published.push_back(object); });
    GC_EXPECT_EQ(published.size(), 1u);
    GC_EXPECT_TRUE(published.front() == fx.obj0);
}

int main(int argc, char** argv)
{
    if (argc == 2) setenv("GC_UNIT_FILTER", argv[1], 1);
    return RunAll();
}
