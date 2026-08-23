// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Unit suite for the ZStatPhase-family port (Base/ZStat.h, zStat.hpp:212-342).
// The property under test is the one the campaign keeps getting wrong by hand:
// a phase duration must be booked under exactly one of pause/concurrent, and the
// two sums must never be mixed. Every assertion below is constructed so a real
// defect turns it red -- each test names the sabotage it catches.

#include "gc_unittest.hpp"
#include "Base/ZStat.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

struct ZStatScope {
    ZStatScope()
    {
        ZStat::SetEnabledForTest(true);
        ZStat::ResetForTest();
    }
    ~ZStatScope()
    {
        ZStat::ResetForTest();
        ZStat::SetEnabledForTest(false);
    }
};

GC_TEST(ZStat, PauseAndConcurrentAreSeparateAccounts)
{
    ZStatScope scope;
    // Sabotage caught: booking everything into one bucket (or into rec=cycle dur_ns).
    ZStat::NotePhase("young.copy", true, 1000);
    ZStat::NotePhase("young.copy", false, 500);
    ZStat::PhaseTotals totals = ZStat::Phase("young.copy");
    GC_EXPECT_EQ(totals.pauseNs, 1000ULL);
    GC_EXPECT_EQ(totals.concNs, 500ULL);
    GC_EXPECT_EQ(totals.nPause, 1U);
    GC_EXPECT_EQ(totals.nConc, 1U);
    GC_EXPECT_EQ(ZStat::CyclePauseNs(), 1000ULL);
    GC_EXPECT_EQ(ZStat::CycleConcNs(), 500ULL);
    // Invariant: cycle total is exactly pause + concurrent, never more, never less.
    GC_EXPECT_EQ(ZStat::CyclePauseNs() + ZStat::CycleConcNs(), 1500ULL);
}

GC_TEST(ZStat, KindIsSampledAtScopeEntry)
{
    ZStatScope scope;
    // The same name on both sides of a world-release must split into two samples;
    // a static per-name kind (the ZGC shape) would misclassify one of them.
    ZStat::NotePhase("young.ref_fix_bulk", true, 200);
    ZStat::NotePhase("young.ref_fix_bulk", false, 800);
    ZStat::PhaseTotals totals = ZStat::Phase("young.ref_fix_bulk");
    GC_EXPECT_EQ(totals.pauseNs, 200ULL);
    GC_EXPECT_EQ(totals.concNs, 800ULL);
}

GC_TEST(ZStat, MaxPauseTracksLargestPauseSample)
{
    ZStatScope scope;
    // Sabotage caught: max tracker fed by concurrent samples (or never updated -> 恒0).
    ZStat::NotePhase("young.mark_closure", true, 300);
    ZStat::NotePhase("young.mark_closure", true, 700);
    ZStat::NotePhase("young.concurrent_relocate", false, 9999);
    GC_EXPECT_EQ(ZStat::CycleMaxPauseNs(), 700ULL);
    GC_EXPECT_EQ(ZStat::Phase("young.mark_closure").maxPauseNs, 700ULL);
}

GC_TEST(ZStat, RegistryEnumeratesObservedPhases)
{
    ZStatScope scope;
    // Sabotage caught: registration on first sight dropped -> "which phases exist" is
    // no longer an enumerable fact (the ZStatValue property, zStat.hpp:66).
    ZStat::NotePhase("young.remset_drain", false, 100);
    ZStat::NotePhase("young.evac_finish", true, 50);
    std::vector<std::string> names = ZStat::RegisteredPhases();
    GC_EXPECT_EQ(names.size(), 2U);
    GC_EXPECT_EQ(names[0], std::string("young.evac_finish")); // sorted for stable diffs
    GC_EXPECT_EQ(names[1], std::string("young.remset_drain"));
}

GC_TEST(ZStat, StwDepthCounterClassifies)
{
    ZStatScope scope;
    GC_EXPECT_EQ(ZStat::WorldStoppedNow(), false);
    ZStat::EnterStwScope();
    GC_EXPECT_EQ(ZStat::WorldStoppedNow(), true);
    ZStat::EnterStwScope(); // nested STW scopes must still read as stopped
    ZStat::ExitStwScope();
    GC_EXPECT_EQ(ZStat::WorldStoppedNow(), true);
    ZStat::ExitStwScope();
    GC_EXPECT_EQ(ZStat::WorldStoppedNow(), false);
}

GC_TEST(ZStat, ZeroDurationSampleStillCounts)
{
    ZStatScope scope;
    // A 0ns phase is a measurement, not an absence: n must advance so a consumer can
    // distinguish "phase ran in 0ns" from "phase never ran" (恒0 前科族).
    ZStat::NotePhase("young.pinned_scan", true, 0);
    ZStat::PhaseTotals totals = ZStat::Phase("young.pinned_scan");
    GC_EXPECT_EQ(totals.nPause, 1U);
    GC_EXPECT_EQ(totals.pauseNs, 0ULL);
}

} // namespace
