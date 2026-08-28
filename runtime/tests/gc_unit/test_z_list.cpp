// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Semantic port of OpenJDK test_zList.cpp onto the product intrusive
// RegionList/RegionInfo links.

#include "gc_heap_fixture.hpp"
#include "Heap/Allocator/RegionList.h"
#include "gc_unittest.hpp"

#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

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
        GC_EXPECT_TRUE(entry->GetRegionListOwner() == &list);
        ++count;
    });

    count = regions.id(list.GetTailRegion());
    for (RegionInfo* entry = list.GetTailRegion(); entry != nullptr; entry = entry->GetPrevRegion()) {
        GC_EXPECT_EQ(regions.id(entry), count);
        --count;
    }
}

} // namespace

#if defined(__linux__)
template <typename Fn>
void ExpectListAbort(Fn&& fn)
{
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        fn();
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
}
#endif

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
        GC_EXPECT_TRUE(entry->GetRegionListOwner() == nullptr);
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
            GC_EXPECT_TRUE(entry->GetRegionListOwner() == nullptr);
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
            GC_EXPECT_TRUE(entry->GetRegionListOwner() == nullptr);
        }
        GC_EXPECT_EQ(list.GetRegionCount(), static_cast<size_t>(0));
    }
}

GC_TEST(ZListAuthority, DoublePrependIsRejected)
{
#if defined(__linux__)
    GcHeapFixture fixture;
    ExpectListAbort([&]() {
        RegionList list("zlist-double-prepend");
        list.PrependRegion(fixture.region0, RegionInfo::RegionType::FROM_REGION);
        list.PrependRegion(fixture.region0, RegionInfo::RegionType::FROM_REGION);
    });
#endif
}

GC_TEST(ZListAuthority, WrongListRemoveIsRejected)
{
#if defined(__linux__)
    GcHeapFixture fixture;
    ExpectListAbort([&]() {
        RegionList owner("zlist-owner");
        RegionList other("zlist-other");
        owner.PrependRegion(fixture.region0, RegionInfo::RegionType::FROM_REGION);
        other.PrependRegion(fixture.region1, RegionInfo::RegionType::FROM_REGION);
        other.DeleteRegion(fixture.region0);
    });
#endif
}

GC_TEST(ZListAuthority, RemoveThenInsertTransfersAuthority)
{
    GcHeapFixture fixture;
    RegionList first("zlist-first");
    RegionList second("zlist-second");
    first.PrependRegion(fixture.region0, RegionInfo::RegionType::FROM_REGION);
    GC_EXPECT_TRUE(fixture.region0->GetRegionListOwner() == &first);
    first.DeleteRegion(fixture.region0);
    GC_EXPECT_TRUE(fixture.region0->GetRegionListOwner() == nullptr);
    GC_EXPECT_TRUE(fixture.region0->GetPrevRegion() == nullptr);
    GC_EXPECT_TRUE(fixture.region0->GetNextRegion() == nullptr);
    second.PrependRegion(fixture.region0, RegionInfo::RegionType::GARBAGE_REGION);
    GC_EXPECT_TRUE(fixture.region0->GetRegionListOwner() == &second);
    GC_EXPECT_EQ(second.GetRegionCount(), static_cast<size_t>(1));
}

GC_TEST(ZListAuthority, GhostSnapshotResetDoesNotOwnRegion)
{
    GcHeapFixture fixture;
    RegionList authority("zlist-authority");
    RegionList ghost("zlist-ghost");
    authority.PrependRegion(fixture.region0, RegionInfo::RegionType::FROM_REGION);
    authority.CopyListTo(ghost);
    GC_EXPECT_TRUE(fixture.region0->GetRegionListOwner() == &authority);
    fixture.region0->metadata.nextRegionIdx0 = static_cast<uint32_t>(fixture.region1->GetUnitIdx());
    GC_EXPECT_TRUE(fixture.region0->GetNextGhostRegion() == fixture.region1);
    authority.DeleteRegion(fixture.region0);
    GC_EXPECT_TRUE(ghost.GetHeadRegion() == fixture.region0);
    GC_EXPECT_TRUE(fixture.region0->GetRegionListOwner() == nullptr);
    fixture.region0->InitRegionInfo(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    GC_EXPECT_TRUE(fixture.region0->GetNextGhostRegion() == nullptr);
    GC_EXPECT_TRUE(fixture.region0->GetRegionListOwner() == nullptr);
}
