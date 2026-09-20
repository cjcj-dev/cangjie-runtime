#include <cstdlib>

#include "Heap/shared/collectedHeap.hpp"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(FillerObj, EnabledWalkCrossesFilledGap)
{
    GcHeapFixture fx;
    MAddress start = fx.region0->GetRegionStart();
    BaseObject* a = fx.PlaceObject(start);
    uintptr_t gap = start + 16;
    BaseObject* b = fx.PlaceObject(gap + 64);
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(b) + 16);
    CollectedHeap::fill_with_dummy_object(gap, gap + 64, true);
    // Check the product result before the heap walk consumes its object header.
    GC_EXPECT_TRUE(CollectedHeap::is_filler_object(reinterpret_cast<BaseObject*>(gap)));
    BaseObject* seen[8] = {};
    size_t n = 0;
    fx.region0->VisitAllObjects([&](BaseObject* o) {
        if (n < 8) {
            seen[n] = o;
        }
        ++n;
    });
    GC_EXPECT_EQ(n, static_cast<size_t>(3));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(seen[0]), reinterpret_cast<uintptr_t>(a));
    GC_EXPECT_TRUE(CollectedHeap::is_filler_object(seen[1]));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(seen[2]), reinterpret_cast<uintptr_t>(b));
}

GC_TEST(FillerObj, RouteReserveGapWalkableWhenEnabled)
{
    GcHeapFixture fx;
    MAddress start = fx.region0->GetRegionStart();
    BaseObject* a = fx.PlaceObject(start);
    uintptr_t gap = start + 16;
    constexpr size_t kReserve = 256;
    BaseObject* b = fx.PlaceObject(gap + kReserve);
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(b) + 16);
    CollectedHeap::fill_with_dummy_object(gap, gap + kReserve, true);
    size_t n = 0;
    fx.region0->VisitAllObjects([&](BaseObject*) { ++n; });
    GC_EXPECT_EQ(n, static_cast<size_t>(3));
    (void)a;
}
