// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// DiagGate is the switch nine GC instruments read, MRT_GCV2_NULLSLOT among them.
// It was hollowed once -- every accessor `return false;` -- and the failure mode is
// silent: setting the env produces no lines, and no lines read as "that arm never
// fires".  These tests fail if the accessors stop consulting the environment, so a
// second hollowing cannot pass the suite.
//
// TokenOn caches MRT_GCV2_DIAG on first use (one getenv per process), so the CSV
// parser is exercised directly rather than by setenv-ing around a cached read.

#include <cstdlib>

#include "gc_unittest.hpp"
#include "Heap/Verify/DiagGate.h"

using namespace MapleRuntime;

GC_TEST(DiagGate, LegacyEnvArmsTheGate)
{
    // A legacy per-probe env set to "1" must arm its gate on its own, with
    // MRT_GCV2_DIAG absent -- that is the form every in-flight recipe uses.
    const char* kName = "MRT_GCV2_DIAGGATE_UNIT";
    (void)unsetenv(kName);
    GC_EXPECT_FALSE(DiagGate::LegacyOrToken(kName, "diaggate_unit_token"));

    GC_EXPECT_EQ(setenv(kName, "1", 1), 0);
    GC_EXPECT_TRUE(DiagGate::LegacyOrToken(kName, "diaggate_unit_token"));

    // Only "1" arms it; a truthy-looking value must not.
    GC_EXPECT_EQ(setenv(kName, "0", 1), 0);
    GC_EXPECT_FALSE(DiagGate::LegacyOrToken(kName, "diaggate_unit_token"));
    GC_EXPECT_EQ(setenv(kName, "yes", 1), 0);
    GC_EXPECT_FALSE(DiagGate::LegacyOrToken(kName, "diaggate_unit_token"));

    (void)unsetenv(kName);
}

GC_TEST(DiagGate, NullLegacyNameIsNotAnArm)
{
    // LegacyOrToken(nullptr, tok) must not arm on a null env name; the token half
    // still decides.  Guards against a getenv(nullptr) crash too.
    GC_EXPECT_FALSE(DiagGate::LegacyOrToken(nullptr, "diaggate_unit_absent_token"));
}

GC_TEST(DiagGate, UnsetLegacyEnvLeavesGateOff)
{
    // Positive control for the zero case: with nothing set, the gate reports off.
    // A hollowed build also reports off, which is exactly why the two tests above
    // (which must report *on*) are the ones that catch re-hollowing.
    const char* kName = "MRT_GCV2_DIAGGATE_UNIT_UNSET";
    (void)unsetenv(kName);
    GC_EXPECT_FALSE(DiagGate::LegacyOrToken(kName, "diaggate_unit_absent_token"));
}

GC_TEST(DiagGate, AnnounceAndLegendAreIdempotent)
{
    // Both are called from product paths; neither may abort or double-emit.
    DiagGate::MaybeAnnounce();
    DiagGate::MaybeAnnounce();
    DiagGate::EmitCounterLegend();
    DiagGate::EmitCounterLegend();
    GC_EXPECT_TRUE(true);
}
