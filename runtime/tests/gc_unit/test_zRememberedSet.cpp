#include "Heap/z/zRememberedSet.inline.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(ZRememberedSetShape, FlipMovesCurrentToPrevious)
{
    ZRememberedSet remset;
    remset.initialize(4096);
    GC_EXPECT_TRUE(remset.is_initialized());
    GC_EXPECT_TRUE(remset.is_cleared_current());
    GC_EXPECT_TRUE(remset.is_cleared_previous());
    GC_EXPECT_TRUE(remset.set_current(sizeof(RefField<>)));
    GC_EXPECT_TRUE(remset.at_current(sizeof(RefField<>)));
    GC_EXPECT_FALSE(remset.at_previous(sizeof(RefField<>)));
    remset.swap_remset_bitmaps();
    GC_EXPECT_TRUE(remset.is_cleared_current());
    GC_EXPECT_TRUE(remset.at_previous(sizeof(RefField<>)));
}

#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/zVirtualMemory.inline.hpp"

GC_TEST(ZRememberedSetShape, PageRememberPublishesCurrentBit)
{
    ZPage page(ZPageType::small, PageAge::old,
               ZVirtualMemory(to_zoffset(ZPageSizeSmall), ZPageSizeSmall));
    auto* field = reinterpret_cast<volatile zpointer*>(page.GetRegionStart() + sizeof(zpointer));
    GC_EXPECT_FALSE(page.is_remembered(field));
    page.remember(field);
    const bool remembered = page.is_remembered(field);
    std::fprintf(stderr, "PAGE_REMEMBER_CURRENT_ASSERT remembered=%d\n", remembered);
    GC_EXPECT_TRUE(remembered);
    GC_EXPECT_FALSE(page.was_remembered(field));
}
