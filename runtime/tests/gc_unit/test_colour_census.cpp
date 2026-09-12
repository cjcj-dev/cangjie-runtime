// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
namespace {

#if defined(MRT_TESTABLE_INTERNALS)
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

#endif

} // namespace
} // namespace MapleRuntime
