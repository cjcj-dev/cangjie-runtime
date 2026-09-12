// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.
// Tests the explicit ownership API. Concurrent mark/store wiring belongs to
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "mark_publication_fixture.hpp"
using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(GenerationMark, YoungMarkWorkDoesNotConsumeOldStripes)
{
    GcHeapFixture fx;
    MarkPublicationFixture mark;
    fx.region0->SetYoungRegionFlag(0);
    fx.region1->SetYoungRegionFlag(1);
    mark.collector.MarkObjectIfActive(fx.obj0);
    mark.collector.MarkObjectIfActive(fx.obj1);
    GC_EXPECT_EQ(mark.OldPending(), 1u);
    GC_EXPECT_EQ(mark.YoungPending(), 1u);
    std::vector<BaseObject*> objects;
    mark.DrainObjects(objects);
    GC_EXPECT_EQ(objects.size(), 2u);
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
