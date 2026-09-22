#include "gc_unittest.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/z_globals.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_COMPONENT_OTHER_VM_TEST(P13Heuristics, FragmentationBudget)
{
    // ZGC z_globals.hpp:40 and zHeuristics.cpp:110-112.
    GC_EXPECT_EQ(ZFragmentationLimit, 5.0);
    ZHeuristics::set_max_heap_size(1000000);
    const size_t budget = ZHeuristics::significant_heap_overhead();
    std::fprintf(stderr, "FRAGMENTATION_BUDGET actual=%zu expected=50000\n", budget);
    GC_EXPECT_EQ(budget, size_t{50000});
    ZHeuristics::set_max_heap_size(1999);
    GC_EXPECT_EQ(ZHeuristics::significant_heap_overhead(), size_t{99});
}

GC_TEST(P05Heuristics, MediumPageSizeFromHeap)
{
    ZHeuristics::set_max_heap_size(256ull * 1024 * 1024);
    ZHeuristics::set_medium_page_size();
    GC_EXPECT_TRUE(ZPageSizeMediumEnabled);
    GC_EXPECT_EQ(ZObjectSizeLimitMedium, ZPageSizeMediumMax / 8);
    GC_EXPECT_TRUE(ZPageSizeMediumMax > ZPageSizeSmall);
    GC_EXPECT_TRUE(ZHeuristics::nparallel_workers() >= 1u);
    GC_EXPECT_TRUE(ZHeuristics::nconcurrent_workers() >= 1u);
}

GC_TEST(P05Heuristics, ZPageAllocationIsStackRequest)
{
    // ZGC zPageAllocator.cpp:433-434: requests snapshot both initialized generations.
    (void)Heap::GetHeap();
    ZPageAllocation request(4096, 0, true, true);
    GC_EXPECT_EQ(request.GetSize(), size_t{4096});
    request.Satisfy(true);
    GC_EXPECT_TRUE(request.Wait());
}
