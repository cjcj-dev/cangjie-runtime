#include <cstdlib>

#include "Heap/Allocator/HeapFiller.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(FillerObj, DefaultOffLeavesZeroGapUnwalkable)
{
    unsetenv("CJRT_HEAP_FILLER");
    GcHeapFixture fx;
    MAddress start = fx.region0->GetRegionStart();
    BaseObject* a = fx.PlaceObject(start);
    uintptr_t gap = start + 16;
    BaseObject* b = fx.PlaceObject(gap + 64);
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(b) + 16);
    HeapFiller::ZeroAndFill(gap, 64);
    size_t n = 0;
    fx.region0->VisitAllObjects([&](BaseObject*) { ++n; });
    GC_EXPECT_EQ(n, static_cast<size_t>(1));
    (void)a;
}

GC_TEST(FillerObj, EnabledWalkCrossesFilledGap)
{
    setenv("CJRT_HEAP_FILLER", "1", 1);
    GcHeapFixture fx;
    MAddress start = fx.region0->GetRegionStart();
    BaseObject* a = fx.PlaceObject(start);
    uintptr_t gap = start + 16;
    BaseObject* b = fx.PlaceObject(gap + 64);
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(b) + 16);
    HeapFiller::ZeroAndFill(gap, 64);
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
    GC_EXPECT_TRUE(HeapFiller::IsFiller(seen[1]));
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(seen[2]), reinterpret_cast<uintptr_t>(b));
    unsetenv("CJRT_HEAP_FILLER");
}

GC_TEST(FillerObj, RouteReserveGapWalkableWhenEnabled)
{
    setenv("CJRT_HEAP_FILLER", "1", 1);
    GcHeapFixture fx;
    MAddress start = fx.region0->GetRegionStart();
    BaseObject* a = fx.PlaceObject(start);
    uintptr_t gap = start + 16;
    constexpr size_t kReserve = 256;
    BaseObject* b = fx.PlaceObject(gap + kReserve);
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(b) + 16);
    HeapFiller::ZeroAndFill(gap, kReserve);
    size_t n = 0;
    fx.region0->VisitAllObjects([&](BaseObject*) { ++n; });
    GC_EXPECT_EQ(n, static_cast<size_t>(3));
    (void)a;
    unsetenv("CJRT_HEAP_FILLER");
}
