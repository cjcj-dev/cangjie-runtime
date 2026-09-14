// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// zStat.cpp:65-240,386-517,1036-1049. The reference tree has no dedicated
// ZStat gtest; these retain coverage of the public registry and STW entry.
// Private sampler/history construction tests retire with the TU-private types
// per A12b advisor 20260913T211044Z.
#include "gc_unittest.hpp"
#include "Heap/z/zStat.hpp"
#include <cstring>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
const ZStatSampler unobserved("Test", "Unobserved", ZStatUnit::TIME);

GC_TEST(ZStat, RegistrySortedWithoutChangingIdentity)
{
    const auto id = unobserved.Id();
    ZStat::Initialize();
    bool sorted = true;
    size_t comparisons = 0;
    for (const auto* value = ZStatSampler::First(); value != nullptr && value->Next() != nullptr;
         value = value->Next()) {
        const int group = std::strcmp(value->Group(), value->Next()->Group());
        sorted &= group < 0 || (group == 0 && std::strcmp(value->Name(), value->Next()->Name()) <= 0);
        ++comparisons;
    }
    std::fprintf(stderr, "ZSTAT_SORT_ASSERT_EXECUTED comparisons=%zu sorted=%d\n", comparisons, sorted);
    GC_EXPECT_TRUE(comparisons > 0);
    GC_EXPECT_TRUE(sorted);
    GC_EXPECT_EQ(unobserved.Id(), id);
}

GC_TEST(ZStat, RegistryExistsBeforeSampling)
{
    ZStat::Initialize();
    bool found = false;
    for (const auto* value = ZStatSampler::First(); value != nullptr; value = value->Next()) {
        if (value == &unobserved) found = true;
    }
    GC_EXPECT_TRUE(found);

}

GC_TEST(ZStat, StwDepthCounterClassifies)
{
    GC_EXPECT_EQ(ZStat::WorldStoppedNow(), false);
    ZStat::EnterStwScope();
    GC_EXPECT_EQ(ZStat::WorldStoppedNow(), true);
    ZStat::EnterStwScope();
    ZStat::ExitStwScope();
    GC_EXPECT_EQ(ZStat::WorldStoppedNow(), true);
    ZStat::ExitStwScope();
    GC_EXPECT_EQ(ZStat::WorldStoppedNow(), false);
}

} // namespace
