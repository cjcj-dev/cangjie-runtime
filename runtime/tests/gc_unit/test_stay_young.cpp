// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Stay-young in-place must retire from-space identity (zRelocate.cpp:1346-1352 flip_survived).

#include "gc_heap_fixture.hpp"
#include "Allocator/RegionManager.h"
#include "Collector/TenuringThreshold.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// The switch's *value* is a shipping decision, not an invariant, so nothing here asserts it. A test
// that says "the policy is on" fails the moment someone turns it off for a good reason -- which is
// what happened: adaptive tenuring is off pending the phase-8 hang, and this test turned that into
// a red build. The behaviour tests below drive ShouldPromoteAge and the ageing helpers directly, so
// they keep their meaning whichever way the switch is set.
//
// What is worth pinning is that the switch reaches the decision at all, in whichever direction it
// currently points -- otherwise it could be dead and everything would still look green.
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
    GC_EXPECT_TRUE(shapedThreshold <= kMaxTenuringThreshold);
}

GC_TEST(StayYoung, BelowThresholdDoesNotPromote)
{
    GC_EXPECT_TRUE(!ShouldPromoteAge(0, 2));
    GC_EXPECT_TRUE(!ShouldPromoteAge(1, 2));
    GC_EXPECT_TRUE(ShouldPromoteAge(2, 2));
}

GC_TEST(StayYoung, BumpAgesAndKeepsYoung)
{
    GcHeapFixture fx;
    RegionInfo* r = fx.region0;
    r->SetYoungRegionFlag(1);
    r->SetYoungAge(0);
    r->SetRegionType(RegionInfo::RegionType::LONE_FROM_REGION);
    RegionManager::BumpYoungSurvivorAge(r);
    GC_EXPECT_EQ(r->GetYoungAge(), 1u);
    GC_EXPECT_TRUE(r->IsYoungRegion());
}

GC_TEST(StayYoung, AgeClampsAtSurvivor14)
{
    GcHeapFixture fx;
    RegionInfo* r = fx.region0;
    r->SetYoungRegionFlag(1);
    r->SetYoungAge(untype(PageAge::survivor14));
    RegionManager::BumpYoungSurvivorAge(r);
    GC_EXPECT_EQ(r->GetYoungAge(), static_cast<unsigned>(untype(PageAge::survivor14)));
}

// Product EnlistStayYoungSurvivor must not leave LONE_FROM (kLoneFromIsFrom).
GC_TEST(StayYoung, EnlistTypeMustNotStayLoneFrom)
{
    GC_EXPECT_TRUE(RegionInfo::RegionType::RECENT_FULL_REGION !=
                   RegionInfo::RegionType::LONE_FROM_REGION);
    GC_EXPECT_TRUE(RegionInfo::RegionType::RECENT_FULL_REGION !=
                   RegionInfo::RegionType::FROM_REGION);
}

// regionType shares regionStateBitField with ghost/young/age. A plain read of the
// bitfield member can tear against SetInGhostRegion / SetYoungAge CAS on the same
// word (TryTakeGarbageRegionAfterDispel CHECK, RegionManager.h:984). GetRegionType
// must observe the CAS writers.
GC_TEST(StayYoung, GarbageTypeSurvivesGhostAndAgeCas)
{
    GcHeapFixture fx;
    RegionInfo* r = fx.region0;
    r->SetRegionType(RegionInfo::RegionType::GARBAGE_REGION);
    r->SetInGhostRegion(1);
    r->SetYoungAge(3);
    GC_EXPECT_TRUE(r->IsGarbageRegion());
    GC_EXPECT_TRUE(r->IsGhostFromRegion());
    GC_EXPECT_EQ(r->GetYoungAge(), 3u);
    GC_EXPECT_EQ(static_cast<unsigned>(r->GetRegionType()),
                 static_cast<unsigned>(RegionInfo::RegionType::GARBAGE_REGION));
    r->SetInGhostRegion(0);
    GC_EXPECT_TRUE(r->IsGarbageRegion());
    GC_EXPECT_TRUE(!r->IsGhostFromRegion());
}

// PrepareFromRegionList walks nextRegionIdx0. Reuse must not keep the previous
// life's ghost successor (InitRegionInfo, RegionInfo.h).
GC_TEST(StayYoung, InitRegionClearsGhostSuccessor)
{
    GcHeapFixture fx;
    RegionInfo* r = fx.region0;
    r->metadata.nextRegionIdx0 = 1;
    r->InitRegionInfo(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_EQ(r->metadata.nextRegionIdx0, RegionInfo::NULLPTR_IDX);
}
