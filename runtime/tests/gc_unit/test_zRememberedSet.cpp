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

#if defined(MRT_DEBUG) && MRT_DEBUG == 1
// ZGC bitMap.inline.hpp:67 verifies the bitmap index; an uninitialized young
// page has no bitmap storage. Keep that precondition at the bitmap layer.
GC_OTHER_VM_TEST(ZRememberedSetShape, UninitializedPageRememberAsserts)
{
    constexpr const char* scene = "GC_UNIT_REMEMBER_UNINITIALIZED";
    if (std::getenv(scene) != nullptr) {
        std::signal(SIGABRT, SIG_DFL);
        ZPage page(ZPageType::small, PageAge::eden,
                   ZVirtualMemory(to_zoffset(ZPageSizeSmall), ZPageSizeSmall));
        page.remember(reinterpret_cast<volatile zpointer*>(page.GetRegionStart()));
        return;
    }
    GC_EXPECT_EQ(setenv(scene, "1", 1), 0);
    try {
        RunInOtherVm("ZRememberedSetShape.UninitializedPageRememberAsserts", "BitMap index out of bounds");
    } catch (...) {
        unsetenv(scene);
        throw;
    }
    GC_EXPECT_EQ(unsetenv(scene), 0);
}
#endif
