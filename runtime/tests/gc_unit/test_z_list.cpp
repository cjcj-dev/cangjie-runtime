// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Semantic port of OpenJDK test_zList.cpp onto the product intrusive
// RegionList/RegionInfo links.

#include "gc_heap_fixture.hpp"
#include "Heap/Allocator/RegionList.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

struct SixRegions {
    explicit SixRegions(GcHeapFixture& fixture)
        : entries{ fixture.region0, fixture.region1, nullptr, nullptr, nullptr, nullptr }
    {
        for (size_t i = 2; i < 6; ++i) {
            entries[i] = RegionInfo::InitRegion(i, 1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
        }
    }

    int id(RegionInfo* region) const
    {
        for (int i = 0; i < 6; ++i) {
            if (entries[i] == region) {
                return i;
            }
        }
        return -1;
    }

    void insert_sorted(RegionList& list)
    {
        for (int i = 5; i >= 0; --i) {
            list.PrependRegion(entries[i], RegionInfo::RegionType::FROM_REGION);
        }
    }

    RegionInfo* entries[6];
};

void AssertSorted(RegionList& list, const SixRegions& regions)
{
    int count = regions.id(list.GetHeadRegion());
    list.VisitAllRegions([&](RegionInfo* entry) {
        GC_EXPECT_EQ(regions.id(entry), count);
        ++count;
    });

    count = regions.id(list.GetTailRegion());
    for (RegionInfo* entry = list.GetTailRegion(); entry != nullptr; entry = entry->GetPrevRegion()) {
        GC_EXPECT_EQ(regions.id(entry), count);
        --count;
    }
}

} // namespace

GC_TEST(ZListPort, InsertAndRemoveFirst)
{
    GcHeapFixture fixture;
    SixRegions regions(fixture);
    RegionList list("zlist-port-insert");
    regions.insert_sorted(list);

    GC_EXPECT_EQ(list.GetRegionCount(), static_cast<size_t>(6));
    AssertSorted(list, regions);
    for (int i = 0; i < 6; ++i) {
        RegionInfo* entry = list.TakeHeadRegion();
        GC_EXPECT_EQ(regions.id(entry), i);
    }
    GC_EXPECT_EQ(list.GetRegionCount(), static_cast<size_t>(0));
}

GC_TEST(ZListPort, RemoveFirstAndLast)
{
    GcHeapFixture fixture;
    SixRegions regions(fixture);

    {
        RegionList list("zlist-port-remove-first");
        regions.insert_sorted(list);
        GC_EXPECT_EQ(list.GetRegionCount(), static_cast<size_t>(6));
        for (int i = 0; i < 6; ++i) {
            RegionInfo* entry = list.TakeHeadRegion();
            GC_EXPECT_EQ(regions.id(entry), i);
        }
        GC_EXPECT_EQ(list.GetRegionCount(), static_cast<size_t>(0));
    }

    {
        RegionList list("zlist-port-remove-last");
        regions.insert_sorted(list);
        GC_EXPECT_EQ(list.GetRegionCount(), static_cast<size_t>(6));
        for (int i = 5; i >= 0; --i) {
            RegionInfo* entry = list.GetTailRegion();
            list.DeleteRegion(entry);
            GC_EXPECT_EQ(regions.id(entry), i);
        }
        GC_EXPECT_EQ(list.GetRegionCount(), static_cast<size_t>(0));
    }
}
