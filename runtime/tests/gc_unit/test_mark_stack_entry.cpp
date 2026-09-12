// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <type_traits>

#include "Heap/Collector/MarkStackEntry.h"
#include "Heap/Collector/MarkStripe.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;

static_assert(sizeof(MarkStackEntry) == sizeof(uint64_t), "entry cost must stay one word");
static_assert(!std::is_convertible<MarkStackEntry, BaseObject*>::value,
              "a typed continuation must not silently become an object pointer");

GC_TEST(MarkStackEntry, ObjectPoliciesAreIndependent)
{
    BaseObject* const object = reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(0x12345678));

    const MarkStackEntry both = MarkStackEntry::MarkAndFollow(object);
    GC_EXPECT_FALSE(both.partialArray());
    GC_EXPECT_TRUE(both.mark());
    GC_EXPECT_TRUE(both.incLive());
    GC_EXPECT_TRUE(both.follow());
    GC_EXPECT_FALSE(both.finalizable());
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(both.object()), reinterpret_cast<uintptr_t>(object));

    const MarkStackEntry markOnly = MarkStackEntry::MarkOnly(object, true);
    GC_EXPECT_TRUE(markOnly.mark());
    GC_EXPECT_TRUE(markOnly.incLive());
    GC_EXPECT_FALSE(markOnly.follow());
    GC_EXPECT_TRUE(markOnly.finalizable());

    const MarkStackEntry followOnly = MarkStackEntry::FollowOnly(object);
    GC_EXPECT_FALSE(followOnly.mark());
    GC_EXPECT_FALSE(followOnly.incLive());
    GC_EXPECT_TRUE(followOnly.follow());
    GC_EXPECT_FALSE(followOnly.finalizable());
}

GC_TEST(MarkStackEntry, PartialArrayIsASeparateKind)
{
    constexpr size_t offset = 0x12345;
    constexpr size_t length = 0x23456;
    const MarkStackEntry entry = MarkStackEntry::PartialArray(offset, length, true);

    GC_EXPECT_TRUE(entry.partialArray());
    GC_EXPECT_TRUE(entry.finalizable());
    GC_EXPECT_EQ(entry.partialArrayOffset(), offset);
    GC_EXPECT_EQ(entry.partialArrayLength(), length);
}

GC_TEST(MarkStackEntry, StripePublishPreservesPolicy)
{
    BaseObject* const first = reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(0x1000));
    BaseObject* const second = reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(0x2000));
    MarkStripeSet stripes(1);
    MarkingSMR smr(1);
    MarkThreadLocalStacks producer(1);
    producer.Push(stripes, 0, MarkStackEntry::MarkOnly(first), true);
    producer.Push(stripes, 0, MarkStackEntry::FollowOnly(second), true);
    GC_EXPECT_TRUE(producer.Flush(stripes, true));
    GC_EXPECT_TRUE(producer.IsEmpty());

    MarkThreadLocalStacks consumer(1);
    MarkStackEntry entry;
    GC_EXPECT_TRUE(consumer.Pop(smr, 0, stripes, 0, entry));
    GC_EXPECT_TRUE(entry.follow());
    GC_EXPECT_FALSE(entry.mark());
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(entry.object()), reinterpret_cast<uintptr_t>(second));
    GC_EXPECT_TRUE(consumer.Pop(smr, 0, stripes, 0, entry));
    GC_EXPECT_TRUE(entry.mark());
    GC_EXPECT_FALSE(entry.follow());
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(entry.object()), reinterpret_cast<uintptr_t>(first));
    GC_EXPECT_FALSE(consumer.Pop(smr, 0, stripes, 0, entry));
}
