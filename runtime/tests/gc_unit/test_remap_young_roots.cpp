// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Phase 8: ZGenerationOld::remap_young_roots (zGeneration.cpp:1503-1508).
// Remap space is four one-hots; a flip is xor, so a colour published at N is
// load-good again at N+2 unless roots are remapped between young flips.

#include "Heap/WCollector/RemapYoungRoots.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
using namespace MapleRuntime::RemapYoungRootsLogic;

namespace {
constexpr uintptr_t kAddr = 0x00007f00'00001000ULL;
} // namespace

GC_TEST(RemapYoungRoots, IntersectionIsOneHot)
{
    GC_EXPECT_EQ(CurrentRemapBit(kYoungMask0, kOldMask0), ZPointerRemapped00);
    GC_EXPECT_EQ(CurrentRemapBit(kYoungMask1, kOldMask0), ZPointerRemapped01);
    GC_EXPECT_EQ(CurrentRemapBit(kYoungMask0, kOldMask1), ZPointerRemapped10);
    GC_EXPECT_EQ(CurrentRemapBit(kYoungMask1, kOldMask1), ZPointerRemapped11);
}

GC_TEST(RemapYoungRoots, YoungFlipXorWrapsAcceptedPair)
{
    GC_EXPECT_EQ(FlipYoungMask(kYoungMask0), kYoungMask1);
    GC_EXPECT_EQ(FlipYoungMask(kYoungMask1), kYoungMask0);
    GC_EXPECT_EQ(FlipOldMask(kOldMask0), kOldMask1);
}

GC_TEST(RemapYoungRoots, TwoYoungFlipsWithoutRemapMakeOldColourGoodAgain)
{
    const uintptr_t published = kAddr | CurrentRemapBit(kYoungMask0, kOldMask0);
    GC_EXPECT_TRUE(ColourWrapsWithoutRemap(published, kYoungMask0, kOldMask0));
    GC_EXPECT_EQ(Classify(published, kYoungMask0, kOldMask0), Kind::LoadGood);
    GC_EXPECT_EQ(Classify(published, FlipYoungMask(kYoungMask0), kOldMask0), Kind::OldOnlyGood);
    GC_EXPECT_EQ(Classify(published, FlipYoungMask(FlipYoungMask(kYoungMask0)), kOldMask0),
                 Kind::LoadGood);
}

GC_TEST(RemapYoungRoots, Phase8KeepsRootsFromWrappingToGood)
{
    const uintptr_t published = kAddr | CurrentRemapBit(kYoungMask0, kOldMask0);
    GC_EXPECT_TRUE(kEnableRemapYoungRoots);
    GC_EXPECT_TRUE(RootsStayAtMostOneBeatStale(published, kYoungMask0, kOldMask0, true));
    GC_EXPECT_FALSE(RootsStayAtMostOneBeatStale(published, kYoungMask0, kOldMask0, false));
}

GC_TEST(RemapYoungRoots, RemapClearsDoubleBadOnColouredSlot)
{
    const uintptr_t doubleBad = kAddr | ZPointerRemapped11;
    GC_EXPECT_TRUE(IsDoubleRemapBad(doubleBad, kYoungMask0, kOldMask0));
    const uintptr_t healed = RemapToCurrent(doubleBad, kYoungMask0, kOldMask0, true);
    GC_EXPECT_EQ(Classify(healed, kYoungMask0, kOldMask0), Kind::LoadGood);
    GC_EXPECT_FALSE(IsDoubleRemapBad(healed, kYoungMask0, kOldMask0));
}

GC_TEST(RemapYoungRoots, UncolouredStackRootIsNotPainted)
{
    const uintptr_t plain = kAddr;
    GC_EXPECT_EQ(Classify(plain, kYoungMask0, kOldMask0), Kind::Uncoloured);
    GC_EXPECT_EQ(RemapToCurrent(plain, kYoungMask0, kOldMask0, true), plain);
}

GC_TEST(RemapYoungRoots, DisabledPassLeavesDoubleBad)
{
    const uintptr_t doubleBad = kAddr | ZPointerRemapped11;
    GC_EXPECT_EQ(RemapToCurrent(doubleBad, kYoungMask0, kOldMask0, false), doubleBad);
    GC_EXPECT_TRUE(IsDoubleRemapBad(doubleBad, kYoungMask0, kOldMask0));
}
