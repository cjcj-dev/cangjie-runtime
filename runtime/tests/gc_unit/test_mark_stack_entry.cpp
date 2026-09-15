// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <cerrno>
#include <cstdio>
#include <type_traits>
#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "Common/MarkWorkStack.h"
#include "Heap/z/zMarkStackEntry.hpp"
#include "gc_unittest.hpp"

namespace MapleRuntime {
class BaseObject;
}
using namespace MapleRuntime;

static_assert(sizeof(MarkStackEntry) == sizeof(uint64_t), "entry cost must stay one word");
static_assert(!std::is_convertible<MarkStackEntry, BaseObject*>::value,
              "a typed continuation must not silently become an object pointer");
static_assert(!std::is_convertible<BaseObject*, MarkStackEntry>::value,
              "zMarkStackEntry.hpp:95-107: an entry is built from a heap offset and explicit flags");

// zMarkStackEntry.hpp:95-140: the two constructors and the eight decoders.
GC_TEST(MarkStackEntry, ObjectPoliciesAreIndependent)
{
    const uintptr_t objectAddress = 0x12345678u;

    const MarkStackEntry both(objectAddress, true, true, true, false);
    GC_EXPECT_EQ(both.object_address(), objectAddress);
    GC_EXPECT_FALSE(both.partial_array());
    GC_EXPECT_TRUE(both.mark());
    GC_EXPECT_TRUE(both.inc_live());
    GC_EXPECT_TRUE(both.follow());
    GC_EXPECT_FALSE(both.finalizable());

    const MarkStackEntry markOnly(objectAddress, true, true, false, true);
    GC_EXPECT_TRUE(markOnly.mark());
    GC_EXPECT_TRUE(markOnly.inc_live());
    GC_EXPECT_FALSE(markOnly.follow());
    GC_EXPECT_TRUE(markOnly.finalizable());

    const MarkStackEntry followOnly(objectAddress, false, false, true, false);
    GC_EXPECT_FALSE(followOnly.mark());
    GC_EXPECT_FALSE(followOnly.inc_live());
    GC_EXPECT_TRUE(followOnly.follow());
    GC_EXPECT_FALSE(followOnly.finalizable());
    GC_EXPECT_EQ(followOnly.object_address(), objectAddress);

    // 59-bit address field round trip (zMarkStackEntry.hpp:81).
    const uintptr_t wide = (uintptr_t(1) << 59) - 8;
    const MarkStackEntry wideEntry(wide, true, false, true, false);
    GC_EXPECT_EQ(wideEntry.object_address(), wide);
    GC_EXPECT_TRUE(wideEntry.mark());
    GC_EXPECT_FALSE(wideEntry.inc_live());
}

GC_TEST(MarkStackEntry, PartialArrayIsASeparateKind)
{
    constexpr size_t offset = 0x12345;
    constexpr size_t length = 0x23456;
    const MarkStackEntry entry(offset, length, true);

    GC_EXPECT_TRUE(entry.partial_array());
    GC_EXPECT_TRUE(entry.finalizable());
    GC_EXPECT_EQ(entry.partial_array_offset(), offset);
    GC_EXPECT_EQ(entry.partial_array_length(), length);
}

GC_TEST(MarkStackEntry, StackSplitPreservesPolicy)
{
    const uintptr_t first = 0x1000;
    const uintptr_t second = 0x2000;
    MarkStack<MarkStackEntry> stack;
    stack.push_back(MarkStackEntry(first, true, true, false, false));
    // Fill a second buffer so split(1) transfers one complete ownership node.
    for (size_t i = 0; i < 64; ++i) {
        stack.push_back(MarkStackEntry(second, false, false, true, false));
    }

    MarkStack<MarkStackEntry> split(stack.split(1));
    GC_EXPECT_FALSE(split.empty());
    const MarkStackEntry transferred = split.back();
    GC_EXPECT_TRUE(transferred.follow());
    GC_EXPECT_FALSE(transferred.mark());
    GC_EXPECT_EQ(transferred.object_address(), second);

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
#if defined(__linux__)
int WaitChild(pid_t child)
{
    int childStatus = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &childStatus, 0);
    } while (waited < 0 && errno == EINTR);
    GC_EXPECT_EQ(waited, child);
    return childStatus;
}
#endif

void ExpectClearCompletes(size_t entries, size_t buffers)
{
#if defined(__linux__)
    std::fflush(nullptr);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        MarkStack<MarkStackEntry> stack;
        for (size_t i = 0; i < entries; ++i) {
            stack.push_back(MarkStackEntry(size_t(i), size_t(1), false));
        }
        if (stack.size() != buffers) {
            std::fprintf(stderr, "MARK_STACK_CLEAR_SETUP_FAILED entries=%zu buffers=%zu\n", entries, stack.size());
            _exit(2);
        }
        std::fprintf(stderr, "MARK_STACK_CLEAR_ENTER entries=%zu buffers=%zu\n", entries, stack.size());
        stack.clear();
        const bool cleared = stack.empty() && stack.size() == 0 &&
            stack.head() == nullptr && stack.tail() == nullptr;
        std::fprintf(stderr, "MARK_STACK_CLEAR_RESULT empty=%d\n", cleared);
        _exit(cleared ? 0 : 3);
    }
    const int childStatus = WaitChild(child);
    std::fprintf(stderr, "MARK_STACK_CLEAR_STATUS entries=%zu status=%d\n", entries, childStatus);
    const bool clearCompletedAndEmpty = WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0;
    GC_EXPECT_TRUE(clearCompletedAndEmpty);
#else
    MarkStack<MarkStackEntry> stack;
    for (size_t i = 0; i < entries; ++i) {
        stack.push_back(MarkStackEntry(size_t(i), size_t(1), false));
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

GC_TEST(MarkStackClear, DestructorAfterMultipleBuffers)
{
#if defined(__linux__)
    std::fflush(nullptr);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        {
            MarkStack<MarkStackEntry> stack;
            for (size_t i = 0; i < 129; ++i) {
                stack.push_back(MarkStackEntry(size_t(i), size_t(1), false));
            }
            if (stack.size() != 3) {
                _exit(2);
            }
            std::fprintf(stderr, "MARK_STACK_DTOR_ENTER buffers=3\n");
        }
        std::fprintf(stderr, "MARK_STACK_DTOR_RESULT empty=1\n");
        _exit(0);
    }
    const int childStatus = WaitChild(child);
    std::fprintf(stderr, "MARK_STACK_DTOR_STATUS status=%d\n", childStatus);
    const bool destructorCompleted = WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0;
    GC_EXPECT_TRUE(destructorCompleted);
#else
    {
        MarkStack<MarkStackEntry> stack;
        for (size_t i = 0; i < 129; ++i) {
            stack.push_back(MarkStackEntry(size_t(i), size_t(1), false));
        }
        GC_EXPECT_EQ(stack.size(), 3U);
    }
#endif
}
