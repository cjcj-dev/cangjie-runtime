// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Heal coverage before colour reuse (LEAD tagwide2).
// Classifier + inject positive control. Does not walk a live heap.
//
// OpenJDK zGeneration.cpp:1503-1508 remap_young_roots + zAddress.hpp:108-128
// xor wrap: a colour published at beat N is load-good again at N+2.

#include "Common/ColourMask.h"
#include "Heap/Verify/HealCoverage.h"
#include "Heap/WCollector/RemapYoungRoots.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
using HealCoverage::Kind;
using HealCoverage::Face;
using HealCoverage::Counts;
using HealCoverage::Classify;
using HealCoverage::IsCoverageMiss;
using HealCoverage::PaintStale;
using HealCoverage::InjectStaleOnce;
using HealCoverage::CensusWords;
using HealCoverage::FaceOf;
using RemapYoungRootsLogic::CurrentRemapBit;
using RemapYoungRootsLogic::kYoungMask0;
using RemapYoungRootsLogic::kOldMask0;
using RemapYoungRootsLogic::ColourWrapsWithoutRemap;
using RemapYoungRootsLogic::FlipYoungMask;

namespace {
constexpr uintptr_t kAddr = 0x00007f00'00001000ULL;

constexpr uintptr_t LoadBad(uintptr_t remap)
{
    return TAGGED_BITS_MASK | (REMAP_COLOUR_MASK ^ remap);
}
} // namespace

GC_TEST(HealCoverage, NullAndPlainAreNotMisses)
{
    const uintptr_t remap = CurrentRemapBit(kYoungMask0, kOldMask0);
    const uintptr_t bad = LoadBad(remap);
    GC_EXPECT_EQ(Classify(0, bad), Kind::Null);
    GC_EXPECT_EQ(Classify(kAddr, bad), Kind::Plain);
    GC_EXPECT_FALSE(IsCoverageMiss(Classify(0, bad)));
    GC_EXPECT_FALSE(IsCoverageMiss(Classify(kAddr, bad)));
}

GC_TEST(HealCoverage, CurrentColourIsLoadGood)
{
    const uintptr_t remap = CurrentRemapBit(kYoungMask0, kOldMask0);
    const uintptr_t word = kAddr | remap;
    GC_EXPECT_EQ(Classify(word, LoadBad(remap)), Kind::LoadGood);
}

GC_TEST(HealCoverage, PreviousYoungColourIsStaleBeforeWrap)
{
    const uintptr_t remap = CurrentRemapBit(kYoungMask0, kOldMask0);
    const uintptr_t stale = PaintStale(kAddr, remap);
    GC_EXPECT_EQ(Classify(stale, LoadBad(remap)), Kind::Stale);
    GC_EXPECT_TRUE(IsCoverageMiss(Classify(stale, LoadBad(remap))));
}

GC_TEST(HealCoverage, InjectPositiveControlRings)
{
    uintptr_t slot = kAddr | CurrentRemapBit(kYoungMask0, kOldMask0);
    const uintptr_t remap = CurrentRemapBit(kYoungMask0, kOldMask0);
    InjectStaleOnce(&slot, remap);
    const uintptr_t words[] = { slot };
    const Counts c = CensusWords(words, 1, LoadBad(remap));
    GC_EXPECT_EQ(c.stale, 1u);
    GC_EXPECT_EQ(c.loadGood, 0u);
}

GC_TEST(HealCoverage, TwoYoungFlipsWrapStaleIntoLookingGood)
{
    const uintptr_t remap0 = CurrentRemapBit(kYoungMask0, kOldMask0);
    const uintptr_t published = kAddr | remap0;
    GC_EXPECT_TRUE(ColourWrapsWithoutRemap(published, kYoungMask0, kOldMask0));
    const uintptr_t y2 = FlipYoungMask(FlipYoungMask(kYoungMask0));
    const uintptr_t remap2 = CurrentRemapBit(y2, kOldMask0);
    GC_EXPECT_EQ(Classify(published, LoadBad(remap2)), Kind::LoadGood);
}

GC_TEST(HealCoverage, FaceOfSplitsTheFiveHealSurfaces)
{
    GC_EXPECT_EQ(static_cast<uint8_t>(FaceOf(true, false, false, true)),
                 static_cast<uint8_t>(Face::YoungHolder));
    GC_EXPECT_EQ(static_cast<uint8_t>(FaceOf(false, false, false, true)),
                 static_cast<uint8_t>(Face::OldToYoung));
    GC_EXPECT_EQ(static_cast<uint8_t>(FaceOf(false, false, false, false)),
                 static_cast<uint8_t>(Face::OldToOld));
    GC_EXPECT_EQ(static_cast<uint8_t>(FaceOf(false, true, false, true)),
                 static_cast<uint8_t>(Face::PinnedHolder));
    GC_EXPECT_EQ(static_cast<uint8_t>(FaceOf(true, false, true, true)),
                 static_cast<uint8_t>(Face::FromHolder));
}

GC_TEST(HealCoverage, CensusWordsCountsEachKind)
{
    const uintptr_t remap = CurrentRemapBit(kYoungMask0, kOldMask0);
    const uintptr_t bad = LoadBad(remap);
    const uintptr_t words[] = {
        0,
        kAddr,
        kAddr | remap,
        PaintStale(kAddr, remap),
    };
    const Counts c = CensusWords(words, 4, bad);
    GC_EXPECT_EQ(c.nulls, 1u);
    GC_EXPECT_EQ(c.plains, 1u);
    GC_EXPECT_EQ(c.loadGood, 1u);
    GC_EXPECT_EQ(c.stale, 1u);
}
