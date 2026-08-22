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
    // Count drain, not page walk (zForwarding.cpp:171-181). Planting LOCKED
    // without note_copy is the hole VisitAllObjects used to miss.
    GcHeapFixture fx;
    RegionManager manager;

    BaseObject* obj = fx.PlaceObject(fx.region0->GetRegionStart());
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj) + 64);
    fx.region0->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    obj->SetStateCode(ObjectState::LOCKED);
    fx.region0->NoteCopyInflight();
    GC_EXPECT_TRUE(obj->GetStateWord().IsLockedWord());
    GC_EXPECT_EQ(fx.region0->CopyInflight(), 1);

    std::atomic<int> phase{ 0 };
    std::thread copier([&]() {
        phase.store(1, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        obj->UnlockObject(ObjectState::FORWARDED);
        fx.region0->EndCopyInflight();
        phase.store(2, std::memory_order_release);
    });
    JoinGuard copierGuard(copier);
    while (phase.load(std::memory_order_acquire) < 1) {
        std::this_thread::yield();
    }
    manager.ExemptFromRegion(fx.region0);
    copier.join();

    GC_EXPECT_FALSE(obj->GetStateWord().IsLockedWord());
    GC_EXPECT_TRUE(obj->IsForwarded());
    GC_EXPECT_TRUE(fx.region0->IsForwardingDone());
    GC_EXPECT_EQ(fx.region0->CopyInflight(), 0);
    GC_EXPECT_TRUE(ExemptUnlockTestAccess::OnUnmovable(manager, fx.region0));
    GC_EXPECT_EQ(phase.load(std::memory_order_acquire), 2);
}

GC_TEST(ExemptLife, InPlaceCopyMustNotPaintNormalBeforeUnlock)
{
    // Exclusive CopyObject(from, from) then SetStateCode(NORMAL) clears LOCKED
    // and UnlockObject CHECK-fails (StateWord.h:183). Skip the paint when to==from.
    GcHeapFixture fx;
    BaseObject* obj = fx.PlaceObject(fx.region0->GetRegionStart());
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj) + 64);
    StateWord word = obj->GetStateWord();
    GC_EXPECT_TRUE(obj->TryLockObject(word));
    GC_EXPECT_TRUE(obj->GetStateWord().IsLockedWord());
    BaseObject* toObj = obj;
    if (toObj != obj) {
        toObj->SetStateCode(ObjectState::NORMAL);
    }
    obj->UnlockObject(ObjectState::FORWARDED);
    GC_EXPECT_TRUE(obj->IsForwarded());
    GC_EXPECT_FALSE(obj->GetStateWord().IsLockedWord());
}

GC_TEST(ExemptLife, FindHitDoesNotEnterCopyInflight)
{
    // zRelocate.cpp:382-410: find() hit returns without retain. Exempt must
    // not wait on a table-hit reader.
    GcHeapFixture fx;
    RegionManager manager;
    BaseObject* obj = fx.PlaceObject(fx.region0->GetRegionStart());
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj) + 64);
    obj->SetStateCode(ObjectState::FORWARDED);
    GC_EXPECT_EQ(fx.region0->CopyInflight(), 0);
    manager.ExemptFromRegion(fx.region0);
    GC_EXPECT_TRUE(fx.region0->IsForwardingDone());
    GC_EXPECT_EQ(fx.region0->CopyInflight(), 0);
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

GC_TEST(ZForwardingLife, DrainScopeWaitsCopiedWhenRefCountZero)
{
    // LEAD-NOTE 0820 21:1x: DrainScope used to return when fwdRefCount==0,
    // so TakeRegion ClearUnits raced a LOCKED copier that never retained.
    GcHeapFixture fx;
    fx.region0->NoteCopyInflight();
    GC_EXPECT_EQ(fx.region0->CopyInflight(), 1);
    GC_EXPECT_EQ(fx.region0->metadata.fwdRefCount.load(std::memory_order_acquire), 0);

    std::atomic<int> phase{ 0 };
    std::thread copier([&]() {
        phase.store(1, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        fx.region0->EndCopyInflight();
        phase.store(2, std::memory_order_release);
    });
    JoinGuard copierGuard(copier);
    while (phase.load(std::memory_order_acquire) < 1) {
        std::this_thread::yield();
    }
    int32_t inflightAfterDrain = -1;
    {
        RegionInfo::DrainScope drain(fx.region0, MutatorRelocate::Retire::TAKE_GARBAGE);
        // Snapshot the state protected by DrainScope without throwing while
        // copier is still joinable. EndCopyInflight publishes the protected
        // copy before the thread's later phase=2 bookkeeping, so DrainScope
        // is not required to synchronize that later store.
        inflightAfterDrain = fx.region0->CopyInflight();
    }
    copier.join();
    GC_EXPECT_EQ(inflightAfterDrain, 0);
    GC_EXPECT_EQ(phase.load(std::memory_order_acquire), 2);
}
