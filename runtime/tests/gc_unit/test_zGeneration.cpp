#include "Heap/z/zAbort.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zHeap.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(ZGeneration, StaticYoungOldAndId)
{
    Heap::GetHeap();
    GC_EXPECT_TRUE(ZGeneration::young() != nullptr);
    GC_EXPECT_TRUE(ZGeneration::old() != nullptr);
    GC_EXPECT_TRUE(ZGeneration::young()->is_young());
    GC_EXPECT_TRUE(ZGeneration::old()->is_old());
    GC_EXPECT_EQ(static_cast<int>(ZGeneration::young()->id()), static_cast<int>(ZGenerationId::young));
    GC_EXPECT_EQ(static_cast<int>(ZGeneration::old()->id()), static_cast<int>(ZGenerationId::old));
    GC_EXPECT_EQ(ZGeneration::generation(ZGenerationId::young), static_cast<ZGeneration*>(ZGeneration::young()));
    GC_EXPECT_EQ(ZGeneration::generation(ZGenerationId::old), static_cast<ZGeneration*>(ZGeneration::old()));
}

GC_TEST(ZGeneration, ThreeStatePhase)
{
    Heap::GetHeap();
    ZGenerationYoung* young = ZGeneration::young();
    GC_EXPECT_TRUE(young != nullptr);
    young->set_phase(ZGeneration::Phase::Mark);
    GC_EXPECT_TRUE(young->is_phase_mark());
    GC_EXPECT_TRUE(!young->is_phase_mark_complete());
    GC_EXPECT_TRUE(!young->is_phase_relocate());
    young->set_phase(ZGeneration::Phase::MarkComplete);
    GC_EXPECT_TRUE(young->is_phase_mark_complete());
    young->set_phase(ZGeneration::Phase::Relocate);
    GC_EXPECT_TRUE(young->is_phase_relocate());
}

GC_TEST(ZAbort, AllStaticAbortpoint)
{
    GC_EXPECT_TRUE(!ZAbort::should_abort());
    ZAbort::abort();
    GC_EXPECT_TRUE(ZAbort::should_abort());
    ZAbort::reset();
    GC_EXPECT_TRUE(!ZAbort::should_abort());
}
