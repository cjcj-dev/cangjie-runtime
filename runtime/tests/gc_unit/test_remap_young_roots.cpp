// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Phase 8: ZGenerationOld::remap_young_roots (zGeneration.cpp:1503-1508).
// Remap space is four one-hots; a flip is xor, so a colour published at N is
// load-good again at N+2 unless roots are remapped between young flips.

#include "UnwindStack/StackWatermark.h"
#include "Heap/Collector/PromotedRegionDomain.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(PromotedRegionDomain, ResidualPromotionRequiresClosedLiveness)
{
    GC_EXPECT_TRUE(PromotedRegionDomain::ResidualPromotionHasClosedLiveness(false, true, true));
    GC_EXPECT_FALSE(PromotedRegionDomain::ResidualPromotionHasClosedLiveness(true, true, true));
    GC_EXPECT_FALSE(PromotedRegionDomain::ResidualPromotionHasClosedLiveness(false, false, true));
    GC_EXPECT_FALSE(PromotedRegionDomain::ResidualPromotionHasClosedLiveness(false, true, false));
}

GC_TEST(PromotedRegionDomain, YoungPromotionDefersItsOnlyFieldScan)
{
    GC_EXPECT_TRUE(PromotedRegionDomain::DeferPromotedFieldScan(true));
    GC_EXPECT_FALSE(PromotedRegionDomain::DeferPromotedFieldScan(false));
}
// zStackWatermark.cpp: start_processing_impl/process; mark and remap are
// separate processing phases, even when their generation counters coincide.
GC_TEST(StackWatermark, MarkCompletionDoesNotCompleteRemap)
{
    StackWatermark watermark;
    using P = StackWatermark::ProcessingPhase;
    GC_EXPECT_TRUE(watermark.TryBegin(7, StackWatermark::WM_OWNER_SELF, 1, P::MARK));
    watermark.AdvanceTo(1, StackWatermark::WM_OWNER_SELF);
    watermark.Finish(StackWatermark::WM_OWNER_SELF);
    GC_EXPECT_TRUE(watermark.IsDone(7, P::MARK));
    GC_EXPECT_FALSE(watermark.IsDone(7, P::REMAP));
    GC_EXPECT_TRUE(watermark.TryBegin(7, StackWatermark::WM_OWNER_SELF, 1, P::REMAP));
    GC_EXPECT_FALSE(watermark.IsDone(7, P::REMAP));
    watermark.AdvanceTo(1, StackWatermark::WM_OWNER_SELF);
    watermark.Finish(StackWatermark::WM_OWNER_SELF);
    GC_EXPECT_TRUE(watermark.IsDone(7, P::REMAP));
    GC_EXPECT_FALSE(watermark.IsDone(7, P::MARK));
}

GC_TEST(StackWatermark, RemapRetainsLogicalStackIdentityAcrossGrow)
{
    StackWatermark watermark;
    using P = StackWatermark::ProcessingPhase;
    GC_EXPECT_TRUE(watermark.TryBegin(9, StackWatermark::WM_OWNER_SELF, 2, P::REMAP));
    watermark.AdvanceTo(1, StackWatermark::WM_OWNER_SELF);
    watermark.OnStackGrow(4096);
    GC_EXPECT_EQ(watermark.GetCursorIndex(), size_t(1));
    GC_EXPECT_EQ(watermark.GetFrameCount(), size_t(2));
    GC_EXPECT_FALSE(watermark.IsDone(9, P::REMAP));
    watermark.AdvanceTo(2, StackWatermark::WM_OWNER_SELF);
    watermark.Finish(StackWatermark::WM_OWNER_SELF);
    GC_EXPECT_TRUE(watermark.IsDone(9, P::REMAP));
}
