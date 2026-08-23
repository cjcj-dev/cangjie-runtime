// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Common/ColourMask.h"
#include "Heap/Barrier/Barrier.h"
#include "Heap/Verify/ZgcInvariants.h"
#include "gc_heap_fixture.hpp"
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
        return WTERMSIG(status);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}

void EnterIsolatedChild()
{
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        (void)dup2(devnull, STDERR_FILENO);
        (void)dup2(devnull, STDOUT_FILENO);
        (void)close(devnull);
    }
    (void)signal(SIGABRT, SIG_DFL);
}

uintptr_t CurrentGoodMask()
{
    return static_cast<uintptr_t>(::g_cjLoadBadMask) ^ REMAP_COLOUR_MASK;
}

} // namespace

// Positive control for the exact tuple ZgcInvariants.cpp records in product:
// current-good slot + FORWARDED target + ghost-from region.  The same classifier and backing
// counter must advance once; a hollow probe or missing backing makes this test fail.
GC_TEST(FwdReturn, IllegalTuplePositiveControlIncrementsBacking)
{
    GcHeapFixture fx;
    fx.region0->SetInGhostRegion(1);
    fx.obj0->SetStateCode(ObjectState::FORWARDED);
    const uintptr_t goodMask = CurrentGoodMask();
    const uintptr_t slotRaw = reinterpret_cast<uintptr_t>(fx.obj0) | goodMask;
    const uint64_t before = ZgcInvariants::IllegalHitCount();
    const uint64_t delta = ZgcInvariants::InjectIllegalTupleForTest(
        slotRaw, goodMask, fx.obj0, static_cast<uint8_t>(BarrierPhase::FORWARD));
    GC_EXPECT_EQ(delta, 1u);
    GC_EXPECT_EQ(ZgcInvariants::IllegalHitCount(), before + 1);
}

GC_TEST(FwdReturn, HealAndReturnSameAddressPasses)
{
    GcHeapFixture fx;
    const uintptr_t healRaw = reinterpret_cast<uintptr_t>(fx.obj0) | CurrentGoodMask();
    ZgcInvariants::AssertHealMatchesReturn(
        healRaw, fx.obj0, static_cast<uint16_t>(HealSite::ForwardReadReference));
}

// Deliberately violate the invariant.  The child must die SIGABRT, proving the always-on CHECK can
// really fail instead of merely documenting a condition that production never evaluates.
GC_TEST(FwdReturn, HealAndReturnMismatchAborts)
{
    GcHeapFixture fx;
    const uintptr_t healRaw = reinterpret_cast<uintptr_t>(fx.obj0) | CurrentGoodMask();
    pid_t pid = fork();
    GC_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        EnterIsolatedChild();
        ZgcInvariants::AssertHealMatchesReturn(
            healRaw, fx.obj1, static_cast<uint16_t>(HealSite::ForwardReadReference));
        _exit(0);
    }
    GC_EXPECT_EQ(WaitChild(pid), SIGABRT);
}
