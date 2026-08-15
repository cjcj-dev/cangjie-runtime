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
