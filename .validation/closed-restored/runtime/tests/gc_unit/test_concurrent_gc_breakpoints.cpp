// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "gc_unittest.hpp"
#include "CjScheduler.h"
#include "Heap/z/concurrentGCBreakpoints.hpp"
extern "C" int CJ_ScheduleManagerInit();
using MapleRuntime::ConcurrentGCBreakpoints;

// Port of gc/TestConcurrentGCBreakpoints.java. Calls the linked runtime's
// request API; phase notifications come exclusively from its actual GC.
GC_RUNTIME_OTHER_VM_TEST(ConcurrentGCBreakpoints, SimpleCycle)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MapleRuntime::MRT_CjRuntimeInit();
    ConcurrentGCBreakpoints::AcquireControl();
    for (int cycle = 0; cycle < 2; ++cycle) {
        GC_EXPECT_TRUE(ConcurrentGCBreakpoints::RunTo("AFTER MARKING STARTED"));
        GC_EXPECT_TRUE(ConcurrentGCBreakpoints::RunTo("BEFORE MARKING COMPLETED"));
        GC_EXPECT_TRUE(ConcurrentGCBreakpoints::RunTo("AFTER CONCURRENT REFERENCE PROCESSING STARTED"));
        ConcurrentGCBreakpoints::RunToIdle();
    }
    ConcurrentGCBreakpoints::ReleaseControl();
}
GC_RUNTIME_OTHER_VM_TEST(ConcurrentGCBreakpoints, EndBeforeBreakpoint)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MapleRuntime::MRT_CjRuntimeInit();
    ConcurrentGCBreakpoints::AcquireControl();
    GC_EXPECT_TRUE(ConcurrentGCBreakpoints::RunTo("BEFORE MARKING COMPLETED"));
    GC_EXPECT_FALSE(ConcurrentGCBreakpoints::RunTo("AFTER MARKING STARTED"));
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
}
GC_RUNTIME_OTHER_VM_TEST(ConcurrentGCBreakpoints, UnknownBreakpoint)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MapleRuntime::MRT_CjRuntimeInit();
    ConcurrentGCBreakpoints::AcquireControl();
    GC_EXPECT_FALSE(ConcurrentGCBreakpoints::RunTo("UNKNOWN BREAKPOINT"));
    ConcurrentGCBreakpoints::RunToIdle();
    ConcurrentGCBreakpoints::ReleaseControl();
}
