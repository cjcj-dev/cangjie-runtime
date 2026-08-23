// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Classifier for MarkCompleteVerify's deadInterior arm. Four named kinds,
// mutually exclusive and exhaustive. No heap: the decision is a pure function
// of the four evidence bits AccountInteriorKind will fill in on the live path.

#include "Heap/Verify/InteriorEdgeClass.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
using InteriorEdgeClass::Kind;
using InteriorEdgeClass::Classify;
using InteriorEdgeClass::KindName;

GC_TEST(InteriorEdgeClass, SlotNotRefWinsOverEverything)
{
    // A non-ref word walked as a ref is an instrument-calibre false positive
    // even if the bits also look like an interior into a live host.
    GC_EXPECT_TRUE(Classify(false, true, true, true, true, false) == Kind::SlotNotRef);
    GC_EXPECT_TRUE(Classify(false, false, false, false, false, true) == Kind::SlotNotRef);
}

GC_TEST(InteriorEdgeClass, InStreamLiveIsRecoverFail)
{
    // Size-walk found a live containing object. The existing okInteriorBase
    // arm would have taken this if TryRecoverInteriorBase had named that host.
    GC_EXPECT_TRUE(Classify(true, true, true, false, false, false) == Kind::RecoverFail);
    GC_EXPECT_TRUE(Classify(true, true, true, true, true, false) == Kind::RecoverFail);
}

GC_TEST(InteriorEdgeClass, InStreamDeadIsBaseUnmarked)
{
    // Containing object exists and is unmarked: same shape as the deadFrom
    // main case, counted here so survnode can join it without mixing in FPs.
    GC_EXPECT_TRUE(Classify(true, true, false, false, false, false) == Kind::BaseUnmarked);
}

GC_TEST(InteriorEdgeClass, RecoverWithoutStream)
{
    // Heuristic found a host the size-walk missed (walk truncated, or
    // VisitAllObjects broke). Live host = recover miss vs the gate; dead host
    // = base unmarked. Must not fall into ValueCorrupt.
    GC_EXPECT_TRUE(Classify(true, false, false, true, true, true) == Kind::RecoverFail);
    GC_EXPECT_TRUE(Classify(true, false, false, true, false, true) == Kind::BaseUnmarked);
}

GC_TEST(InteriorEdgeClass, TruncatedWalkIsNotValueCorrupt)
{
    // Absence of a containing object is unproven if the walk broke. Refuse
    // to call the bits "corrupt".
    GC_EXPECT_TRUE(Classify(true, false, false, false, false, true) == Kind::RecoverFail);
}

GC_TEST(InteriorEdgeClass, ValueCorruptIsTheResidual)
{
    GC_EXPECT_TRUE(Classify(true, false, false, false, false, false) == Kind::ValueCorrupt);
}

GC_TEST(InteriorEdgeClass, FourKindsExhaustiveAndNamed)
{
    Kind seen[4] = { Kind::SlotNotRef, Kind::RecoverFail, Kind::BaseUnmarked, Kind::ValueCorrupt };
    const char* names[4] = { "slotNotRef", "recoverFail", "baseUnmarked", "valueCorrupt" };
    for (int i = 0; i < 4; ++i) {
        GC_EXPECT_EQ(std::strcmp(KindName(seen[i]), names[i]), 0);
    }
    // Every boolean combination lands on one of the four; none is unknown.
    int counts[4] = {};
    for (int bits = 0; bits < 64; ++bits) {
        Kind k = Classify((bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0, (bits & 16) != 0,
                          (bits & 32) != 0);
        counts[static_cast<int>(k)]++;
    }
    int sum = counts[0] + counts[1] + counts[2] + counts[3];
    GC_EXPECT_EQ(sum, 64);
    GC_EXPECT_TRUE(counts[0] > 0 && counts[1] > 0 && counts[2] > 0 && counts[3] > 0);
}
