// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Raw-pointer pin regression: a native-held object without a major mark must not
// be reclaimed from a pinned region.  The fixture uses product RegionManager
// collection paths and Future-sized objects; only region/list setup is injected.

#include <cstddef>
#include <cstdio>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/Allocator/RegionManager.h"
#include "Sync/Sync.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

struct PinRootTestAccess {
    static void MakeOldPinned(RegionManager& manager, RegionInfo* region)
    {
        manager.oldPinnedRegionList.PrependRegion(region, RegionInfo::RegionType::FULL_PINNED_REGION);
    }

    // Reuses the friendship this file already has rather than adding another one to the product
    // header: the region lists are private, and a second friend declaration is a permanent change
    // to RegionManager for the sake of one test.
    static void ParkOnThreadLocal(RegionManager& manager, RegionInfo* region)
    {
        manager.tlRegionList.PrependRegion(region, RegionInfo::RegionType::THREAD_LOCAL_REGION);
    }
    static bool OnRecentFull(RegionManager& manager, const RegionInfo* region)
    {
        bool found = false;
        manager.recentFullRegionList.VisitAllRegions([&found, region](RegionInfo* r) {
            if (r == region) {
                found = true;
            }
        });
        return found;
    }
    static bool OnThreadLocal(RegionManager& manager, const RegionInfo* region)
    {
        bool found = false;
        manager.tlRegionList.VisitAllRegions([&found, region](RegionInfo* r) {
            if (r == region) {
                found = true;
            }
        });
        return found;
    }
};

} // namespace MapleRuntime

namespace {

constexpr size_t kFutureCount = 16;
constexpr size_t kFutureSize = CJFuture::SYNC_OBJECT_SIZE;

struct PinRootResult {
    size_t garbageWhilePinned;
    size_t garbageAfterRemove;
    int32_t countWhilePinned;
    int32_t countAfterRemove;
};

PinRootResult RunPinnedArm()
{
    GcHeapFixture fx;
    fx.typeInfo->SetInstanceSize(static_cast<U32>(kFutureSize - sizeof(void*)));

    RegionInfo* region = fx.region0;
    MAddress start = region->GetRegionStart();
    for (size_t i = 0; i < kFutureCount; ++i) {
        (void)fx.PlaceObject(start + i * kFutureSize);
    }
    region->SetRegionAllocPtr(start + kFutureCount * kFutureSize);

    RegionManager manager;
    PinRootTestAccess::MakeOldPinned(manager, region);
    BaseObject* held = from_region_addr(start);
    manager.AddRawPointerObject(held);
    int32_t countWhilePinned = region->GetRawPointerObjectCount();
    size_t garbageWhilePinned = manager.CollectPinnedGarbage();

    manager.RemoveRawPointerObject(held);
    int32_t countAfterRemove = region->GetRawPointerObjectCount();
    size_t garbageAfterRemove = manager.CollectPinnedGarbage();

    return { garbageWhilePinned, garbageAfterRemove, countWhilePinned, countAfterRemove };
}

} // namespace

GC_TEST(PinRoot, NativeHeldUnmarkedPinnedObjectSurvivesUntilRemove)
{
    PinRootResult result = RunPinnedArm();
    constexpr size_t reclaimableBytes = kFutureCount * kFutureSize;
    constexpr size_t collateralBytes = reclaimableBytes - kFutureSize;

    std::printf("PINROOT_RESULT futures=%zu object_bytes=%zu pinned_regions=1 total_pinned_regions=1 "
                "raw_pinned_region_pct=100 garbage_while_pinned=%zu garbage_after_remove=%zu "
                "retained_bytes=%zu collateral_bytes=%zu count_while_pinned=%d count_after_remove=%d\n",
                kFutureCount, kFutureSize, result.garbageWhilePinned, result.garbageAfterRemove,
                reclaimableBytes, collateralBytes, result.countWhilePinned, result.countAfterRemove);

    GC_EXPECT_EQ(result.countWhilePinned, 1);
    GC_EXPECT_EQ(result.garbageWhilePinned, 0u);
    GC_EXPECT_EQ(result.countAfterRemove, 0);
    GC_EXPECT_EQ(result.garbageAfterRemove, reclaimableBytes);
}

// A region the forward path finishes with in place must still be reachable by a collection-set
// builder afterwards.
//
// ZGC separates the two questions. A page is in _page_table from ZHeap::alloc_page
// (zHeap.cpp:257) until ZHeap::free_page (:277), and select_relocation_set iterates that table
// (zGeneration.cpp:205-212), so whether an allocator currently points at a page has nothing to do
// with whether the collector can still see it. Ours answers both with list membership, and
// compact-in-place used to drop the allocator side without moving the region: CompactRegion ends by
// prepending it to tlRegionList, and AllocBuffer::ClearRegion only nulls tlRegion.
//
// Neither collection-set builder walks tlRegionList -- AssembleSmallGarbageCandidates and
// PrepareYoungGarbageCandidates walk fromRegionList, recentFullRegionList and
// unmovableFromRegionList -- so such a region could never be collected, and never allocated from
// either, since AllocateThreadLocalRegion always takes a fresh one. It was simply retained.
//
// The existing suite could not have caught this: exactly one test in it ever puts a region on a
// list, and it puts it on the list its own walker reads.
GC_TEST(RegionRetirement, CompactInPlaceLeavesRegionOnAListACollectorWalks)
{
    GcHeapFixture fx;
    RegionManager manager;
    RegionInfo* region = fx.region0;

    // Where CompactRegion leaves it, with no AllocBuffer owning it any more.
    PinRootTestAccess::ParkOnThreadLocal(manager, region);
    GC_EXPECT_EQ(static_cast<unsigned>(region->GetRegionType()),
                 static_cast<unsigned>(RegionInfo::RegionType::THREAD_LOCAL_REGION));

    manager.RehomeCompactedInPlaceRegion(region);

    // The invariant: it now sits on a list a collection-set builder reads, typed accordingly.
    GC_EXPECT_EQ(static_cast<unsigned>(region->GetRegionType()),
                 static_cast<unsigned>(RegionInfo::RegionType::RECENT_FULL_REGION));

    GC_EXPECT_TRUE(PinRootTestAccess::OnRecentFull(manager, region));
    GC_EXPECT_TRUE(!PinRootTestAccess::OnThreadLocal(manager, region));
}
