// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Collector/Collector.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(MarkPort203Closure, MarkEndUsesGenerationSeqAndPhase)
{
    GenerationCycle young(GCCycleGeneration::YOUNG);
    young.Begin(1);
    young.PublishPhase(GC_PHASE_ENUM);
    GC_EXPECT_NE(static_cast<int>(young.Phase()), static_cast<int>(GC_PHASE_MARK_COMPLETE));
    const uint64_t seq = young.Snapshot().sequence;
    GC_EXPECT_EQ(seq, 1u);

    young.PublishPhase(GC_PHASE_MARK_COMPLETE);
    GC_EXPECT_EQ(static_cast<int>(young.Phase()), static_cast<int>(GC_PHASE_MARK_COMPLETE));
    GC_EXPECT_EQ(young.Snapshot().sequence, seq);
}

GC_TEST(MarkPort203Closure, MarkStartClearsCompleteOnSameGeneration)
{
    GenerationCycle young(GCCycleGeneration::YOUNG);
    young.Begin(1);
    young.PublishPhase(GC_PHASE_MARK_COMPLETE);
    young.End();

    young.Begin(2);
    young.PublishPhase(GC_PHASE_ENUM);
    GC_EXPECT_NE(static_cast<int>(young.Phase()), static_cast<int>(GC_PHASE_MARK_COMPLETE));
    GC_EXPECT_EQ(young.Snapshot().sequence, 2u);
}

GC_TEST(MarkPort203Closure, OldAndYoungClosuresAreIndependent)
{
    GenerationCycle young(GCCycleGeneration::YOUNG);
    GenerationCycle old(GCCycleGeneration::OLD);
    young.Begin(1);
    old.Begin(1);
    young.PublishPhase(GC_PHASE_MARK_COMPLETE);
    old.PublishPhase(GC_PHASE_ENUM);
    GC_EXPECT_EQ(static_cast<int>(young.Phase()), static_cast<int>(GC_PHASE_MARK_COMPLETE));
    GC_EXPECT_NE(static_cast<int>(old.Phase()), static_cast<int>(GC_PHASE_MARK_COMPLETE));
}
