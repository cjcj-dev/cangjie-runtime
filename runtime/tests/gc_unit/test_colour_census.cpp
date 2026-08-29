// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <csignal>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#include "Heap/Verify/ColourCensus.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
namespace {

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(ColourCensus, PlainObjectSlotIsIllegalAndMovesCounter)
{
    GcUnit::GcHeapFixture fixture;
    HeapSlot<>& slot = HeapSlotAt<>(reinterpret_cast<MAddress>(fixture.obj1) + TYPEINFO_PTR_SIZE);
    const uintptr_t plain = reinterpret_cast<uintptr_t>(fixture.obj0);
    std::memcpy(&slot, &plain, sizeof(plain));

    ColourCensusStats stats;
    CensusObjectSlots(fixture.obj1, stats);

    GC_EXPECT_EQ(stats.slots, 1U);
    GC_EXPECT_EQ(stats.total, 1U);
    GC_EXPECT_EQ(stats.plain, 1U);
    GC_EXPECT_EQ(stats.nulls, 0U);
    GC_EXPECT_EQ(stats.coloured, 0U);
    GC_EXPECT_EQ(stats.illegal, 0U);
    GC_EXPECT_TRUE(stats.firstPlainSlot == &slot);
}

GC_TEST(ColourCensus, PlainWriteFunnelFailsClosed)
{
    GcUnit::GcHeapFixture fixture;
    HeapSlot<>& slot = HeapSlotAt<>(reinterpret_cast<MAddress>(fixture.obj1) + TYPEINFO_PTR_SIZE);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        (void)signal(SIGABRT, SIG_DFL);
        slot.StoreColoured(to_zpointer(reinterpret_cast<MAddress>(fixture.obj0)));
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
}

GC_TEST(ColourCensus, IllegalSlotAlwaysFailsClosed)
{
    GcUnit::GcHeapFixture fixture;
    HeapSlot<>& slot = HeapSlotAt<>(reinterpret_cast<MAddress>(fixture.obj1) + TYPEINFO_PTR_SIZE);
    const uintptr_t illegal = reinterpret_cast<uintptr_t>(fixture.obj0) |
        ZPointerRemapped00 | ZPointerRemapped01;
    std::memcpy(&slot, &illegal, sizeof(illegal));

    ColourCensusStats stats;
    CensusObjectSlots(fixture.obj1, stats);
    GC_EXPECT_EQ(stats.illegal, 1U);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        (void)signal(SIGABRT, SIG_DFL);
        EnforceColourCensusForTesting(stats);
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
}
#endif

} // namespace
} // namespace MapleRuntime
