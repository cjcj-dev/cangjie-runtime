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

GC_TEST(StayYoung, PolicyOn)
{
    GC_EXPECT_TRUE(kPageAgeAdaptiveTenuring);
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
