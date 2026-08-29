// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#include "Heap/Verify/ColourCensus.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
namespace {

GC_TEST(ColourCensus, PlainObjectSlotMovesCounter)
{
    GcUnit::GcHeapFixture fixture;
    HeapSlot<>& slot = HeapSlotAt<>(reinterpret_cast<MAddress>(fixture.obj1) + TYPEINFO_PTR_SIZE);
    slot.StoreColoured(to_zpointer(reinterpret_cast<MAddress>(fixture.obj0)));

    ColourCensusStats stats;
    CensusObjectSlots(fixture.obj1, stats);

    GC_EXPECT_EQ(stats.total, 1U);
    GC_EXPECT_EQ(stats.legacyPlain, 1U);
    GC_EXPECT_EQ(stats.nulls, 0U);
    GC_EXPECT_EQ(stats.coloured, 0U);
    GC_EXPECT_EQ(stats.illegal, 0U);
    GC_EXPECT_TRUE(stats.firstPlainSlot == &slot);
}

#if defined(MRT_TESTABLE_INTERNALS)
GC_TEST(ColourCensus, IllegalSlotOnlyFailsWhenArmed)
{
    GcUnit::GcHeapFixture fixture;
    HeapSlot<>& slot = HeapSlotAt<>(reinterpret_cast<MAddress>(fixture.obj1) + TYPEINFO_PTR_SIZE);
    const uintptr_t illegal = reinterpret_cast<uintptr_t>(fixture.obj0) |
        ZPointerRemapped00 | ZPointerRemapped01;
    slot.StoreColoured(to_zpointer(illegal));

    ColourCensusStats stats;
    CensusObjectSlots(fixture.obj1, stats);
    GC_EXPECT_EQ(stats.illegal, 1U);
    EnforceColourCensusForTesting(stats, false);

    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        (void)signal(SIGABRT, SIG_DFL);
        EnforceColourCensusForTesting(stats, true);
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
