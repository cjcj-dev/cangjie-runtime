// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Allocator/PageAge.h"
#include "Heap/Collector/TenuringThreshold.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(PageAgeRangeTest, test)
{
    PageAgeRange rangeEden = kPageAgeRangeEden;
    GC_EXPECT_EQ(untype(rangeEden.first()), untype(PageAge::eden));
    GC_EXPECT_EQ(untype(rangeEden.last()), untype(PageAge::eden));

    PageAgeRange rangeYoung = kPageAgeRangeYoung;
    GC_EXPECT_EQ(untype(rangeYoung.first()), untype(PageAge::eden));
    GC_EXPECT_EQ(untype(rangeYoung.last()), untype(PageAge::survivor14));

    PageAgeRange rangeSurvivor = kPageAgeRangeSurvivor;
    GC_EXPECT_EQ(untype(rangeSurvivor.first()), untype(PageAge::survivor1));
    GC_EXPECT_EQ(untype(rangeSurvivor.last()), untype(PageAge::survivor14));

    PageAgeRange rangeRelocation = kPageAgeRangeRelocation;
    GC_EXPECT_EQ(untype(rangeRelocation.first()), untype(PageAge::survivor1));
    GC_EXPECT_EQ(untype(rangeRelocation.last()), untype(PageAge::old));

    PageAgeRange rangeOld = kPageAgeRangeOld;
    GC_EXPECT_EQ(untype(rangeOld.first()), untype(PageAge::old));
    GC_EXPECT_EQ(untype(rangeOld.last()), untype(PageAge::old));

    PageAgeRange rangeAll = kPageAgeRangeAll;
    GC_EXPECT_EQ(untype(rangeAll.first()), untype(PageAge::eden));
    GC_EXPECT_EQ(untype(rangeAll.last()), untype(PageAge::old));
}

GC_TEST(PageAge, UntypeRoundTrip)
{
    GC_EXPECT_EQ(untype(PageAge::eden), 0u);
    GC_EXPECT_EQ(untype(PageAge::old), 15u);
    GC_EXPECT_EQ(untype(PageAge::eden + 1), untype(PageAge::survivor1));
    GC_EXPECT_EQ(untype(PageAge::survivor2 - 1), untype(PageAge::survivor1));
}

GC_TEST(TenuringThreshold, PromoteAllIsZero)
{
    TenuringInputs in;
    in.promoteAll = true;
    in.liveByAge[0] = 64 * 1024 * 1024;
    in.softMaxCapacity = 1024 * 1024 * 1024;
    GC_EXPECT_EQ(ComputeTenuringThreshold(in), 0u);
}

GC_TEST(TenuringThreshold, EmptyYoungIsZero)
{
    TenuringInputs in;
    in.softMaxCapacity = 1024 * 1024 * 1024;
    GC_EXPECT_EQ(ComputeTenuringThreshold(in), 0u);
}

GC_TEST(TenuringThreshold, GenerationalRaisesThreshold)
{
    TenuringInputs in;
    in.liveByAge[0] = 100 * 1024 * 1024;
    in.liveByAge[1] = 20 * 1024 * 1024;
    in.liveByAge[2] = 4 * 1024 * 1024;
    in.youngGarbage = 80 * 1024 * 1024;
    in.youngAllocated = 10 * 1024 * 1024;
    in.softMaxCapacity = 1024 * 1024 * 1024;
    const uint32_t thr = ComputeTenuringThreshold(in);
    GC_EXPECT_EQ(thr, 3u);
}

GC_TEST(TenuringThreshold, PressureLowersThreshold)
{
    TenuringInputs in;
    in.liveByAge[0] = 200 * 1024 * 1024;
    in.liveByAge[1] = 200 * 1024 * 1024;
    in.youngGarbage = 10 * 1024 * 1024;
    in.youngAllocated = 200 * 1024 * 1024;
    in.softMaxCapacity = 256 * 1024 * 1024;
    const uint32_t thr = ComputeTenuringThreshold(in);
    GC_EXPECT_EQ(thr, 1u);
}

GC_TEST(TenuringThreshold, DistributionFlipBothDirections)
{
    TenuringInputs gen;
    gen.liveByAge[0] = 100 * 1024 * 1024;
    gen.liveByAge[1] = 20 * 1024 * 1024;
    gen.liveByAge[2] = 4 * 1024 * 1024;
    gen.youngGarbage = 80 * 1024 * 1024;
    gen.youngAllocated = 10 * 1024 * 1024;
    gen.softMaxCapacity = 1024 * 1024 * 1024;

    TenuringInputs press;
    press.liveByAge[0] = 200 * 1024 * 1024;
    press.liveByAge[1] = 200 * 1024 * 1024;
    press.youngGarbage = 10 * 1024 * 1024;
    press.youngAllocated = 200 * 1024 * 1024;
    press.softMaxCapacity = 256 * 1024 * 1024;

    const uint32_t up = ComputeTenuringThreshold(gen);
    const uint32_t down = ComputeTenuringThreshold(press);
    GC_EXPECT_EQ(up, 3u);
    GC_EXPECT_EQ(down, 1u);
    GC_EXPECT_TRUE(up > down);
}

GC_TEST(TenuringThreshold, ComputeToAge)
{
    GC_EXPECT_EQ(untype(ComputeToAge(PageAge::old, 3)), untype(PageAge::old));
    GC_EXPECT_EQ(untype(ComputeToAge(PageAge::eden, 0)), untype(PageAge::old));
    GC_EXPECT_EQ(untype(ComputeToAge(PageAge::eden, 2)), untype(PageAge::survivor1));
    GC_EXPECT_EQ(untype(ComputeToAge(PageAge::survivor1, 1)), untype(PageAge::old));
    GC_EXPECT_TRUE(ShouldPromoteAge(0, 0));
    GC_EXPECT_FALSE(ShouldPromoteAge(0, 2));
}
