// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Collector/MarkEngine.h"
#include "Heap/Collector/MarkStripe.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(MarkPort203Publish, TryEndFailsWhenResurrected)
{
    MarkDomain domain(4, VerifyMarkingStacks::MarkingGeneration::MAJOR);
    domain.PrepareWork(1);
    GC_EXPECT_TRUE(domain.TryEnd());
    domain.Terminate().SetResurrected(true);
    GC_EXPECT_FALSE(domain.TryEnd());
}

GC_TEST(MarkPort203Publish, TryTerminateFlushClearsResurrectedAndSeesPublishedWork)
{
    MarkDomain domain(4, VerifyMarkingStacks::MarkingGeneration::YOUNG);
    domain.PrepareWork(1);
    domain.Terminate().SetResurrected(true);
    GC_EXPECT_TRUE(domain.TryTerminateFlush());
    GC_EXPECT_FALSE(domain.Terminate().Resurrected());
    MarkStackEntry entry = MarkStackEntry::PartialArray(1, 3, false);
    domain.Stacks(0).Push(domain.Stripes(), 0, entry, true);
    GC_EXPECT_TRUE(domain.FlushStacks());
    GC_EXPECT_FALSE(domain.TryEnd());
}

GC_TEST(MarkPort203Publish, FlushStacksPublishesPrivateEntries)
{
    MarkDomain domain(4, VerifyMarkingStacks::MarkingGeneration::MAJOR);
    domain.PrepareWork(1);
    MarkStackEntry entry = MarkStackEntry::PartialArray(8, 4, false);
    domain.Stacks(0).Push(domain.Stripes(), 1, entry, false);
    GC_EXPECT_TRUE(domain.Stripes().IsEmpty());
    GC_EXPECT_TRUE(domain.FlushStacks());
    GC_EXPECT_FALSE(domain.Stripes().IsEmpty());
}
