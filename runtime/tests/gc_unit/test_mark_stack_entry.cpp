// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <type_traits>

#include "Common/MarkWorkStack.h"
#include "Heap/Collector/MarkStackEntry.h"
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

GC_TEST(MarkStackEntry, StackSplitPreservesPolicy)
{
    BaseObject* const first = reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(0x1000));
    BaseObject* const second = reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(0x2000));
    MarkStack<MarkStackEntry> stack;
    stack.push_back(MarkStackEntry::MarkOnly(first));
    // Fill a second buffer so split(1) transfers one complete ownership node.
    for (size_t i = 0; i < 64; ++i) {
        stack.push_back(MarkStackEntry::FollowOnly(second));
    }

    MarkStack<MarkStackEntry> split(stack.split(1));
    GC_EXPECT_FALSE(split.empty());
    const MarkStackEntry transferred = split.back();
    GC_EXPECT_TRUE(transferred.follow());
    GC_EXPECT_FALSE(transferred.mark());
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(transferred.object()), reinterpret_cast<uintptr_t>(second));

    // Production marking tasks drain their owned stack before destruction.
    // Keep that ownership protocol here; MarkStack::clear() is not a substitute
    // for draining a non-empty tail buffer.
    while (!split.empty()) {
        split.pop_back();
    }
    while (!stack.empty()) {
        stack.pop_back();
    }
}
