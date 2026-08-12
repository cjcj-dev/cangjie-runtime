// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Deterministic harness for rareyc B / C (gctibzero).
// B: HasRefField TypeInfo with gctib.tag==0 → GCTib pointer-arm SEGV (MClass.h:386).
//    Product fix publishes TYPEINFO_INITED after CalculateGCTib (TypeInfoManager.cpp).
// C: 1-region plan (to2=INVALID) + preLive >= to1used → LiveInfo.cpp:20 CHECK.
//    Product fix sizes to1used by max(counter, bitmap live). CHECK stays.

#include <csignal>
#include <cstdint>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

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
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -2;
}

void WalkHasRefObject(TypeInfo* ti, BaseObject* obj)
{
    *reinterpret_cast<uint64_t*>(obj) = reinterpret_cast<uintptr_t>(ti);
    GC_EXPECT_TRUE(obj->HasRefField());
    obj->ForEachRefField([](RefField<>&) {});
}

} // namespace

// B defect signature: HasRefField + gctib.tag==0 walks the pointer arm (this=0).
// Child must die SIGSEGV. Product consumer is unchanged — this is the crash we refuse
// to swallow with a null check. The publisher fix (INITED after CalculateGCTib) is
// what stops this TypeInfo from being published.
GC_TEST(GctibZero, B_NullGctibSegvIsPointerArm)
{
    GcHeapFixture fx;
    TypeInfo* ti = fx.typeInfo;
    ti->SetFlagHasRefField();
    GCTib empty {};
    empty.tag = 0;
    ti->SetGCTib(empty);
    GC_EXPECT_FALSE(ti->GetGCTib().IsGCTibWord());
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(ti->GetGCTib().gctib), 0u);

    pid_t pid = fork();
    GC_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        WalkHasRefObject(ti, fx.obj0);
        _exit(0);
    }
    int term = WaitChild(pid);
    GC_EXPECT_EQ(term, SIGSEGV);
}

// B after CalculateGCTib: short-word gctib (bit63=1) walks without SEGV.
// Models the payload the publisher now writes before INITED.
GC_TEST(GctibZero, B_AfterCalculateGCTibWalks)
{
    GcHeapFixture fx;
    TypeInfo* ti = fx.typeInfo;
    ti->SetFlagHasRefField();
    GCTib filled {};
    filled.tag = SIGN_BIT | 1;
    ti->SetGCTib(filled);
    GC_EXPECT_TRUE(ti->GetGCTib().IsGCTibWord());
    WalkHasRefObject(ti, fx.obj0);
}

// C defect signature: 1-region plan + preLive >= to1used → CHECK abort.
// Child must die SIGABRT. Proves the CHECK is still armed (not relaxed).
GC_TEST(GctibZero, C_OneRegionElseStillChecks)
{
    RouteInfo ri;
    constexpr uintptr_t kTo1 = 0x30000000u;
    constexpr uint32_t kCounter = 64;
    ri.SetRouteInfo(kTo1, kCounter); // to2 defaults INVALID
    GC_EXPECT_EQ(ri.GetToRegion2Idx(), RouteInfo::INVALID_VALUE);

    pid_t pid = fork();
    GC_EXPECT_TRUE(pid >= 0);
    if (pid == 0) {
        (void)ri.GetRoute(kCounter);
        _exit(0);
    }
    int term = WaitChild(pid);
    GC_EXPECT_EQ(term, SIGABRT);
}

// C after plan-size fix: to1used = max(counter, bitmapLive) keeps prefix in region1.
GC_TEST(GctibZero, C_PlanSizedByBitmapFaceStaysInRegion1)
{
    constexpr uint32_t kCounter = 64;
    constexpr uint32_t kBitmapLive = 128;
    uint32_t fromBytes = kCounter;
    if (kBitmapLive > fromBytes) {
        fromBytes = kBitmapLive;
    }
    RouteInfo ri;
    constexpr uintptr_t kTo1 = 0x40000000u;
    ri.SetRouteInfo(kTo1, fromBytes);
    GC_EXPECT_EQ(ri.GetToRegion2Idx(), RouteInfo::INVALID_VALUE);
    GC_EXPECT_EQ(ri.GetRoute(0), kTo1);
    GC_EXPECT_EQ(ri.GetRoute(kCounter), kTo1 + kCounter);
    GC_EXPECT_EQ(ri.GetRoute(kBitmapLive - 1), kTo1 + (kBitmapLive - 1));
}
