// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc_heap_fixture.hpp must come before product headers that transitively
// include LiveInfo.h: the fixture's access-unlocking window only applies to
// headers it pulls in itself.
#include "gc_heap_fixture.hpp"

#include <csignal>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

#include "Heap/Collector/CollectorResources.h"
#include "Heap/WCollector/WCollector.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

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

[[maybe_unused]] void RetireGhostAfterLookup(RegionInfo* region)
{
    region->SetInGhostRegion(0);
}

int RunIsUnmovableChild(bool armRetireHook)
{
    pid_t pid = fork();
    GC_EXPECT_TRUE(pid >= 0);
    if (pid != 0) {
        return WaitChild(pid);
    }
    (void)signal(SIGABRT, SIG_DFL);
    (void)signal(SIGSEGV, SIG_DFL);
    GcHeapFixture fx;
    fx.region0->SetRegionType(RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
    fx.region0->SetInGhostRegion(1);
#if defined(MRT_GC_UNIT_TESTS)
    if (armRetireHook) {
        RegionInfo::SetGhostLookupTestHook(RetireGhostAfterLookup);
    }
#else
    (void)armRetireHook;
#endif

    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector(Heap::GetHeap().GetAllocator(), resources);
    const bool unmovable = collector.IsUnmovableFromObject(fx.obj0);
#if defined(MRT_GC_UNIT_TESTS)
    const bool oneLookup = !armRetireHook || RegionInfo::GhostLookupTestHookCalls() == 1;
    const bool retired = !armRetireHook || !RegionInfo::InGhostFromRegion(fx.obj0);
    _exit(unmovable && oneLookup && retired ? 0 : 3);
#else
    _exit(unmovable ? 0 : 3);
#endif
}

} // namespace

// Positive control: an object whose owning region carries UNMOVABLE_FROM plus a
// live ghost bit answers true through the ghost lookup path.
GC_TEST(GhostRegionLookup, GhostFromUnmovableRegionAnswersTrue)
{
    GC_EXPECT_EQ(RunIsUnmovableChild(false), 0);
}

#if defined(MRT_GC_UNIT_TESTS)
// The product getter fires the one-shot hook only after it has computed a non-null
// ghost owner; the hook retires the ghost bit so an independent second lookup must
// miss. Pre-fix IsUnmovableFromObject performs that second lookup and dereferences
// the null result (SIGSEGV); post-fix a single captured owner answers the question.
GC_TEST(GhostRegionLookup, RetirementBetweenMembershipAndFetchKeepsCapturedOwner)
{
    GC_EXPECT_EQ(RunIsUnmovableChild(true), 0);
}
#endif
