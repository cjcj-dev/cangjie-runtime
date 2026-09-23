// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Surviving age-policy and generation constraints.

#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(StayYoung, PolicySwitchIsWiredToTheDecision)
{
    // promoteAll is the one input that must win regardless of policy: a major collection promotes
    // everything, and no threshold may override that.
    TenuringInputs forced{};
    forced.promoteAll = true;
    forced.liveByAge[1] = 4096;
    GC_EXPECT_EQ(ComputeTenuringThreshold(forced), 0u);

    // And the threshold is computed from the distribution rather than being a constant, which is
    // the property the policy consumes when it is on.
    TenuringInputs shaped{};
    shaped.liveByAge[1] = 4096;
    shaped.liveByAge[2] = 4096;
    shaped.youngAllocated = 1 << 20;
    shaped.softMaxCapacity = 1 << 24;
    const uint32_t shapedThreshold = ComputeTenuringThreshold(shaped);
    GC_EXPECT_TRUE(shapedThreshold <= MaxTenuringThreshold);
}

GC_TEST(StayYoung, BelowThresholdDoesNotPromote)
{
    GC_EXPECT_TRUE(!ShouldPromoteAge(0, 2));
    GC_EXPECT_TRUE(!ShouldPromoteAge(1, 2));
    GC_EXPECT_TRUE(ShouldPromoteAge(2, 2));
}

GC_TEST(StayYoung, OldGenerationCannotRelocateYoungRegion)
{
    GC_EXPECT_TRUE(RegionManager::GenerationMayRelocateYoung(Generation::Young));
    GC_EXPECT_FALSE(RegionManager::GenerationMayRelocateYoung(Generation::Old));
}
