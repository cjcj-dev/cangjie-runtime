#include "Heap/z/zAbort.hpp"
#include "Heap/z/zCollectedHeap.hpp"
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

// ZGC zGeneration.cpp:499-505 and zRemembered.cpp:347-355: the
// remembered set is ready when generation construction returns.
GC_TEST(RememberedLifecycle720, ConstructedGenerationPublishesHighestGranule)
{
    Heap::GetHeap();
    ZPageTable pages;
    RegionManager allocator;
    ZGenerationOld old;
    ZGenerationYoung young(&pages, &old.forwarding_table(), &allocator);
    const size_t last = ZAddressOffsetMax - ZGranuleSize;
    ZPage page(ZPageType::large, PageAge::old,
               ZVirtualMemory(static_cast<zoffset>(last), ZGranuleSize));
    GC_EXPECT_EQ(pages.map().size(), ZAddressOffsetMax >> ZGranuleSizeShift);
    pages.insert(&page);
    ZRemsetTableIterator iter(young.remembered(), false);
    ZRemsetTableEntry entry{};
    const bool found = iter.next(&entry);
    std::fprintf(stderr, "REMEMBERED720 highest=%zu found=%d page_matches=%d\n",
                 last, found, entry._page == &page);
    GC_EXPECT_TRUE(found && entry._page == &page);
    GC_EXPECT_TRUE(!iter.next(&entry));
    pages.remove(&page);
}

GC_TEST(RememberedLifecycle720, HeapPublicationReachesConstructedRemembered)
{
    auto& heap = Heap::GetHeap();
    const size_t last = ZAddressOffsetMax - ZGranuleSize;
    ZPage page(ZPageType::large, PageAge::old,
               ZVirtualMemory(static_cast<zoffset>(last), ZGranuleSize));
    Heap::alloc_page(&page);
    ZRemsetTableIterator iter(heap.young().remembered(), false);
    ZRemsetTableEntry entry{};
    bool found = false;
    while (iter.next(&entry)) {
        found |= entry._page == &page;
    }
    std::fprintf(stderr, "REMEMBERED720 heap_publication_found=%d\n", found);
    GC_EXPECT_TRUE(found);
    Heap::page_table().remove(&page);
}
