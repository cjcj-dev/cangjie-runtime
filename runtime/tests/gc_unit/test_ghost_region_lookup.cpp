// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <csignal>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

#include "Heap/Collector/CollectorResources.h"
#include "Heap/WCollector/WCollector.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

void RetireGhostAfterLookup(RegionInfo* region)
{
    region->SetInGhostRegion(0);
}

int WaitChild(pid_t pid)
{
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}

} // namespace

// The product getter fires the one-shot hook only after it has computed a non-null ghost owner.
// The old IsUnmovableFromObject then performs an independent second lookup, which must miss after
// this retirement. A single captured lookup keeps the owner and returns the unmovable answer.
GC_TEST(GhostRegionLookup, RetirementBetweenMembershipAndFetchKeepsCapturedOwner)
{
    pid_t pid = fork();
    GC_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        (void)signal(SIGABRT, SIG_DFL);
        (void)signal(SIGSEGV, SIG_DFL);
        GcHeapFixture fx;
        fx.region0->SetRegionType(RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
        fx.region0->SetInGhostRegion(1);
        RegionInfo::SetGhostLookupTestHook(RetireGhostAfterLookup);

        CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
        WCollector collector(Heap::GetHeap().GetAllocator(), resources);
        const bool unmovable = collector.IsUnmovableFromObject(fx.obj0);
        const bool oneLookup = RegionInfo::GhostLookupTestHookCalls() == 1;
        const bool retired = !RegionInfo::InGhostFromRegion(fx.obj0);
        _exit(unmovable && oneLookup && retired ? 0 : 3);
    }
    GC_EXPECT_EQ(WaitChild(pid), 0);
}
