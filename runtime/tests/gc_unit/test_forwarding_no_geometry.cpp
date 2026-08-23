// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Invariant: region in the forward set, object not copied → LookupTo is null.
// Geometry GetRoute may still invent a to; the table must not.

#include "Heap/Allocator/ForwardingTable.h"
#include "gc_heap_fixture.hpp"
#include "Heap/WCollector/WCollector.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace MapleRuntime {

#if defined(MRT_TESTABLE_INTERNALS)
// Access only: the exercised implementation remains the product SO's
// WCollector::ForwardObjectExclusive, including its real Publish call.
struct MutatorPublishTestAccess {
    static BaseObject* ForwardExclusive(
        WCollector& collector, BaseObject* from, BaseObject* to, RegionInfo* copyPage)
    {
        return collector.ForwardObjectExclusive(from, to, copyPage);
    }
};
#endif

} // namespace MapleRuntime

GC_TEST(ForwardingNoGeometry, ArmedLookupAndSuccessfulExclusiveCopyPublishProductReceipt)
{
    static_assert(ForwardingTable::kEntriesSoleWhenArmed, "step 3 requires sole-when-armed");

    GcHeapFixture fx;
    ForwardingTable::Initialize(fx.heapStart, 2 * RegionInfo::UNIT_SIZE, RegionInfo::UNIT_SIZE);
    fx.region0->SetRegionType(RegionInfo::RegionType::FROM_REGION);
    fx.region0->SetRouteState(RegionInfo::ROUTED);
    fx.region0->SetRouteInfo(0x20000000u, 4096);

    LiveInfo* live = fx.PlantLiveInfo(fx.region0);
    size_t regionSize = fx.region0->GetRegionSize();
    RegionBitmap* bm = fx.PlantMarkBitmap(live, regionSize);
    size_t offset = fx.region0->GetAddressOffset(reinterpret_cast<MAddress>(fx.obj0));
    (void)bm->MarkBits(offset, 8, regionSize);
    fx.region0->BindLiveInfo0FromLiveIfNull();

    const MAddress from = reinterpret_cast<MAddress>(fx.obj0);
    GC_EXPECT_TRUE(ForwardingTable::EntriesArmed(from));
    GC_EXPECT_TRUE(ForwardingTable::GetEntries(from)->is_provisional());

    BaseObject* geometric = fx.region0->GetRouteForProbe(fx.obj0);
    GC_EXPECT_TRUE(geometric != nullptr);
    GC_EXPECT_TRUE(reinterpret_cast<MAddress>(geometric) == 0x20000000u ||
                   reinterpret_cast<MAddress>(geometric) != 0);

    ForwardingTable::ToAnswer ans = ForwardingTable::ToAnswer::Unarmed;
    const MAddress looked = ForwardingTable::LookupTo(from, &ans);
    GC_EXPECT_TRUE(ans == ForwardingTable::ToAnswer::ArmedMiss);
    GC_EXPECT_EQ(looked, static_cast<MAddress>(0));
    GC_EXPECT_TRUE(looked != reinterpret_cast<MAddress>(geometric));

    const MAddress stored = fx.heapStart + RegionInfo::UNIT_SIZE + 128;
    GC_EXPECT_EQ(ForwardingTable::InsertMapping(from, stored), stored);
    GC_EXPECT_FALSE(ForwardingTable::GetEntries(from)->is_provisional());
    ans = ForwardingTable::ToAnswer::Unarmed;
    GC_EXPECT_EQ(ForwardingTable::LookupTo(from, &ans), stored);
    GC_EXPECT_TRUE(ans == ForwardingTable::ToAnswer::ArmedHit);

#if defined(MRT_TESTABLE_INTERNALS)
    // Drive the product success path itself: CopyObject -> InstallMapping ->
    // RelocationRequestQueue::Publish -> UnlockObject(FORWARDED). This is the
    // call site at Relocate.cpp in WCollector::ForwardObjectExclusive, not a
    // queue helper with a hand-fed receipt.
    RegionSpace& productSpace = reinterpret_cast<RegionSpace&>(Heap::GetHeap().GetAllocator());
    RelocationRequestQueue& requests = productSpace.GetRegionManager().GetRelocationRequestQueue();
    requests.BeginCycle();
    BaseObject* copyFrom = fx.PlaceObject(fx.region0->GetRegionStart() + 128);
    BaseObject* copyTo = fx.PlaceObject(fx.region1->GetRegionStart() + 128);
    fx.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(copyFrom) + 64);
    fx.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(copyTo) + 64);
    const MAddress copyFromAddr = reinterpret_cast<MAddress>(copyFrom);
    const MAddress copyToAddr = reinterpret_cast<MAddress>(copyTo);
    const auto requested = requests.Add(fx.region0, copyFromAddr);
    GC_EXPECT_TRUE(requested.accepted);

    StateWord oldWord = copyFrom->GetStateWord();
    GC_EXPECT_TRUE(copyFrom->TryLockObject(oldWord));
    fx.region0->NoteCopyInflight();
    WCollector collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources());
    BaseObject* relocated = MutatorPublishTestAccess::ForwardExclusive(
        collector, copyFrom, copyTo, fx.region0);

    const bool productPublished =
        requested.request->state() == RelocationRequestQueue::State::COMPLETED;
    GC_EXPECT_TRUE(productPublished);
    if (!productPublished) {
        // A disconnected product call must report this test, not stall the
        // runner while the request remains queued.
        (void)requests.Fail(copyFromAddr);
    }
    GC_EXPECT_TRUE(relocated == copyTo);
    GC_EXPECT_EQ(requested.request->receipt(), copyToAddr);
    GC_EXPECT_EQ(requests.Wait(requested.request), copyToAddr);
    GC_EXPECT_TRUE(copyFrom->IsForwarded());
    GC_EXPECT_EQ(fx.region0->CopyInflight(), 0);
#endif

    fx.region0->SetRouteState(RegionInfo::NORMAL);
    fx.region0->RetireFromPageMetadata();
    fx.region0->metadata.liveInfo = nullptr;
    fx.FreePlanted(live);
}
