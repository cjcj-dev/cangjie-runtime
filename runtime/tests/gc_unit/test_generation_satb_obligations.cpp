// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.
// Per-generation publication and independent product mark consumption.
#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "mark_publication_fixture.hpp"
using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" int CJ_ScheduleManagerInit();

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
    WCollector::MinorSlotSet slots;
    WCollector::MinorSlotSet weakSlots;
    GC_EXPECT_TRUE(mark.collector.FollowYoungMark(work, false, reached, slots, weakSlots));
    GC_EXPECT_TRUE(work.empty());
    GC_EXPECT_EQ(mark.YoungPending(), 0u);
    GC_EXPECT_EQ(reached.size(), 1u);
    GC_EXPECT_TRUE(reached.front() == fx.obj1);
    // Completing/cleaning young work must leave old's object and carrier intact.
    GC_EXPECT_TRUE(mark.collector.IsMarkedObject<Generation::Young>(fx.obj1));
    GC_EXPECT_EQ(mark.OldPending(), 1u);
    std::vector<BaseObject*> oldObjects;
    mark.DrainDomain(*mark.collector.majorMarkDomain,
                    [&](BaseObject* object, bool) { oldObjects.push_back(object); });
    GC_EXPECT_EQ(oldObjects.size(), 1u);
    GC_EXPECT_TRUE(oldObjects.front() == fx.obj0);
    fx.region1->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
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
int main(int argc, char** argv)
{
    if (argc == 2) setenv("GC_UNIT_FILTER", argv[1], 1);
    return RunAll();
}
