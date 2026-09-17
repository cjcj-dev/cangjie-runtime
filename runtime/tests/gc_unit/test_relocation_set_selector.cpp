// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zRelocationSetSelector.hpp"
#include "Heap/z/zRelocationSetSelector.inline.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(RelocationSetSelector, EmptySet)
{
    ZRelocationSetSelector selector(ZYoungCompactionLimit);
    selector.select();
    GC_EXPECT_EQ(selector.selected_small()->length(), 0);
    GC_EXPECT_EQ(selector.selected_medium()->length(), 0);
    GC_EXPECT_EQ(selector.forwarding_entries(), static_cast<size_t>(0));
}

GC_TEST(RelocationSetSelector, SelectOrderLargeMediumSmall)
{
    ZRelocationSetSelector selector(ZFragmentationLimit);
    selector.select();
    GC_EXPECT_TRUE(selector.empty_pages()->length() == 0);
}
