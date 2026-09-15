// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "gc_worker_fixture.hpp"
#include <csignal>
#include <cstdlib>
#include <limits>
#include <sys/wait.h>
#include <unistd.h>
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zVerify.hpp"
#include "gc_unittest.hpp"
using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

// Port of the zMarkStack/zMarkStripe population and empty-boundary checks.
GC_TEST(MarkingStacks, PopulationCountsEntriesAndPublishedChunks)
{
    MarkStripeSet stripes(4);
    MarkThreadLocalStacks local(4);
    local.Push(stripes, 2, MarkStackEntry::MarkAndFollow(reinterpret_cast<BaseObject*>(0x1000)), true);
    local.Push(stripes, 2, MarkStackEntry::MarkAndFollow(reinterpret_cast<BaseObject*>(0x2000)), true);
    GC_EXPECT_EQ(local.Population(), 2u);
    GC_EXPECT_EQ(stripes.Population(), 0u);
    GC_EXPECT_TRUE(local.Flush(stripes, true));
    GC_EXPECT_EQ(local.Population(), 0u);
    GC_EXPECT_EQ(stripes.Population(), 1u);
    GC_EXPECT_EQ(stripes.FirstNonEmptyStripe(), 2u);
    MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
    MarkStripeStack* published = stripes.At(2).StealStack(smr, 0);
    GC_EXPECT_TRUE(published != nullptr);
    MarkStripeStack::Destroy(published);
    smr.reclaim();
    GC_EXPECT_EQ(stripes.Population(), 0u);
    GC_EXPECT_EQ(stripes.FirstNonEmptyStripe(), std::numeric_limits<size_t>::max());
}

GC_OTHER_VM_TEST(MarkingStacks, RejectsPublishedStackAndAcceptsDrainedStack)
{
    if (!ZVerifyMarking) {
        // Flags are startup constants. Re-exec, rather than changing a flag
        // after the runtime library has already initialized it.
        GC_EXPECT_EQ(setenv("ZVerifyMarking", "1", 1), 0);
        RunInOtherVm("MarkingStacks.RejectsPublishedStackAndAcceptsDrainedStack");
        return;
    }
    MarkStripeSet stripes(4);
    MarkThreadLocalStacks local(4);
    local.Push(stripes, 1, MarkStackEntry::MarkAndFollow(reinterpret_cast<BaseObject*>(0x1000)), true);
    GC_EXPECT_TRUE(local.Flush(stripes, true));
    GC_EXPECT_EQ(stripes.Population(), 1u);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        signal(SIGABRT, SIG_DFL);
        MarkingStacks::VerifyEmpty(stripes.Population());
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
    MapleRuntime::GcUnit::WorkerFixture workerFixture;
    MarkingSMR smr;
    MarkStripeStack* stack = stripes.At(1).StealStack(smr, 0);
    GC_EXPECT_TRUE(stack != nullptr);
    MarkStripeStack::Destroy(stack);
    smr.reclaim();
    MarkingStacks::VerifyEmpty(stripes.Population());
    GC_EXPECT_EQ(stripes.Population(), 0u);
}
