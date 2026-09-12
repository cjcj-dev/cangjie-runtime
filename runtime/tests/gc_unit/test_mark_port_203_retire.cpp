// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Collector/MarkStripe.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;

GC_TEST(MarkPort203Retire, DestroyReclaimsNonEmptySegment)
{
    MarkStripeStack* stack = MarkStripeStack::Create(true);
    GC_EXPECT_TRUE(stack != nullptr);
    stack->Push(MarkStackEntry::PartialArray(1, 1));
    stack->Push(MarkStackEntry::PartialArray(2, 1));
    GC_EXPECT_FALSE(stack->IsEmpty());
    MarkStripeStack::Destroy(stack);
}

GC_TEST(MarkPort203Retire, FlushThenStealLeavesEmptyOwner)
{
    MarkStripeSet stripes(1);
    MarkingSMR smr(1);
    MarkThreadLocalStacks local(1);
    for (size_t i = 0; i < 3; ++i) {
        local.Push(stripes, 0, MarkStackEntry::PartialArray(i + 1, 1), true);
    }
    GC_EXPECT_TRUE(local.Flush(stripes, true));
    GC_EXPECT_TRUE(local.IsEmpty());
    MarkThreadLocalStacks consumer(1);
    size_t n = 0;
    MarkStackEntry entry;
    while (consumer.Pop(smr, 0, stripes, 0, entry)) {
        ++n;
    }
    GC_EXPECT_EQ(n, 3u);
    GC_EXPECT_TRUE(stripes.IsEmpty());
    GC_EXPECT_TRUE(consumer.IsEmpty());
}
