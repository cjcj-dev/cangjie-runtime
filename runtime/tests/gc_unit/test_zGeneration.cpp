#include "Heap/z/zAbort.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zJNICritical.hpp"
#include "gc_unittest.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

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
    GC_EXPECT_TRUE(ZGeneration::generation(ZGenerationId::young) == static_cast<ZGeneration*>(ZGeneration::young()));
    GC_EXPECT_TRUE(ZGeneration::generation(ZGenerationId::old) == static_cast<ZGeneration*>(ZGeneration::old()));
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

GC_TEST(ZCollectedHeap, StopAborts)
{
    Heap::GetHeap();
    ZAbort::reset();
    ZCollectedHeap::stop();
    GC_EXPECT_TRUE(ZAbort::should_abort());
    ZAbort::reset();
}

GC_TEST(ZGeneration, CollectionScopeClearsTimer)
{
    Heap::GetHeap();
    ZGenerationYoung* young = ZGeneration::young();
    young->at_collection_start(young);
    GC_EXPECT_TRUE(young->gc_timer() == young);
    young->at_collection_end();
    GC_EXPECT_TRUE(young->gc_timer() == nullptr);
}

GC_TEST(ZGeneration, FreedPromotedCompactedAtomics)
{
    Heap::GetHeap();
    ZGenerationYoung* young = ZGeneration::young();
    young->reset_statistics();
    GC_EXPECT_EQ(young->freed(), static_cast<size_t>(0));
    young->increase_freed(16);
    young->increase_promoted(8);
    young->increase_compacted(4);
    GC_EXPECT_EQ(young->freed(), static_cast<size_t>(16));
    GC_EXPECT_EQ(young->promoted(), static_cast<size_t>(8));
    GC_EXPECT_EQ(young->compacted(), static_cast<size_t>(4));
    young->reset_statistics();
}

GC_TEST(ZJNICritical, PauseSeesBlockedCount)
{
    Heap::GetHeap();
    const bool ok = ZGeneration::TestPauseJniCritical();
    std::printf("ZJNI_CRITICAL_PAUSE_SAW_BLOCKED ok=%d\n", ok ? 1 : 0);
    GC_EXPECT_TRUE(ok);
}

GC_TEST(ZJNICritical, BlockWaitsWhileEntered)
{
    ZJNICritical::initialize();
    ZJNICritical::enter();
    std::atomic<bool> finished{ false };
    std::thread waiter([&] {
        ZJNICritical::block();
        finished.store(true, std::memory_order_release);
        ZJNICritical::unblock();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const bool relocatedWhileEntered = finished.load(std::memory_order_acquire);
    std::printf("ZJNI_CRITICAL_NO_RELOCATE_WHILE_ENTERED finished=%d count=%lld\n",
                relocatedWhileEntered ? 1 : 0,
                static_cast<long long>(ZJNICritical::count_snapshot()));
    GC_EXPECT_FALSE(relocatedWhileEntered);
    ZJNICritical::exit();
    waiter.join();
    GC_EXPECT_TRUE(finished.load(std::memory_order_acquire));
}
