// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zPageAge.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(PageAgeRangeTest, test)
{
    PageAgeRange rangeEden = kPageAgeRangeEden;
    GC_EXPECT_EQ(untype(rangeEden.first()), untype(PageAge::eden));
    GC_EXPECT_EQ(untype(rangeEden.last()), untype(PageAge::eden));

    PageAgeRange rangeYoung = kPageAgeRangeYoung;
    GC_EXPECT_EQ(untype(rangeYoung.first()), untype(PageAge::eden));
    GC_EXPECT_EQ(untype(rangeYoung.last()), untype(PageAge::survivor14));

    PageAgeRange rangeSurvivor = kPageAgeRangeSurvivor;
    GC_EXPECT_EQ(untype(rangeSurvivor.first()), untype(PageAge::survivor1));
    GC_EXPECT_EQ(untype(rangeSurvivor.last()), untype(PageAge::survivor14));

    PageAgeRange rangeRelocation = kPageAgeRangeRelocation;
    GC_EXPECT_EQ(untype(rangeRelocation.first()), untype(PageAge::survivor1));
    GC_EXPECT_EQ(untype(rangeRelocation.last()), untype(PageAge::old));

    PageAgeRange rangeOld = kPageAgeRangeOld;
    GC_EXPECT_EQ(untype(rangeOld.first()), untype(PageAge::old));
    GC_EXPECT_EQ(untype(rangeOld.last()), untype(PageAge::old));

    PageAgeRange rangeAll = kPageAgeRangeAll;
    GC_EXPECT_EQ(untype(rangeAll.first()), untype(PageAge::eden));
    GC_EXPECT_EQ(untype(rangeAll.last()), untype(PageAge::old));
}

GC_TEST(PageAge, UntypeRoundTrip)
{
    GC_EXPECT_EQ(untype(PageAge::eden), 0u);
    GC_EXPECT_EQ(untype(PageAge::old), 15u);
    GC_EXPECT_EQ(untype(PageAge::eden + 1), untype(PageAge::survivor1));
    GC_EXPECT_EQ(untype(PageAge::survivor2 - 1), untype(PageAge::survivor1));
}

GC_TEST(TenuringThreshold, PromoteAllIsZero)
{
    TenuringInputs in;
    in.promoteAll = true;
    in.liveByAge[0] = 64 * 1024 * 1024;
    in.softMaxCapacity = 1024 * 1024 * 1024;
    GC_EXPECT_EQ(ComputeTenuringThreshold(in), 0u);
}

GC_TEST(TenuringThreshold, EmptyYoungIsZero)
{
    TenuringInputs in;
    in.softMaxCapacity = 1024 * 1024 * 1024;
    GC_EXPECT_EQ(ComputeTenuringThreshold(in), 0u);
}

GC_TEST(TenuringThreshold, GenerationalRaisesThreshold)
{
    TenuringInputs in;
    in.liveByAge[0] = 100 * 1024 * 1024;
    in.liveByAge[1] = 20 * 1024 * 1024;
    in.liveByAge[2] = 4 * 1024 * 1024;
    in.youngGarbage = 80 * 1024 * 1024;
    in.youngAllocated = 10 * 1024 * 1024;
    in.softMaxCapacity = 1024 * 1024 * 1024;
    const uint32_t thr = ComputeTenuringThreshold(in);
    GC_EXPECT_EQ(thr, 3u);
}

GC_TEST(TenuringThreshold, PressureLowersThreshold)
{
    TenuringInputs in;
    in.liveByAge[0] = 200 * 1024 * 1024;
    in.liveByAge[1] = 200 * 1024 * 1024;
    in.youngGarbage = 10 * 1024 * 1024;
    in.youngAllocated = 200 * 1024 * 1024;
    in.softMaxCapacity = 256 * 1024 * 1024;
    const uint32_t thr = ComputeTenuringThreshold(in);
    GC_EXPECT_EQ(thr, 1u);
}

GC_TEST(TenuringThreshold, DistributionFlipBothDirections)
{
    TenuringInputs gen;
    gen.liveByAge[0] = 100 * 1024 * 1024;
    gen.liveByAge[1] = 20 * 1024 * 1024;
    gen.liveByAge[2] = 4 * 1024 * 1024;
    gen.youngGarbage = 80 * 1024 * 1024;
    gen.youngAllocated = 10 * 1024 * 1024;
    gen.softMaxCapacity = 1024 * 1024 * 1024;

    TenuringInputs press;
    press.liveByAge[0] = 200 * 1024 * 1024;
    press.liveByAge[1] = 200 * 1024 * 1024;
    press.youngGarbage = 10 * 1024 * 1024;
    press.youngAllocated = 200 * 1024 * 1024;
    press.softMaxCapacity = 256 * 1024 * 1024;

    const uint32_t up = ComputeTenuringThreshold(gen);
    const uint32_t down = ComputeTenuringThreshold(press);
    GC_EXPECT_EQ(up, 3u);
    GC_EXPECT_EQ(down, 1u);
    GC_EXPECT_TRUE(up > down);
}

GC_TEST(TenuringThreshold, ComputeToAge)
{
    GC_EXPECT_EQ(untype(ComputeToAge(PageAge::old, 3)), untype(PageAge::old));
    GC_EXPECT_EQ(untype(ComputeToAge(PageAge::eden, 0)), untype(PageAge::old));
    GC_EXPECT_EQ(untype(ComputeToAge(PageAge::eden, 2)), untype(PageAge::survivor1));
    GC_EXPECT_EQ(untype(ComputeToAge(PageAge::survivor1, 1)), untype(PageAge::old));
    GC_EXPECT_TRUE(ShouldPromoteAge(0, 0));
    GC_EXPECT_FALSE(ShouldPromoteAge(0, 2));
}

#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/zVirtualMemory.inline.hpp"

// ZGC zPage.inline.hpp:359-369: current and previous are direct bitmap reads.
GC_TEST(PageRemset1272, InitializedReadFaces)
{
    ZPage page(ZPageType::small, PageAge::old,
               ZVirtualMemory(to_zoffset(ZPageSizeSmall), ZPageSizeSmall));
    auto* slot = reinterpret_cast<volatile zpointer*>(page.GetRegionStart());
    GC_EXPECT_FALSE(page.is_remembered(slot));
    GC_EXPECT_FALSE(page.was_remembered(slot));
    page.remember(slot);
    GC_EXPECT_TRUE(page.is_remembered(slot));
    GC_EXPECT_FALSE(page.was_remembered(slot));
    page.swap_remset_bitmaps();
    GC_EXPECT_FALSE(page.is_remembered(slot));
    GC_EXPECT_TRUE(page.was_remembered(slot));
    std::fprintf(stderr, "REMSET1272_TARGET current=0 previous=1 after_swap\n");
}

#if defined(MRT_DEBUG) && MRT_DEBUG == 1
#include "Heap/z/zLiveMap.inline.hpp"
#include "gc_cycle_sequence_fixture.hpp"
namespace {
// Header-only product methods are compiled with the same MRT_DEBUG setting
// as the linked runtime. The child must match the exact product diagnostic.
void CheckRemsetPrecondition(const char* test, bool previous, bool live,
                             const char* diagnostic = "BitMap index out of bounds",
                             bool advance = false, bool mark = false)
{
    if (std::getenv("REMSET1272_SCENE") == nullptr) {
        GC_EXPECT_EQ(setenv("REMSET1272_SCENE", "1", 1), 0);
        try {
            RunInOtherVm(test, diagnostic);
        } catch (...) {
            unsetenv("REMSET1272_SCENE");
            throw;
        }
        GC_EXPECT_EQ(unsetenv("REMSET1272_SCENE"), 0);
        std::fprintf(stderr, "REMSET1272_TARGET %s diagnostic_matched=1\n", test);
        return;
    }
    ZPage page(ZPageType::small, live ? PageAge::old : PageAge::eden,
               ZVirtualMemory(to_zoffset(ZPageSizeSmall), ZPageSizeSmall));
    auto* slot = reinterpret_cast<volatile zpointer*>(page.GetRegionStart());
    if (live) {
        if (advance) {
            // This fixture supplies an older page, not a mark-start acceptance
            // test. The preconditions under test remain the product header.
            GenerationSequenceFixture::Advance(*ZGeneration::old());
            GC_EXPECT_FALSE(page.is_allocating());
            if (mark) {
                ZGeneration::old()->set_phase(ZGenerationPhase::Mark);
                bool increment = false;
                page.livemap().set(ZGenerationId::old, 0, false, increment);
                GC_EXPECT_TRUE(page.is_marked());
                GC_EXPECT_TRUE(ZGeneration::old()->is_phase_mark());
            } else {
                ZGeneration::old()->set_phase(ZGenerationPhase::MarkComplete);
                GC_EXPECT_FALSE(ZGeneration::old()->is_phase_mark());
                GC_EXPECT_FALSE(page.is_marked());
            }
        }
        std::fprintf(stderr, "REMSET1272_ENTER test=%s allocating=%d phase_mark=%d\n",
                     test, page.is_allocating(), ZGeneration::old()->is_phase_mark());
        page.oops_do_remembered_in_live([](volatile zpointer*) {});
    } else {
        const bool result = previous ? page.was_remembered(slot) : page.is_remembered(slot);
        std::fprintf(stderr, "REMSET1272_UNEXPECTED_RETURN value=%d\n", result);
    }
}
}

GC_OTHER_VM_TEST(PageRemset1272, UninitializedCurrentRejected)
{
    CheckRemsetPrecondition("PageRemset1272.UninitializedCurrentRejected", false, false);
}

GC_OTHER_VM_TEST(PageRemset1272, UninitializedPreviousRejected)
{
    CheckRemsetPrecondition("PageRemset1272.UninitializedPreviousRejected", true, false);
}

// ZGC zPage.inline.hpp:406-408: a newly allocated page has no scan liveness.
GC_OTHER_VM_TEST(PageRemset1272, AllocatingLiveScanRejected)
{
    CheckRemsetPrecondition("PageRemset1272.AllocatingLiveScanRejected", false, true,
                             "Check failed: !is_allocating()");
}

GC_OTHER_VM_TEST(PageRemset1272, MarkPhaseLiveScanRejected)
{
    CheckRemsetPrecondition("PageRemset1272.MarkPhaseLiveScanRejected", false, true,
                             "Check failed: !ZGeneration::old()->is_phase_mark()", true, true);
}

GC_OTHER_VM_TEST(PageRemset1272, UnmarkedLiveScanRejected)
{
    CheckRemsetPrecondition("PageRemset1272.UnmarkedLiveScanRejected", false, true,
                             "Check failed: is_marked()", true, false);
}
#endif
