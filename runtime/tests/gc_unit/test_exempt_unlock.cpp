// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// After-copy Exempt must not publish done while a copier still holds LOCKED.
// insert-before-unlock (MutatorRelocate.h:124, WCollector.cpp:10055-10075).
// ZGC: zRelocate.cpp:1041-1047; zRelocationSet.cpp:91-96.

#include <atomic>
#include <chrono>
#include <thread>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/Allocator/RegionManager.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

struct ExemptUnlockTestAccess {
    static bool OnUnmovable(RegionManager& manager, const RegionInfo* region)
    {
        bool found = false;
        manager.unmovableFromRegionList.VisitAllRegions([&found, region](RegionInfo* r) {
            if (r == region) {
                found = true;
            }
        });
        return found;
    }
};

} // namespace MapleRuntime

GC_TEST(ExemptLife, ExemptWaitsForLockedThenPublishesDone)
{
    GcHeapFixture fx;
    RegionManager manager;

    BaseObject* obj = fx.PlaceObject(fx.region0->GetRegionStart());
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj) + 64);
    fx.region0->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    obj->SetStateCode(ObjectState::LOCKED);
    GC_EXPECT_TRUE(obj->GetStateWord().IsLockedWord());

    std::atomic<int> phase{ 0 };
    std::thread copier([&]() {
        phase.store(1, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        obj->UnlockObject(ObjectState::FORWARDED);
        phase.store(2, std::memory_order_release);
    });
    while (phase.load(std::memory_order_acquire) < 1) {
        std::this_thread::yield();
    }
    manager.ExemptFromRegion(fx.region0);
    copier.join();

    GC_EXPECT_FALSE(obj->GetStateWord().IsLockedWord());
    GC_EXPECT_TRUE(obj->IsForwarded());
    GC_EXPECT_TRUE(fx.region0->IsForwardingDone());
    GC_EXPECT_TRUE(ExemptUnlockTestAccess::OnUnmovable(manager, fx.region0));
    GC_EXPECT_EQ(phase.load(std::memory_order_acquire), 2);
}

GC_TEST(ExemptLife, ExemptAlreadyForwardedStillPublishesDone)
{
    GcHeapFixture fx;
    RegionManager manager;
    BaseObject* obj = fx.PlaceObject(fx.region0->GetRegionStart());
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj) + 64);
    obj->SetStateCode(ObjectState::FORWARDED);
    manager.ExemptFromRegion(fx.region0);
    GC_EXPECT_TRUE(obj->IsForwarded());
    GC_EXPECT_TRUE(fx.region0->IsForwardingDone());
}

GC_TEST(ExemptLife, PrepareInstallStripsForwardedResidual)
{
    // CSet empty-select still needs FORWARDED headers; strip only at the next
    // install after the table is retired (zRelocationSet.cpp:91-96).
    GcHeapFixture fx;
    BaseObject* obj = fx.PlaceObject(fx.region0->GetRegionStart());
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj) + 64);
    obj->SetStateCode(ObjectState::FORWARDED);
    GC_EXPECT_TRUE(obj->IsForwarded());
    fx.region0->ClearRelocationResiduals();
    GC_EXPECT_FALSE(obj->IsForwarded());
}

GC_TEST(ExemptLife, PrepareInstallLeavesLockedAlone)
{
    GcHeapFixture fx;
    BaseObject* obj = fx.PlaceObject(fx.region0->GetRegionStart());
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj) + 64);
    obj->SetStateCode(ObjectState::LOCKED);
    fx.region0->ClearRelocationResiduals();
    GC_EXPECT_TRUE(obj->GetStateWord().IsLockedWord());
}
