// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Collector/MarkEngine.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(MarkPort203Closure, MarkEndBindsGenerationAndDoesNotRetireForwarding)
{
    MarkDomain young(4, VerifyMarkingStacks::MarkingGeneration::YOUNG);
    young.PrepareWork(1);
    GC_EXPECT_TRUE(young.TryEnd());
    const MarkClosure first = young.NoteMarkComplete();
    GC_EXPECT_TRUE(first.completed);
    GC_EXPECT_EQ(static_cast<int>(first.generation),
                 static_cast<int>(VerifyMarkingStacks::MarkingGeneration::YOUNG));
    GC_EXPECT_EQ(first.seq, 1u);
    GC_EXPECT_EQ(young.LastClosure().seq, 1u);

    MarkDomain old(4, VerifyMarkingStacks::MarkingGeneration::MAJOR);
    old.PrepareWork(1);
    GC_EXPECT_TRUE(old.TryEnd());
    const MarkClosure second = old.NoteMarkComplete();
    GC_EXPECT_TRUE(second.completed);
    GC_EXPECT_EQ(static_cast<int>(second.generation),
                 static_cast<int>(VerifyMarkingStacks::MarkingGeneration::MAJOR));
    GC_EXPECT_EQ(second.seq, 1u);
    GC_EXPECT_NE(first.generation, second.generation);
}

GC_TEST(MarkPort203Closure, IncompleteMarkDoesNotPublishClosure)
{
    MarkDomain domain(2, VerifyMarkingStacks::MarkingGeneration::YOUNG);
    domain.PrepareWork(1);
    MarkThreadLocalStacks seed(2);
    seed.Push(domain.Stripes(), 0, MarkStackEntry::PartialArray(1, 3, false), true);
    (void)seed.Flush(domain.Stripes(), true);
    GC_EXPECT_FALSE(domain.TryEnd());
    GC_EXPECT_FALSE(domain.LastClosure().completed);
    GC_EXPECT_EQ(domain.LastClosure().seq, 0u);
}
