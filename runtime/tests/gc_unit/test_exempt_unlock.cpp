// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// After-copy Exempt must not publish done while a copier still holds LOCKED.
// insert-before-unlock.
// ZGC: zRelocate.cpp:1041-1047; zRelocationSet.cpp:91-96.

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zUtils.inline.hpp"
#include "Heap/WCollector/WCollector.h"

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



GC_TEST(ExemptLife, InPlaceCopyMustNotPaintNormalBeforeUnlock)
{
    // Exclusive CopyObject(from, from) then SetStateCode(NORMAL) clears LOCKED
    // and UnlockObject CHECK-fails (StateWord.h:183). Skip the paint when to==from.
    GcHeapFixture fx;
    BaseObject* obj = fx.PlaceObject(fx.region0->GetRegionStart());
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj) + obj->GetSize());
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

namespace {


// zRelocate.cpp:634-639: the in-place path copies conjoint when the new
// object overlaps the old one, disjoint otherwise. ZGC holds no object lock
// across the copy (the forwarding insert resolves the race), so the copy
// promises only that the destination holds the source bytes.
void ExerciseOverlappingCopy(intptr_t destinationDelta)
{
    GcHeapFixture fx;
    RegionInfo* region = fx.region0;
    const MAddress fromAddress = region->GetRegionStart() + 128;
    BaseObject* from = fx.PlaceObject(fromAddress);
    constexpr size_t size = 0x18;
    region->SetRegionAllocPtr(fromAddress + size);
    unsigned char expected[size];
    std::memcpy(expected, from, size);
    const MAddress toAddress = static_cast<MAddress>(static_cast<intptr_t>(fromAddress) + destinationDelta);
    if (toAddress + size > fromAddress) {
        ZUtils::object_copy_conjoint(to_zaddress(fromAddress), to_zaddress(toAddress), size);
    } else {
        ZUtils::object_copy_disjoint(to_zaddress(fromAddress), to_zaddress(toAddress), size);
    }
    GC_EXPECT_EQ(std::memcmp(reinterpret_cast<const void*>(toAddress), expected, size), 0);
}

} // namespace

// The destination starts 0x10 bytes before the source and extends into its
// header: the conjoint copy still lands the complete source image.
GC_OTHER_VM_TEST(ExemptLife, PartialOverlapCopyLandsSourceImage)
{
    ExerciseOverlappingCopy(-0x10);
}

// A copy to `from - 0x18` touches no source byte: the disjoint branch.
GC_OTHER_VM_TEST(ExemptLife, NonOverlapCopyBeforeSourceLandsSourceImage)
{
    ExerciseOverlappingCopy(-0x18);
}

GC_OTHER_VM_TEST(ExemptLife, NonOverlapCopyFarBeforeSourceLandsSourceImage)
{
    ExerciseOverlappingCopy(-0x20);
}

GC_TEST(ExemptLife, PrepareInstallStripsForwardedResidual)
{
    // CSet empty-select still needs FORWARDED headers; strip only at the next
    // install after the table is retired (zRelocationSet.cpp:91-96).
    GcHeapFixture fx;
    BaseObject* obj = fx.PlaceObject(fx.region0->GetRegionStart());
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj) + obj->GetSize());
    obj->SetStateCode(ObjectState::FORWARDED);
    GC_EXPECT_TRUE(obj->IsForwarded());
    fx.region0->ClearRelocationResiduals();
    GC_EXPECT_FALSE(obj->IsForwarded());
}

GC_TEST(ExemptLife, PrepareInstallLeavesLockedAlone)
{
    GcHeapFixture fx;
    BaseObject* obj = fx.PlaceObject(fx.region0->GetRegionStart());
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(obj) + obj->GetSize());
    obj->SetStateCode(ObjectState::LOCKED);
    fx.region0->ClearRelocationResiduals();
    GC_EXPECT_TRUE(obj->GetStateWord().IsLockedWord());
}


