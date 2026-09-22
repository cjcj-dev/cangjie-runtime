#include "gc_unittest.hpp"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "Heap/z/zPageAllocator.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

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
    ZPageAllocation request(4096, 0, true);
    GC_EXPECT_EQ(request.GetSize(), size_t{4096});
    request.Satisfy(true);
    GC_EXPECT_TRUE(request.Wait());
}
