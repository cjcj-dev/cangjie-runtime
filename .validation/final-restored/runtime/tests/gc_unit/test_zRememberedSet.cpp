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
