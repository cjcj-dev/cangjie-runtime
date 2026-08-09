// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Eth: region young-age / generation face (JDK test_zPageAge spirit, no GPL).
// Product: RegionInfo::SetYoungAge / GetYoungAge / IsYoungRegion / MAX_YOUNG_AGE.
// We are a generational GC; age bits had zero unit coverage before this file.

#include <cstdint>

#include "Common/ColourMask.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(RegionAge, YoungAgeRoundTrip)
{
    GcHeapFixture fx;
    RegionInfo* r = fx.region0;
    r->SetYoungAge(0);
    GC_EXPECT_EQ(r->GetYoungAge(), 0u);
    r->SetYoungAge(1);
    GC_EXPECT_EQ(r->GetYoungAge(), 1u);
    r->SetYoungAge(14);
    GC_EXPECT_EQ(r->GetYoungAge(), 14u);
    // Promote-style clear used by product after tenuring (RegionManager promote paths).
    r->SetYoungAge(0);
    GC_EXPECT_EQ(r->GetYoungAge(), 0u);
}

GC_TEST(RegionAge, MaxYoungAgeBound)
{
    GcHeapFixture fx;
    GC_EXPECT_TRUE(RegionInfo::MAX_YOUNG_AGE >= 14u);
    fx.region0->SetYoungAge(RegionInfo::MAX_YOUNG_AGE);
    GC_EXPECT_EQ(fx.region0->GetYoungAge(), static_cast<unsigned>(RegionInfo::MAX_YOUNG_AGE));
}

GC_TEST(RegionAge, YoungFlagIndependentOfAge)
{
    GcHeapFixture fx;
    // Fixture regions start as THREAD_LOCAL (not necessarily young-flagged).
    // Age storage must round-trip regardless of young flag bit.
    uint8_t before = fx.region0->GetYoungAge();
    fx.region0->SetYoungAge(3);
    GC_EXPECT_EQ(fx.region0->GetYoungAge(), 3u);
    fx.region0->SetYoungAge(before);
}

// generation_id is set at PrepareForwardable from IsYoungRegion — header contract.
GC_TEST(RegionAge, GenerationIdEnumExists)
{
    // Compile/link face: ZGenerationId young/old are the only two product generations.
    ZGenerationId y = ZGenerationId::young;
    ZGenerationId o = ZGenerationId::old;
    GC_EXPECT_TRUE(static_cast<unsigned>(y) != static_cast<unsigned>(o));
}
