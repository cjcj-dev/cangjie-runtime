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

namespace {
void ExpectClearCompletes(size_t entries, size_t buffers)
{
#if defined(__linux__)
    std::fflush(nullptr);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        MarkStack<MarkStackEntry> stack;
        for (size_t i = 0; i < entries; ++i) {
            stack.push_back(MarkStackEntry::PartialArray(i, 1));
        }
        if (stack.size() != buffers) {
            std::fprintf(stderr, "MARK_STACK_CLEAR_SETUP_FAILED entries=%zu buffers=%zu\n", entries, stack.size());
            _exit(2);
        }
        std::fprintf(stderr, "MARK_STACK_CLEAR_ENTER entries=%zu buffers=%zu\n", entries, stack.size());
        stack.clear();
        const bool cleared = stack.empty() && stack.size() == 0 &&
            stack.head() == nullptr && stack.tail() == nullptr;
        if (!cleared) {
            _exit(3);
        }
        stack.clear();
        if (entries == 0) {
            std::fprintf(stderr, "MARK_STACK_CLEAR_RESULT empty=1\n");
            _exit(0);
        }
        stack.push_back(MarkStackEntry::PartialArray(7, 1));
        if (stack.size() != 1 || stack.back().partialArrayOffset() != 7) {
            _exit(4);
        }
        stack.clear();
        const bool reusable = stack.empty() && stack.size() == 0 &&
            stack.head() == nullptr && stack.tail() == nullptr;
        std::fprintf(stderr, "MARK_STACK_CLEAR_RESULT empty=%d\n", reusable);
        _exit(reusable ? 0 : 5);
    }
    int childStatus = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &childStatus, 0);
    } while (waited < 0 && errno == EINTR);
    GC_EXPECT_EQ(waited, child);
    std::fprintf(stderr, "MARK_STACK_CLEAR_STATUS entries=%zu status=%d\n", entries, childStatus);
    const bool clearCompletedAndEmpty = WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0;
    GC_EXPECT_TRUE(clearCompletedAndEmpty);
#else
    MarkStack<MarkStackEntry> stack;
    for (size_t i = 0; i < entries; ++i) {
        stack.push_back(MarkStackEntry::PartialArray(i, 1));
    }
    GC_EXPECT_EQ(stack.size(), buffers);
    stack.clear();
    GC_EXPECT_TRUE(stack.empty());
    GC_EXPECT_EQ(stack.size(), 0U);
    GC_EXPECT_TRUE(stack.head() == nullptr && stack.tail() == nullptr);
#endif
}
} // namespace

GC_TEST(MarkStackClear, Empty)
{
    ExpectClearCompletes(0, 0);
}

GC_TEST(MarkStackClear, SingleBuffer)
{
    ExpectClearCompletes(1, 1);
}

GC_TEST(MarkStackClear, MultipleBuffers)
{
    ExpectClearCompletes(129, 3);
}
