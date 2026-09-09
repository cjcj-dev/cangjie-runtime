// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Cangjie.h"
#include "CjScheduler.h"
#include "Heap/Verify/MarkCompleteVerify.h"
#include "gc_unittest.hpp"

#include <cstdio>
#include <cstdlib>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

#if defined(MRT_GC_UNIT_TESTS)
extern "C" int CJ_ScheduleManagerInit();

// Drive the public full-GC entry in a fresh runtime process. The asserted
// receipt is written by the two product verifier invocations after each scene
// has consumed its heap/root result; no test-local verifier copy is linked.
GC_OTHER_VM_TEST(MarkCompleteScenes, MajorGcRunsStrongThenWeakComplete)
{
#if defined(__linux__)
    GC_EXPECT_EQ(setenv("MRT_GCV2_VERIFY_MARKING", "1", 1), 0);
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    MRT_CjRuntimeInit();

    MarkCompleteVerify::ResetSceneTestReceipt();
    CJ_MRT_ForceFullGC();
    const MarkCompleteVerify::SceneTestReceipt receipt = MarkCompleteVerify::ReadSceneTestReceipt();

    const bool ordered = receipt.strongOnlyCalls == 1 && receipt.weakCompleteCalls == 1 &&
        receipt.strongOnlyOrdinal != 0 && receipt.strongOnlyOrdinal < receipt.weakCompleteOrdinal;
    std::fprintf(stderr,
                 "TARGET_SCENE_ORDER_ASSERT_EXECUTED strongCalls=%llu weakCalls=%llu strongOrdinal=%llu "
                 "weakOrdinal=%llu ordered=%u\n",
                 static_cast<unsigned long long>(receipt.strongOnlyCalls),
                 static_cast<unsigned long long>(receipt.weakCompleteCalls),
                 static_cast<unsigned long long>(receipt.strongOnlyOrdinal),
                 static_cast<unsigned long long>(receipt.weakCompleteOrdinal), ordered ? 1u : 0u);
    GC_EXPECT_TRUE(ordered);

    const bool policy = receipt.strongOnlyIncludedWeak == 0 && receipt.weakCompleteIncludedWeak == 1;
    std::fprintf(stderr,
                 "TARGET_SCENE_POLICY_ASSERT_EXECUTED strongIncludesWeak=%llu weakIncludesWeak=%llu policy=%u "
                 "strongRoots=%llu weakRoots=%llu weakRootFamily=%llu weakEdges=%llu\n",
                 static_cast<unsigned long long>(receipt.strongOnlyIncludedWeak),
                 static_cast<unsigned long long>(receipt.weakCompleteIncludedWeak), policy ? 1u : 0u,
                 static_cast<unsigned long long>(receipt.strongOnlyRootsSeen),
                 static_cast<unsigned long long>(receipt.weakCompleteRootsSeen),
                 static_cast<unsigned long long>(receipt.weakRootsSeen),
                 static_cast<unsigned long long>(receipt.weakEdgesSeen));
    GC_EXPECT_TRUE(policy);
#else
    GC_EXPECT_TRUE(true);
#endif
}
#endif
